// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2007 Oracle.  All rights reserved.
 */

#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/time.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/backing-dev.h>
#include <linux/falloc.h>
#include <linux/writeback.h>
#include <linux/compat.h>
#include <linux/slab.h>
#include <linux/btrfs.h>
#include <linux/uio.h>
#include <linux/iversion.h>
#include <linux/fsverity.h>
#include "ctree.h"
#include "direct-io.h"
#include "disk-io.h"
#include "transaction.h"
#include "btrfs_inode.h"
#include "tree-log.h"
#include "locking.h"
#include "qgroup.h"
#include "compression.h"
#include "delalloc-space.h"
#include "reflink.h"
#include "subpage.h"
#include "fs.h"
#include "accessors.h"
#include "extent-tree.h"
#include "file-item.h"
#include "ioctl.h"
#include "file.h"
#include "super.h"
#include "print-tree.h"

/*
 * Unlock folio after btrfs_file_write() is done with it.
 */
static void btrfs_drop_folio(struct btrfs_fs_info *fs_info, struct folio *folio,
			     u64 pos, u64 copied)
{
	u64 block_start = round_down(pos, fs_info->sectorsize);
	u64 block_len = round_up(pos + copied, fs_info->sectorsize) - block_start;

	ASSERT(block_len <= U32_MAX);
	/*
	 * Folio checked is some magic around finding folios that have been
	 * modified without going through btrfs_dirty_folio().  Clear it here.
	 * There should be no need to mark the pages accessed as
	 * prepare_one_folio() should have marked them accessed in
	 * prepare_one_folio() via find_or_create_page()
	 */
	btrfs_folio_clamp_clear_checked(fs_info, folio, block_start, block_len);
	folio_unlock(folio);
	folio_put(folio);
}

/*
 * After copy_folio_from_iter_atomic(), update the following things for delalloc:
 * - Mark newly dirtied folio as DELALLOC in the io tree.
 *   Used to advise which range is to be written back.
 * - Mark modified folio as Uptodate/Dirty and not needing COW fixup
 * - Update inode size for past EOF write
 */
int btrfs_dirty_folio(struct btrfs_inode *inode, struct folio *folio, loff_t pos,
		      size_t write_bytes, struct extent_state **cached, bool noreserve)
{
	struct btrfs_fs_info *fs_info = inode->root->fs_info;
	int ret = 0;
	u64 num_bytes;
	u64 start_pos;
	u64 end_of_last_block;
	u64 end_pos = pos + write_bytes;
	loff_t isize = i_size_read(&inode->vfs_inode);
	unsigned int extra_bits = 0;

	if (write_bytes == 0)
		return 0;

	if (noreserve)
		extra_bits |= EXTENT_NORESERVE;

	start_pos = round_down(pos, fs_info->sectorsize);
	num_bytes = round_up(write_bytes + pos - start_pos,
			     fs_info->sectorsize);
	ASSERT(num_bytes <= U32_MAX);
	ASSERT(folio_pos(folio) <= pos && folio_end(folio) >= pos + write_bytes);

	end_of_last_block = start_pos + num_bytes - 1;

	/*
	 * The pages may have already been dirty, clear out old accounting so
	 * we can set things up properly
	 */
	btrfs_clear_extent_bit(&inode->io_tree, start_pos, end_of_last_block,
			       EXTENT_DELALLOC | EXTENT_DO_ACCOUNTING | EXTENT_DEFRAG,
			       cached);

	ret = btrfs_set_extent_delalloc(inode, start_pos, end_of_last_block,
					extra_bits, cached);
	if (ret)
		return ret;

	btrfs_folio_clamp_set_uptodate(fs_info, folio, start_pos, num_bytes);
	btrfs_folio_clamp_clear_checked(fs_info, folio, start_pos, num_bytes);
	btrfs_folio_clamp_set_dirty(fs_info, folio, start_pos, num_bytes);

	/*
	 * we've only changed i_size in ram, and we haven't updated
	 * the disk i_size.  There is no need to log the inode
	 * at this time.
	 */
	if (end_pos > isize)
		i_size_write(&inode->vfs_inode, end_pos);
	return 0;
}

/*
 * this is very complex, but the basic idea is to drop all extents
 * in the range start - end.  hint_block is filled in with a block number
 * that would be a good hint to the block allocator for this file.
 *
 * If an extent intersects the range but is not entirely inside the range
 * it is either truncated or split.  Anything entirely inside the range
 * is deleted from the tree.
 *
 * Note: the VFS' inode number of bytes is not updated, it's up to the caller
 * to deal with that. We set the field 'bytes_found' of the arguments structure
 * with the number of allocated bytes found in the target range, so that the
 * caller can update the inode's number of bytes in an atomic way when
 * replacing extents in a range to avoid races with stat(2).
 */
int btrfs_drop_extents(struct btrfs_trans_handle *trans,
		       struct btrfs_root *root, struct btrfs_inode *inode,
		       struct btrfs_drop_extents_args *args)
{
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct extent_buffer *leaf;
	struct btrfs_file_extent_item *fi;
	struct btrfs_key key;
	struct btrfs_key new_key;
	u64 ino = btrfs_ino(inode);
	u64 search_start = args->start;
	u64 disk_bytenr = 0;
	u64 num_bytes = 0;
	u64 extent_offset = 0;
	u64 extent_end = 0;
	u64 last_end = args->start;
	int del_nr = 0;
	int del_slot = 0;
	int extent_type;
	int recow;
	int ret;
	int modify_tree = -1;
	int update_refs;
	int found = 0;
	struct btrfs_path *path = args->path;

	args->bytes_found = 0;
	args->extent_inserted = false;

	/* Must always have a path if ->replace_extent is true */
	ASSERT(!(args->replace_extent && !args->path));

	if (!path) {
		path = btrfs_alloc_path();
		if (!path) {
			ret = -ENOMEM;
			goto out;
		}
	}

	if (args->drop_cache)
		btrfs_drop_extent_map_range(inode, args->start, args->end - 1, false);

	if (data_race(args->start >= inode->disk_i_size) && !args->replace_extent)
		modify_tree = 0;

	update_refs = (btrfs_root_id(root) != BTRFS_TREE_LOG_OBJECTID);
	while (1) {
		recow = 0;
		ret = btrfs_lookup_file_extent(trans, root, path, ino,
					       search_start, modify_tree);
		if (ret < 0)
			break;
		if (ret > 0 && path->slots[0] > 0 && search_start == args->start) {
			leaf = path->nodes[0];
			btrfs_item_key_to_cpu(leaf, &key, path->slots[0] - 1);
			if (key.objectid == ino &&
			    key.type == BTRFS_EXTENT_DATA_KEY)
				path->slots[0]--;
		}
		ret = 0;
next_slot:
		leaf = path->nodes[0];
		if (path->slots[0] >= btrfs_header_nritems(leaf)) {
			if (WARN_ON(del_nr > 0)) {
				btrfs_print_leaf(leaf);
				ret = -EINVAL;
				break;
			}
			ret = btrfs_next_leaf(root, path);
			if (ret < 0)
				break;
			if (ret > 0) {
				ret = 0;
				break;
			}
			leaf = path->nodes[0];
			recow = 1;
		}

		btrfs_item_key_to_cpu(leaf, &key, path->slots[0]);

		if (key.objectid > ino)
			break;
		if (WARN_ON_ONCE(key.objectid < ino) ||
		    key.type < BTRFS_EXTENT_DATA_KEY) {
			ASSERT(del_nr == 0);
			path->slots[0]++;
			goto next_slot;
		}
		if (key.type > BTRFS_EXTENT_DATA_KEY || key.offset >= args->end)
			break;

		fi = btrfs_item_ptr(leaf, path->slots[0],
				    struct btrfs_file_extent_item);
		extent_type = btrfs_file_extent_type(leaf, fi);

		if (extent_type == BTRFS_FILE_EXTENT_REG ||
		    extent_type == BTRFS_FILE_EXTENT_PREALLOC) {
			disk_bytenr = btrfs_file_extent_disk_bytenr(leaf, fi);
			num_bytes = btrfs_file_extent_disk_num_bytes(leaf, fi);
			extent_offset = btrfs_file_extent_offset(leaf, fi);
			extent_end = key.offset +
				btrfs_file_extent_num_bytes(leaf, fi);
		} else if (extent_type == BTRFS_FILE_EXTENT_INLINE) {
			extent_end = key.offset +
				btrfs_file_extent_ram_bytes(leaf, fi);
		} else {
			/* can't happen */
			BUG();
		}

		/*
		 * Don't skip extent items representing 0 byte lengths. They
		 * used to be created (bug) if while punching holes we hit
		 * -ENOSPC condition. So if we find one here, just ensure we
		 * delete it, otherwise we would insert a new file extent item
		 * with the same key (offset) as that 0 bytes length file
		 * extent item in the call to setup_items_for_insert() later
		 * in this function.
		 */
		if (extent_end == key.offset && extent_end >= search_start) {
			last_end = extent_end;
			goto delete_extent_item;
		}

		if (extent_end <= search_start) {
			path->slots[0]++;
			goto next_slot;
		}

		found = 1;
		search_start = max(key.offset, args->start);
		if (recow || !modify_tree) {
			modify_tree = -1;
			btrfs_release_path(path);
			continue;
		}

		/*
		 *     | - range to drop - |
		 *  | -------- extent -------- |
		 */
		if (args->start > key.offset && args->end < extent_end) {
			if (WARN_ON(del_nr > 0)) {
				btrfs_print_leaf(leaf);
				ret = -EINVAL;
				break;
			}
			if (extent_type == BTRFS_FILE_EXTENT_INLINE) {
				ret = -EOPNOTSUPP;
				break;
			}

			memcpy(&new_key, &key, sizeof(new_key));
			new_key.offset = args->start;
			ret = btrfs_duplicate_item(trans, root, path,
						   &new_key);
			if (ret == -EAGAIN) {
				btrfs_release_path(path);
				continue;
			}
			if (ret < 0)
				break;

			leaf = path->nodes[0];
			fi = btrfs_item_ptr(leaf, path->slots[0] - 1,
					    struct btrfs_file_extent_item);
			btrfs_set_file_extent_num_bytes(leaf, fi,
							args->start - key.offset);

			fi = btrfs_item_ptr(leaf, path->slots[0],
					    struct btrfs_file_extent_item);

			extent_offset += args->start - key.offset;
			btrfs_set_file_extent_offset(leaf, fi, extent_offset);
			btrfs_set_file_extent_num_bytes(leaf, fi,
							extent_end - args->start);

			if (update_refs && disk_bytenr > 0) {
				struct btrfs_ref ref = {
					.action = BTRFS_ADD_DELAYED_REF,
					.bytenr = disk_bytenr,
					.num_bytes = num_bytes,
					.parent = 0,
					.owning_root = btrfs_root_id(root),
					.ref_root = btrfs_root_id(root),
				};
				btrfs_init_data_ref(&ref, new_key.objectid,
						    args->start - extent_offset,
						    0, false);
				ret = btrfs_inc_extent_ref(trans, &ref);
				if (ret) {
					btrfs_abort_transaction(trans, ret);
					break;
				}
			}
			key.offset = args->start;
		}
		/*
		 * From here on out we will have actually dropped something, so
		 * last_end can be updated.
		 */
		last_end = extent_end;

		/*
		 *  | ---- range to drop ----- |
		 *      | -------- extent -------- |
		 */
		if (args->start <= key.offset && args->end < extent_end) {
			if (extent_type == BTRFS_FILE_EXTENT_INLINE) {
				ret = -EOPNOTSUPP;
				break;
			}

			memcpy(&new_key, &key, sizeof(new_key));
			new_key.offset = args->end;
			btrfs_set_item_key_safe(trans, path, &new_key);

			extent_offset += args->end - key.offset;
			btrfs_set_file_extent_offset(leaf, fi, extent_offset);
			btrfs_set_file_extent_num_bytes(leaf, fi,
							extent_end - args->end);
			if (update_refs && disk_bytenr > 0)
				args->bytes_found += args->end - key.offset;
			break;
		}

		search_start = extent_end;
		/*
		 *       | ---- range to drop ----- |
		 *  | -------- extent -------- |
		 */
		if (args->start > key.offset && args->end >= extent_end) {
			if (WARN_ON(del_nr > 0)) {
				btrfs_print_leaf(leaf);
				ret = -EINVAL;
				break;
			}
			if (extent_type == BTRFS_FILE_EXTENT_INLINE) {
				ret = -EOPNOTSUPP;
				break;
			}

			btrfs_set_file_extent_num_bytes(leaf, fi,
							args->start - key.offset);
			if (update_refs && disk_bytenr > 0)
				args->bytes_found += extent_end - args->start;
			if (args->end == extent_end)
				break;

			path->slots[0]++;
			goto next_slot;
		}

		/*
		 *  | ---- range to drop ----- |
		 *    | ------ extent ------ |
		 */
		if (args->start <= key.offset && args->end >= extent_end) {
delete_extent_item:
			if (del_nr == 0) {
				del_slot = path->slots[0];
				del_nr = 1;
			} else {
				if (WARN_ON(del_slot + del_nr != path->slots[0])) {
					btrfs_print_leaf(leaf);
					ret = -EINVAL;
					break;
				}
				del_nr++;
			}

			if (update_refs &&
			    extent_type == BTRFS_FILE_EXTENT_INLINE) {
				args->bytes_found += extent_end - key.offset;
				extent_end = ALIGN(extent_end,
						   fs_info->sectorsize);
			} else if (update_refs && disk_bytenr > 0) {
				struct btrfs_ref ref = {
					.action = BTRFS_DROP_DELAYED_REF,
					.bytenr = disk_bytenr,
					.num_bytes = num_bytes,
					.parent = 0,
					.owning_root = btrfs_root_id(root),
					.ref_root = btrfs_root_id(root),
				};
				btrfs_init_data_ref(&ref, key.objectid,
						    key.offset - extent_offset,
						    0, false);
				ret = btrfs_free_extent(trans, &ref);
				if (ret) {
					btrfs_abort_transaction(trans, ret);
					break;
				}
				args->bytes_found += extent_end - key.offset;
			}

			if (args->end == extent_end)
				break;

			if (path->slots[0] + 1 < btrfs_header_nritems(leaf)) {
				path->slots[0]++;
				goto next_slot;
			}

			ret = btrfs_del_items(trans, root, path, del_slot,
					      del_nr);
			if (ret) {
				btrfs_abort_transaction(trans, ret);
				break;
			}

			del_nr = 0;
			del_slot = 0;

			btrfs_release_path(path);
			continue;
		}

		BUG();
	}

	if (!ret && del_nr > 0) {
		/*
		 * Set path->slots[0] to first slot, so that after the delete
		 * if items are move off from our leaf to its immediate left or
		 * right neighbor leafs, we end up with a correct and adjusted
		 * path->slots[0] for our insertion (if args->replace_extent).
		 */
		path->slots[0] = del_slot;
		ret = btrfs_del_items(trans, root, path, del_slot, del_nr);
		if (ret)
			btrfs_abort_transaction(trans, ret);
	}

	leaf = path->nodes[0];
	/*
	 * If btrfs_del_items() was called, it might have deleted a leaf, in
	 * which case it unlocked our path, so check path->locks[0] matches a
	 * write lock.
	 */
	if (!ret && args->replace_extent &&
	    path->locks[0] == BTRFS_WRITE_LOCK &&
	    btrfs_leaf_free_space(leaf) >=
	    sizeof(struct btrfs_item) + args->extent_item_size) {

		key.objectid = ino;
		key.type = BTRFS_EXTENT_DATA_KEY;
		key.offset = args->start;
		if (!del_nr && path->slots[0] < btrfs_header_nritems(leaf)) {
			struct btrfs_key slot_key;

			btrfs_item_key_to_cpu(leaf, &slot_key, path->slots[0]);
			if (btrfs_comp_cpu_keys(&key, &slot_key) > 0)
				path->slots[0]++;
		}
		btrfs_setup_item_for_insert(trans, root, path, &key,
					    args->extent_item_size);
		args->extent_inserted = true;
	}

	if (!args->path)
		btrfs_free_path(path);
	else if (!args->extent_inserted)
		btrfs_release_path(path);
out:
	args->drop_end = found ? min(args->end, last_end) : args->end;

	return ret;
}

static bool extent_mergeable(struct extent_buffer *leaf, int slot, u64 objectid,
			     u64 bytenr, u64 orig_offset, u64 *start, u64 *end)
{
	struct btrfs_file_extent_item *fi;
	struct btrfs_key key;
	u64 extent_end;

	if (slot < 0 || slot >= btrfs_header_nritems(leaf))
		return false;

	btrfs_item_key_to_cpu(leaf, &key, slot);
	if (key.objectid != objectid || key.type != BTRFS_EXTENT_DATA_KEY)
		return false;

	fi = btrfs_item_ptr(leaf, slot, struct btrfs_file_extent_item);
	if (btrfs_file_extent_type(leaf, fi) != BTRFS_FILE_EXTENT_REG ||
	    btrfs_file_extent_disk_bytenr(leaf, fi) != bytenr ||
	    btrfs_file_extent_offset(leaf, fi) != key.offset - orig_offset ||
	    btrfs_file_extent_compression(leaf, fi) ||
	    btrfs_file_extent_encryption(leaf, fi) ||
	    btrfs_file_extent_other_encoding(leaf, fi))
		return false;

	extent_end = key.offset + btrfs_file_extent_num_bytes(leaf, fi);
	if ((*start && *start != key.offset) || (*end && *end != extent_end))
		return false;

	*start = key.offset;
	*end = extent_end;
	return true;
}

/*
 * Mark extent in the range start - end as written.
 *
 * This changes extent type from 'pre-allocated' to 'regular'. If only
 * part of extent is marked as written, the extent will be split into
 * two or three.
 */
int btrfs_mark_extent_written(struct btrfs_trans_handle *trans,
			      struct btrfs_inode *inode, u64 start, u64 end)
{
	struct btrfs_root *root = inode->root;
	struct extent_buffer *leaf;
	BTRFS_PATH_AUTO_FREE(path);
	struct btrfs_file_extent_item *fi;
	struct btrfs_ref ref = { 0 };
	struct btrfs_key key;
	struct btrfs_key new_key;
	u64 bytenr;
	u64 num_bytes;
	u64 extent_end;
	u64 orig_offset;
	u64 other_start;
	u64 other_end;
	u64 split;
	int del_nr = 0;
	int del_slot = 0;
	int recow;
	int ret = 0;
	u64 ino = btrfs_ino(inode);

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;
again:
	recow = 0;
	split = start;
	key.objectid = ino;
	key.type = BTRFS_EXTENT_DATA_KEY;
	key.offset = split;

	ret = btrfs_search_slot(trans, root, &key, path, -1, 1);
	if (ret < 0)
		goto out;
	if (ret > 0 && path->slots[0] > 0)
		path->slots[0]--;

	leaf = path->nodes[0];
	btrfs_item_key_to_cpu(leaf, &key, path->slots[0]);
	if (key.objectid != ino ||
	    key.type != BTRFS_EXTENT_DATA_KEY) {
		ret = -EINVAL;
		btrfs_abort_transaction(trans, ret);
		goto out;
	}
	fi = btrfs_item_ptr(leaf, path->slots[0],
			    struct btrfs_file_extent_item);
	if (btrfs_file_extent_type(leaf, fi) != BTRFS_FILE_EXTENT_PREALLOC) {
		ret = -EINVAL;
		btrfs_abort_transaction(trans, ret);
		goto out;
	}
	extent_end = key.offset + btrfs_file_extent_num_bytes(leaf, fi);
	if (key.offset > start || extent_end < end) {
		ret = -EINVAL;
		btrfs_abort_transaction(trans, ret);
		goto out;
	}

	bytenr = btrfs_file_extent_disk_bytenr(leaf, fi);
	num_bytes = btrfs_file_extent_disk_num_bytes(leaf, fi);
	orig_offset = key.offset - btrfs_file_extent_offset(leaf, fi);
	memcpy(&new_key, &key, sizeof(new_key));

	if (start == key.offset && end < extent_end) {
		other_start = 0;
		other_end = start;
		if (extent_mergeable(leaf, path->slots[0] - 1,
				     ino, bytenr, orig_offset,
				     &other_start, &other_end)) {
			new_key.offset = end;
			btrfs_set_item_key_safe(trans, path, &new_key);
			fi = btrfs_item_ptr(leaf, path->slots[0],
					    struct btrfs_file_extent_item);
			btrfs_set_file_extent_generation(leaf, fi,
							 trans->transid);
			btrfs_set_file_extent_num_bytes(leaf, fi,
							extent_end - end);
			btrfs_set_file_extent_offset(leaf, fi,
						     end - orig_offset);
			fi = btrfs_item_ptr(leaf, path->slots[0] - 1,
					    struct btrfs_file_extent_item);
			btrfs_set_file_extent_generation(leaf, fi,
							 trans->transid);
			btrfs_set_file_extent_num_bytes(leaf, fi,
							end - other_start);
			goto out;
		}
	}

	if (start > key.offset && end == extent_end) {
		other_start = end;
		other_end = 0;
		if (extent_mergeable(leaf, path->slots[0] + 1,
				     ino, bytenr, orig_offset,
				     &other_start, &other_end)) {
			fi = btrfs_item_ptr(leaf, path->slots[0],
					    struct btrfs_file_extent_item);
			btrfs_set_file_extent_num_bytes(leaf, fi,
							start - key.offset);
			btrfs_set_file_extent_generation(leaf, fi,
							 trans->transid);
			path->slots[0]++;
			new_key.offset = start;
			btrfs_set_item_key_safe(trans, path, &new_key);

			fi = btrfs_item_ptr(leaf, path->slots[0],
					    struct btrfs_file_extent_item);
			btrfs_set_file_extent_generation(leaf, fi,
							 trans->transid);
			btrfs_set_file_extent_num_bytes(leaf, fi,
							other_end - start);
			btrfs_set_file_extent_offset(leaf, fi,
						     start - orig_offset);
			goto out;
		}
	}

	while (start > key.offset || end < extent_end) {
		if (key.offset == start)
			split = end;

		new_key.offset = split;
		ret = btrfs_duplicate_item(trans, root, path, &new_key);
		if (ret == -EAGAIN) {
			btrfs_release_path(path);
			goto again;
		}
		if (ret < 0) {
			btrfs_abort_transaction(trans, ret);
			goto out;
		}

		leaf = path->nodes[0];
		fi = btrfs_item_ptr(leaf, path->slots[0] - 1,
				    struct btrfs_file_extent_item);
		btrfs_set_file_extent_generation(leaf, fi, trans->transid);
		btrfs_set_file_extent_num_bytes(leaf, fi,
						split - key.offset);

		fi = btrfs_item_ptr(leaf, path->slots[0],
				    struct btrfs_file_extent_item);

		btrfs_set_file_extent_generation(leaf, fi, trans->transid);
		btrfs_set_file_extent_offset(leaf, fi, split - orig_offset);
		btrfs_set_file_extent_num_bytes(leaf, fi,
						extent_end - split);

		ref.action = BTRFS_ADD_DELAYED_REF;
		ref.bytenr = bytenr;
		ref.num_bytes = num_bytes;
		ref.parent = 0;
		ref.owning_root = btrfs_root_id(root);
		ref.ref_root = btrfs_root_id(root);
		btrfs_init_data_ref(&ref, ino, orig_offset, 0, false);
		ret = btrfs_inc_extent_ref(trans, &ref);
		if (ret) {
			btrfs_abort_transaction(trans, ret);
			goto out;
		}

		if (split == start) {
			key.offset = start;
		} else {
			if (start != key.offset) {
				ret = -EINVAL;
				btrfs_abort_transaction(trans, ret);
				goto out;
			}
			path->slots[0]--;
			extent_end = end;
		}
		recow = 1;
	}

	other_start = end;
	other_end = 0;

	ref.action = BTRFS_DROP_DELAYED_REF;
	ref.bytenr = bytenr;
	ref.num_bytes = num_bytes;
	ref.parent = 0;
	ref.owning_root = btrfs_root_id(root);
	ref.ref_root = btrfs_root_id(root);
	btrfs_init_data_ref(&ref, ino, orig_offset, 0, false);
	if (extent_mergeable(leaf, path->slots[0] + 1,
			     ino, bytenr, orig_offset,
			     &other_start, &other_end)) {
		if (recow) {
			btrfs_release_path(path);
			goto again;
		}
		extent_end = other_end;
		del_slot = path->slots[0] + 1;
		del_nr++;
		ret = btrfs_free_extent(trans, &ref);
		if (ret) {
			btrfs_abort_transaction(trans, ret);
			goto out;
		}
	}
	other_start = 0;
	other_end = start;
	if (extent_mergeable(leaf, path->slots[0] - 1,
			     ino, bytenr, orig_offset,
			     &other_start, &other_end)) {
		if (recow) {
			btrfs_release_path(path);
			goto again;
		}
		key.offset = other_start;
		del_slot = path->slots[0];
		del_nr++;
		ret = btrfs_free_extent(trans, &ref);
		if (ret) {
			btrfs_abort_transaction(trans, ret);
			goto out;
		}
	}
	if (del_nr == 0) {
		fi = btrfs_item_ptr(leaf, path->slots[0],
			   struct btrfs_file_extent_item);
		btrfs_set_file_extent_type(leaf, fi,
					   BTRFS_FILE_EXTENT_REG);
		btrfs_set_file_extent_generation(leaf, fi, trans->transid);
	} else {
		fi = btrfs_item_ptr(leaf, del_slot - 1,
			   struct btrfs_file_extent_item);
		btrfs_set_file_extent_type(leaf, fi,
					   BTRFS_FILE_EXTENT_REG);
		btrfs_set_file_extent_generation(leaf, fi, trans->transid);
		btrfs_set_file_extent_num_bytes(leaf, fi,
						extent_end - key.offset);

		ret = btrfs_del_items(trans, root, path, del_slot, del_nr);
		if (ret < 0) {
			btrfs_abort_transaction(trans, ret);
			goto out;
		}
	}
out:
	return ret;
}

/*
 * On error return an unlocked folio and the error value
 * On success return a locked folio and 0
 */
static int prepare_uptodate_folio(struct inode *inode, struct folio *folio, u64 pos,
				  u64 len)
{
	u64 clamp_start = max_t(u64, pos, folio_pos(folio));
	u64 clamp_end = min_t(u64, pos + len, folio_end(folio));
	const u32 blocksize = inode_to_fs_info(inode)->sectorsize;
	int ret = 0;

	if (folio_test_uptodate(folio))
		return 0;

	if (IS_ALIGNED(clamp_start, blocksize) &&
	    IS_ALIGNED(clamp_end, blocksize))
		return 0;

	ret = btrfs_read_folio(NULL, folio);
	if (ret)
		return ret;
	folio_lock(folio);
	if (!folio_test_uptodate(folio)) {
		folio_unlock(folio);
		return -EIO;
	}

	/*
	 * Since btrfs_read_folio() will unlock the folio before it returns,
	 * there is a window where btrfs_release_folio() can be called to
	 * release the page.  Here we check both inode mapping and page
	 * private to make sure the page was not released.
	 *
	 * The private flag check is essential for subpage as we need to store
	 * extra bitmap using folio private.
	 */
	if (folio->mapping != inode->i_mapping || !folio_test_private(folio)) {
		folio_unlock(folio);
		return -EAGAIN;
	}
	return 0;
}

static gfp_t get_prepare_gfp_flags(struct inode *inode, bool nowait)
{
	gfp_t gfp;

	gfp = btrfs_alloc_write_mask(inode->i_mapping);
	if (nowait) {
		gfp &= ~__GFP_DIRECT_RECLAIM;
		gfp |= GFP_NOWAIT;
	}

	return gfp;
}

/*
 * Get folio into the page cache and lock it.
 */
static noinline int prepare_one_folio(struct inode *inode, struct folio **folio_ret,
				      loff_t pos, size_t write_bytes,
				      bool nowait)
{
	const pgoff_t index = pos >> PAGE_SHIFT;
	gfp_t mask = get_prepare_gfp_flags(inode, nowait);
	fgf_t fgp_flags = (nowait ? FGP_WRITEBEGIN | FGP_NOWAIT : FGP_WRITEBEGIN) |
			  fgf_set_order(write_bytes);
	struct folio *folio;
	int ret = 0;

again:
	folio = __filemap_get_folio(inode->i_mapping, index, fgp_flags, mask);
	if (IS_ERR(folio))
		return PTR_ERR(folio);

	ret = set_folio_extent_mapped(folio);
	if (ret < 0) {
		folio_unlock(folio);
		folio_put(folio);
		return ret;
	}
	ret = prepare_uptodate_folio(inode, folio, pos, write_bytes);
	if (ret) {
		/* The folio is already unlocked. */
		folio_put(folio);
		if (!nowait && ret == -EAGAIN) {
			ret = 0;
			goto again;
		}
		return ret;
	}
	*folio_ret = folio;
	return 0;
}

/*
 * Locks the extent and properly waits for data=ordered extents to finish
 * before allowing the folios to be modified if need.
 *
 * Return:
 * 1 - the extent is locked
 * 0 - the extent is not locked, and everything is OK
 * -EAGAIN - need to prepare the folios again
 */
static noinline int
lock_and_cleanup_extent_if_need(struct btrfs_inode *inode, struct folio *folio,
				loff_t pos, size_t write_bytes,
				u64 *lockstart, u64 *lockend, bool nowait,
				struct extent_state **cached_state)
{
	struct btrfs_fs_info *fs_info = inode->root->fs_info;
	u64 start_pos;
	u64 last_pos;
	int ret = 0;

	start_pos = round_down(pos, fs_info->sectorsize);
	last_pos = round_up(pos + write_bytes, fs_info->sectorsize) - 1;

	if (start_pos < inode->vfs_inode.i_size) {
		struct btrfs_ordered_extent *ordered;

		if (nowait) {
			if (!btrfs_try_lock_extent(&inode->io_tree, start_pos,
						   last_pos, cached_state)) {
				folio_unlock(folio);
				folio_put(folio);
				return -EAGAIN;
			}
		} else {
			btrfs_lock_extent(&inode->io_tree, start_pos, last_pos,
					  cached_state);
		}

		ordered = btrfs_lookup_ordered_range(inode, start_pos,
						     last_pos - start_pos + 1);
		if (ordered &&
		    ordered->file_offset + ordered->num_bytes > start_pos &&
		    ordered->file_offset <= last_pos) {
			btrfs_unlock_extent(&inode->io_tree, start_pos, last_pos,
					    cached_state);
			folio_unlock(folio);
			folio_put(folio);
			btrfs_start_ordered_extent(ordered);
			btrfs_put_ordered_extent(ordered);
			return -EAGAIN;
		}
		if (ordered)
			btrfs_put_ordered_extent(ordered);

		*lockstart = start_pos;
		*lockend = last_pos;
		ret = 1;
	}

	/*
	 * We should be called after prepare_one_folio() which should have locked
	 * all pages in the range.
	 */
	WARN_ON(!folio_test_locked(folio));

	return ret;
}

/*
 * Check if we can do nocow write into the range [@pos, @pos + @write_bytes)
 *
 * @pos:         File offset.
 * @write_bytes: The length to write, will be updated to the nocow writeable
 *               range.
 * @nowait:      Indicate if we can block or not (non-blocking IO context).
 *
 * This function will flush ordered extents in the range to ensure proper
 * nocow checks.
 *
 * Return:
 * > 0          If we can nocow, and updates @write_bytes.
 *  0           If we can't do a nocow write.
 * -EAGAIN      If we can't do a nocow write because snapshoting of the inode's
 *              root is in progress or because we are in a non-blocking IO
 *              context and need to block (@nowait is true).
 * < 0          If an error happened.
 *
 * NOTE: Callers need to call btrfs_check_nocow_unlock() if we return > 0.
 */
int btrfs_check_nocow_lock(struct btrfs_inode *inode, loff_t pos,
			   size_t *write_bytes, bool nowait)
{
	struct btrfs_fs_info *fs_info = inode->root->fs_info;
	struct btrfs_root *root = inode->root;
	struct extent_state *cached_state = NULL;
	u64 lockstart, lockend;
	u64 cur_offset;
	int ret = 0;

	if (!(inode->flags & (BTRFS_INODE_NODATACOW | BTRFS_INODE_PREALLOC)))
		return 0;

	if (!btrfs_drew_try_write_lock(&root->snapshot_lock))
		return -EAGAIN;

	lockstart = round_down(pos, fs_info->sectorsize);
	lockend = round_up(pos + *write_bytes,
			   fs_info->sectorsize) - 1;

	if (nowait) {
		if (!btrfs_try_lock_ordered_range(inode, lockstart, lockend,
						  &cached_state)) {
			btrfs_drew_write_unlock(&root->snapshot_lock);
			return -EAGAIN;
		}
	} else {
		btrfs_lock_and_flush_ordered_range(inode, lockstart, lockend,
						   &cached_state);
	}

	cur_offset = lockstart;
	while (cur_offset < lockend) {
		u64 num_bytes = lockend - cur_offset + 1;

		ret = can_nocow_extent(inode, cur_offset, &num_bytes, NULL, nowait);
		if (ret <= 0) {
			/*
			 * If cur_offset == lockstart it means we haven't found
			 * any extent against which we can NOCOW, so unlock the
			 * snapshot lock.
			 */
			if (cur_offset == lockstart)
				btrfs_drew_write_unlock(&root->snapshot_lock);
			break;
		}
		cur_offset += num_bytes;
	}

	btrfs_unlock_extent(&inode->io_tree, lockstart, lockend, &cached_state);

	/*
	 * cur_offset > lockstart means there's at least a partial range we can
	 * NOCOW, and that range can cover one or more extents.
	 */
	if (cur_offset > lockstart) {
		*write_bytes = min_t(size_t, *write_bytes, cur_offset - pos);
		return 1;
	}

	return ret;
}

void btrfs_check_nocow_unlock(struct btrfs_inode *inode)
{
	btrfs_drew_write_unlock(&inode->root->snapshot_lock);
}

int btrfs_write_check(struct kiocb *iocb, size_t count)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file_inode(file);
	struct btrfs_fs_info *fs_info = inode_to_fs_info(inode);
	loff_t pos = iocb->ki_pos;
	int ret;
	loff_t oldsize;

	/*
	 * Quickly bail out on NOWAIT writes if we don't have the nodatacow or
	 * prealloc flags, as without those flags we always have to COW. We will
	 * later check if we can really COW into the target range (using
	 * can_nocow_extent() at btrfs_get_blocks_direct_write()).
	 */
	if ((iocb->ki_flags & IOCB_NOWAIT) &&
	    !(BTRFS_I(inode)->flags & (BTRFS_INODE_NODATACOW | BTRFS_INODE_PREALLOC)))
		return -EAGAIN;

	ret = file_remove_privs(file);
	if (ret)
		return ret;

	/*
	 * We reserve space for updating the inode when we reserve space for the
	 * extent we are going to write, so we will enospc out there.  We don't
	 * need to start yet another transaction to update the inode as we will
	 * update the inode when we finish writing whatever data we write.
	 */
	if (!IS_NOCMTIME(inode)) {
		inode_set_mtime_to_ts(inode, inode_set_ctime_current(inode));
		inode_inc_iversion(inode);
	}

	oldsize = i_size_read(inode);
	if (pos > oldsize) {
		/* Expand hole size to cover write data, preventing empty gap */
		loff_t end_pos = round_up(pos + count, fs_info->sectorsize);

		ret = btrfs_cont_expand(BTRFS_I(inode), oldsize, end_pos);
		if (ret)
			return ret;
	}

	return 0;
}

static void release_space(struct btrfs_inode *inode, struct extent_changeset *data_reserved,
			  u64 start, u64 len, bool only_release_metadata)
{
	if (len == 0)
		return;

	if (only_release_metadata) {
		btrfs_check_nocow_unlock(inode);
		btrfs_delalloc_release_metadata(inode, len, true);
	} else {
		const struct btrfs_fs_info *fs_info = inode->root->fs_info;

		btrfs_delalloc_release_space(inode, data_reserved,
					     round_down(start, fs_info->sectorsize),
					     len, true);
	}
}

/*
 * Reserve data and metadata space for this buffered write range.
 *
 * Return >0 for the number of bytes reserved, which is always block aligned.
 * Return <0 for error.
 */
static ssize_t reserve_space(struct btrfs_inode *inode,
			     struct extent_changeset **data_reserved,
			     u64 start, size_t *len, bool nowait,
			     bool *only_release_metadata)
{
	const struct btrfs_fs_info *fs_info = inode->root->fs_info;
	const unsigned int block_offset = (start & (fs_info->sectorsize - 1));
	size_t reserve_bytes;
	int ret;

	ret = btrfs_check_data_free_space(inode, data_reserved, start, *len, nowait);
	if (ret < 0) {
		int can_nocow;

		if (nowait && (ret == -ENOSPC || ret == -EAGAIN))
			return -EAGAIN;

		/*
		 * If we don't have to COW at the offset, reserve metadata only.
		 * write_bytes may get smaller than requested here.
		 */
		can_nocow = btrfs_check_nocow_lock(inode, start, len, nowait);
		if (can_nocow < 0)
			ret = can_nocow;
		if (can_nocow > 0)
			ret = 0;
		if (ret)
			return ret;
		*only_release_metadata = true;
	}

	reserve_bytes = round_up(*len + block_offset, fs_info->sectorsize);
	WARN_ON(reserve_bytes == 0);
	ret = btrfs_delalloc_reserve_metadata(inode, reserve_bytes,
					      reserve_bytes, nowait);
	if (ret) {
		if (!*only_release_metadata)
			btrfs_free_reserved_data_space(inode, *data_reserved,
						       start, *len);
		else
			btrfs_check_nocow_unlock(inode);

		if (nowait && ret == -ENOSPC)
			ret = -EAGAIN;
		return ret;
	}
	return reserve_bytes;
}

/* Shrink the reserved data and metadata space from @reserved_len to @new_len. */
static void shrink_reserved_space(struct btrfs_inode *inode,
				  struct extent_changeset *data_reserved,
				  u64 reserved_start, u64 reserved_len,
				  u64 new_len, bool only_release_metadata)
{
	const u64 diff = reserved_len - new_len;

	ASSERT(new_len <= reserved_len);
	btrfs_delalloc_shrink_extents(inode, reserved_len, new_len);
	if (only_release_metadata)
		btrfs_delalloc_release_metadata(inode, diff, true);
	else
		btrfs_delalloc_release_space(inode, data_reserved,
					     reserved_start + new_len, diff, true);
}

/* Calculate the maximum amount of bytes we can write into one folio. */
static size_t calc_write_bytes(const struct btrfs_inode *inode,
			       const struct iov_iter *iter, u64 start)
{
	const size_t max_folio_size = mapping_max_folio_size(inode->vfs_inode.i_mapping);

	return min(max_folio_size - (start & (max_folio_size - 1)),
		   iov_iter_count(iter));
}

/*
 * Do the heavy-lifting work to copy one range into one folio of the page cache.
 *
 * Return > 0 in case we copied all bytes or just some of them.
 * Return 0 if no bytes were copied, in which case the caller should retry.
 * Return <0 on error.
 */
static int copy_one_range(struct btrfs_inode *inode, struct iov_iter *iter,
			  struct extent_changeset **data_reserved, u64 start,
			  bool nowait)
{
	struct btrfs_fs_info *fs_info = inode->root->fs_info;
	struct extent_state *cached_state = NULL;
	size_t write_bytes = calc_write_bytes(inode, iter, start);
	size_t copied;
	const u64 reserved_start = round_down(start, fs_info->sectorsize);
	u64 reserved_len;
	struct folio *folio = NULL;
	int extents_locked;
	u64 lockstart;
	u64 lockend;
	bool only_release_metadata = false;
	const unsigned int bdp_flags = (nowait ? BDP_ASYNC : 0);
	int ret;

	/*
	 * Fault all pages before locking them in prepare_one_folio() to avoid
	 * recursive lock.
	 */
	if (unlikely(fault_in_iov_iter_readable(iter, write_bytes)))
		return -EFAULT;
	extent_changeset_release(*data_reserved);
	ret = reserve_space(inode, data_reserved, start, &write_bytes, nowait,
			    &only_release_metadata);
	if (ret < 0)
		return ret;
	reserved_len = ret;
	/* Write range must be inside the reserved range. */
	ASSERT(reserved_start <= start);
	ASSERT(start + write_bytes <= reserved_start + reserved_len);

again:
	ret = balance_dirty_pages_ratelimited_flags(inode->vfs_inode.i_mapping,
						    bdp_flags);
	if (ret) {
		btrfs_delalloc_release_extents(inode, reserved_len);
		release_space(inode, *data_reserved, reserved_start, reserved_len,
			      only_release_metadata);
		return ret;
	}

	ret = prepare_one_folio(&inode->vfs_inode, &folio, start, write_bytes, false);
	if (ret) {
		btrfs_delalloc_release_extents(inode, reserved_len);
		release_space(inode, *data_reserved, reserved_start, reserved_len,
			      only_release_metadata);
		return ret;
	}

	/*
	 * The reserved range goes beyond the current folio, shrink the reserved
	 * space to the folio boundary.
	 */
	if (reserved_start + reserved_len > folio_end(folio)) {
		const u64 last_block = folio_end(folio);

		shrink_reserved_space(inode, *data_reserved, reserved_start,
				      reserved_len, last_block - reserved_start,
				      only_release_metadata);
		write_bytes = last_block - start;
		reserved_len = last_block - reserved_start;
	}

	extents_locked = lock_and_cleanup_extent_if_need(inode, folio, start,
							 write_bytes, &lockstart,
							 &lockend, nowait,
							 &cached_state);
	if (extents_locked < 0) {
		if (!nowait && extents_locked == -EAGAIN)
			goto again;

		btrfs_delalloc_release_extents(inode, reserved_len);
		release_space(inode, *data_reserved, reserved_start, reserved_len,
			      only_release_metadata);
		ret = extents_locked;
		return ret;
	}

	copied = copy_folio_from_iter_atomic(folio, offset_in_folio(folio, start),
					     write_bytes, iter);
	flush_dcache_folio(folio);

	if (unlikely(copied < write_bytes)) {
		u64 last_block;

		/*
		 * The original write range doesn't need an uptodate folio as
		 * the range is block aligned. But now a short copy happened.
		 * We cannot handle it without an uptodate folio.
		 *
		 * So just revert the range and we will retry.
		 */
		if (!folio_test_uptodate(folio)) {
			iov_iter_revert(iter, copied);
			copied = 0;
		}

		/* No copied bytes, unlock, release reserved space and exit. */
		if (copied == 0) {
			if (extents_locked)
				btrfs_unlock_extent(&inode->io_tree, lockstart, lockend,
						    &cached_state);
			else
				btrfs_free_extent_state(cached_state);
			btrfs_delalloc_release_extents(inode, reserved_len);
			release_space(inode, *data_reserved, reserved_start, reserved_len,
				      only_release_metadata);
			btrfs_drop_folio(fs_info, folio, start, copied);
			return 0;
		}

		/* Release the reserved space beyond the last block. */
		last_block = round_up(start + copied, fs_info->sectorsize);

		shrink_reserved_space(inode, *data_reserved, reserved_start,
				      reserved_len, last_block - reserved_start,
				      only_release_metadata);
		reserved_len = last_block - reserved_start;
	}

	ret = btrfs_dirty_folio(inode, folio, start, copied, &cached_state,
				only_release_metadata);
	/*
	 * If we have not locked the extent range, because the range's start
	 * offset is >= i_size, we might still have a non-NULL cached extent
	 * state, acquired while marking the extent range as delalloc through
	 * btrfs_dirty_page(). Therefore free any possible cached extent state
	 * to avoid a memory leak.
	 */
	if (extents_locked)
		btrfs_unlock_extent(&inode->io_tree, lockstart, lockend, &cached_state);
	else
		btrfs_free_extent_state(cached_state);

	btrfs_delalloc_release_extents(inode, reserved_len);
	if (ret) {
		btrfs_drop_folio(fs_info, folio, start, copied);
		release_space(inode, *data_reserved, reserved_start, reserved_len,
			      only_release_metadata);
		return ret;
	}
	if (only_release_metadata)
		btrfs_check_nocow_unlock(inode);

	btrfs_drop_folio(fs_info, folio, start, copied);
	return copied;
}

ssize_t btrfs_buffered_write(struct kiocb *iocb, struct iov_iter *iter)
{
	struct file *file = iocb->ki_filp;
	loff_t pos;
	struct inode *inode = file_inode(file);
	struct extent_changeset *data_reserved = NULL;
	size_t num_written = 0;
	ssize_t ret;
	loff_t old_isize;
	unsigned int ilock_flags = 0;
	const bool nowait = (iocb->ki_flags & IOCB_NOWAIT);

	if (nowait)
		ilock_flags |= BTRFS_ILOCK_TRY;

	ret = btrfs_inode_lock(BTRFS_I(inode), ilock_flags);
	if (ret < 0)
		return ret;

	/*
	 * We can only trust the isize with inode lock held, or it can race with
	 * other buffered writes and cause incorrect call of
	 * pagecache_isize_extended() to overwrite existing data.
	 */
	old_isize = i_size_read(inode);

	ret = generic_write_checks(iocb, iter);
	if (ret <= 0)
		goto out;

	ret = btrfs_write_check(iocb, ret);
	if (ret < 0)
		goto out;

	pos = iocb->ki_pos;
	while (iov_iter_count(iter) > 0) {
		ret = copy_one_range(BTRFS_I(inode), iter, &data_reserved, pos, nowait);
		if (ret < 0)
			break;
		pos += ret;
		num_written += ret;
		cond_resched();
	}

	extent_changeset_free(data_reserved);
	if (num_written > 0) {
		pagecache_isize_extended(inode, old_isize, iocb->ki_pos);
		iocb->ki_pos += num_written;
	}
out:
	btrfs_inode_unlock(BTRFS_I(inode), ilock_flags);
	return num_written ? num_written : ret;
}

static ssize_t btrfs_encoded_write(struct kiocb *iocb, struct iov_iter *from,
			const struct btrfs_ioctl_encoded_io_args *encoded)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file_inode(file);
	loff_t count;
	ssize_t ret;

	btrfs_inode_lock(BTRFS_I(inode), 0);
	count = encoded->len;
	ret = generic_write_checks_count(iocb, &count);
	if (ret == 0 && count != encoded->len) {
		/*
		 * The write got truncated by generic_write_checks_count(). We
		 * can't do a partial encoded write.
		 */
		ret = -EFBIG;
	}
	if (ret || encoded->len == 0)
		goto out;

	ret = btrfs_write_check(iocb, encoded->len);
	if (ret < 0)
		goto out;

	ret = btrfs_do_encoded_write(iocb, from, encoded);
out:
	btrfs_inode_unlock(BTRFS_I(inode), 0);
	return ret;
}

ssize_t btrfs_do_write_iter(struct kiocb *iocb, struct iov_iter *from,
			    const struct btrfs_ioctl_encoded_io_args *encoded)
{
	struct file *file = iocb->ki_filp;
	struct btrfs_inode *inode = BTRFS_I(file_inode(file));
	ssize_t num_written, num_sync;

	/*
	 * If the fs flips readonly due to some impossible error, although we
	 * have opened a file as writable, we have to stop this write operation
	 * to ensure consistency.
	 */
	if (BTRFS_FS_ERROR(inode->root->fs_info))
		return -EROFS;

	if (encoded && (iocb->ki_flags & IOCB_NOWAIT))
		return -EOPNOTSUPP;

	if (encoded) {
		num_written = btrfs_encoded_write(iocb, from, encoded);
		num_sync = encoded->len;
	} else if (iocb->ki_flags & IOCB_DIRECT) {
		num_written = btrfs_direct_write(iocb, from);
		num_sync = num_written;
	} else {
		num_written = btrfs_buffered_write(iocb, from);
		num_sync = num_written;
	}

	btrfs_set_inode_last_sub_trans(inode);

	if (num_sync > 0) {
		num_sync = generic_write_sync(iocb, num_sync);
		if (num_sync < 0)
			num_written = num_sync;
	}

	return num_written;
}

static ssize_t btrfs_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	return btrfs_do_write_iter(iocb, from, NULL);
}

int btrfs_release_file(struct inode *inode, struct file *filp)
{
	struct btrfs_file_private *private = filp->private_data;

	if (private) {
		kfree(private->filldir_buf);
		btrfs_free_extent_state(private->llseek_cached_state);
		kfree(private);
		filp->private_data = NULL;
	}

	/*
	 * Set by setattr when we are about to truncate a file from a non-zero
	 * size to a zero size.  This tries to flush down new bytes that may
	 * have been written if the application were using truncate to replace
	 * a file in place.
	 */
	if (test_and_clear_bit(BTRFS_INODE_FLUSH_ON_CLOSE,
			       &BTRFS_I(inode)->runtime_flags))
			filemap_flush(inode->i_mapping);
	return 0;
}

static int start_ordered_ops(struct btrfs_inode *inode, loff_t start, loff_t end)
{
	int ret;
	struct blk_plug plug;

	/*
	 * This is only called in fsync, which would do synchronous writes, so
	 * a plug can merge adjacent IOs as much as possible.  Esp. in case of
	 * multiple disks using raid profile, a large IO can be split to
	 * several segments of stripe length (currently 64K).
	 */
	blk_start_plug(&plug);
	ret = btrfs_fdatawrite_range(inode, start, end);
	blk_finish_plug(&plug);

	return ret;
}

static inline bool skip_inode_logging(const struct btrfs_log_ctx *ctx)
{
	struct btrfs_inode *inode = ctx->inode;
	struct btrfs_fs_info *fs_info = inode->root->fs_info;

	if (btrfs_inode_in_log(inode, btrfs_get_fs_generation(fs_info)) &&
	    list_empty(&ctx->ordered_extents))
		return true;

	/*
	 * If we are doing a fast fsync we can not bail out if the inode's
	 * last_trans is <= then the last committed transaction, because we only
	 * update the last_trans of the inode during ordered extent completion,
	 * and for a fast fsync we don't wait for that, we only wait for the
	 * writeback to complete.
	 */
	if (inode->last_trans <= btrfs_get_last_trans_committed(fs_info) &&
	    (test_bit(BTRFS_INODE_NEEDS_FULL_SYNC, &inode->runtime_flags) ||
	     list_empty(&ctx->ordered_extents)))
		return true;

	return false;
}

/*
 * fsync call for both files and directories.  This logs the inode into
 * the tree log instead of forcing full commits whenever possible.
 *
 * It needs to call filemap_fdatawait so that all ordered extent updates are
 * in the metadata btree are up to date for copying to the log.
 *
 * It drops the inode mutex before doing the tree log commit.  This is an
 * important optimization for directories because holding the mutex prevents
 * new operations on the dir while we write to disk.
 */
int btrfs_sync_file(struct file *file, loff_t start, loff_t end, int datasync)
{
	struct dentry *dentry = file_dentry(file);
	struct btrfs_inode *inode = BTRFS_I(d_inode(dentry));
	struct btrfs_root *root = inode->root;
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct btrfs_trans_handle *trans;
	struct btrfs_log_ctx ctx;
	int ret = 0, err;
	u64 len;
	bool full_sync;
	bool skip_ilock = false;

	if (current->journal_info == BTRFS_TRANS_DIO_WRITE_STUB) {
		skip_ilock = true;
		current->journal_info = NULL;
		btrfs_assert_inode_locked(inode);
	}

	trace_btrfs_sync_file(file, datasync);

	btrfs_init_log_ctx(&ctx, inode);

	/*
	 * Always set the range to a full range, otherwise we can get into
	 * several problems, from missing file extent items to represent holes
	 * when not using the NO_HOLES feature, to log tree corruption due to
	 * races between hole detection during logging and completion of ordered
	 * extents outside the range, to missing checksums due to ordered extents
	 * for which we flushed only a subset of their pages.
	 */
	start = 0;
	end = LLONG_MAX;
	len = (u64)LLONG_MAX + 1;

	/*
	 * We write the dirty pages in the range and wait until they complete
	 * out of the ->i_mutex. If so, we can flush the dirty pages by
	 * multi-task, and make the performance up.  See
	 * btrfs_wait_ordered_range for an explanation of the ASYNC check.
	 */
	ret = start_ordered_ops(inode, start, end);
	if (ret)
		goto out;

	if (skip_ilock)
		down_write(&inode->i_mmap_lock);
	else
		btrfs_inode_lock(inode, BTRFS_ILOCK_MMAP);

	atomic_inc(&root->log_batch);

	/*
	 * Before we acquired the inode's lock and the mmap lock, someone may
	 * have dirtied more pages in the target range. We need to make sure
	 * that writeback for any such pages does not start while we are logging
	 * the inode, because if it does, any of the following might happen when
	 * we are not doing a full inode sync:
	 *
	 * 1) We log an extent after its writeback finishes but before its
	 *    checksums are added to the csum tree, leading to -EIO errors
	 *    when attempting to read the extent after a log replay.
	 *
	 * 2) We can end up logging an extent before its writeback finishes.
	 *    Therefore after the log replay we will have a file extent item
	 *    pointing to an unwritten extent (and no data checksums as well).
	 *
	 * So trigger writeback for any eventual new dirty pages and then we
	 * wait for all ordered extents to complete below.
	 */
	ret = start_ordered_ops(inode, start, end);
	if (ret) {
		if (skip_ilock)
			up_write(&inode->i_mmap_lock);
		else
			btrfs_inode_unlock(inode, BTRFS_ILOCK_MMAP);
		goto out;
	}

	/*
	 * Always check for the full sync flag while holding the inode's lock,
	 * to avoid races with other tasks. The flag must be either set all the
	 * time during logging or always off all the time while logging.
	 * We check the flag here after starting delalloc above, because when
	 * running delalloc the full sync flag may be set if we need to drop
	 * extra extent map ranges due to temporary memory allocation failures.
	 */
	full_sync = test_bit(BTRFS_INODE_NEEDS_FULL_SYNC, &inode->runtime_flags);

	/*
	 * We have to do this here to avoid the priority inversion of waiting on
	 * IO of a lower priority task while holding a transaction open.
	 *
	 * For a full fsync we wait for the ordered extents to complete while
	 * for a fast fsync we wait just for writeback to complete, and then
	 * attach the ordered extents to the transaction so that a transaction
	 * commit waits for their completion, to avoid data loss if we fsync,
	 * the current transaction commits before the ordered extents complete
	 * and a power failure happens right after that.
	 *
	 * For zoned filesystem, if a write IO uses a ZONE_APPEND command, the
	 * logical address recorded in the ordered extent may change. We need
	 * to wait for the IO to stabilize the logical address.
	 */
	if (full_sync || btrfs_is_zoned(fs_info)) {
		ret = btrfs_wait_ordered_range(inode, start, len);
		clear_bit(BTRFS_INODE_COW_WRITE_ERROR, &inode->runtime_flags);
	} else {
		/*
		 * Get our ordered extents as soon as possible to avoid doing
		 * checksum lookups in the csum tree, and use instead the
		 * checksums attached to the ordered extents.
		 */
		btrfs_get_ordered_extents_for_logging(inode, &ctx.ordered_extents);
		ret = filemap_fdatawait_range(inode->vfs_inode.i_mapping, start, end);
		if (ret)
			goto out_release_extents;

		/*
		 * Check and clear the BTRFS_INODE_COW_WRITE_ERROR now after
		 * starting and waiting for writeback, because for buffered IO
		 * it may have been set during the end IO callback
		 * (end_bbio_data_write() -> btrfs_finish_ordered_extent()) in
		 * case an error happened and we need to wait for ordered
		 * extents to complete so that any extent maps that point to
		 * unwritten locations are dropped and we don't log them.
		 */
		if (test_and_clear_bit(BTRFS_INODE_COW_WRITE_ERROR, &inode->runtime_flags))
			ret = btrfs_wait_ordered_range(inode, start, len);
	}

	if (ret)
		goto out_release_extents;

	atomic_inc(&root->log_batch);

	if (skip_inode_logging(&ctx)) {
		/*
		 * We've had everything committed since the last time we were
		 * modified so clear this flag in case it was set for whatever
		 * reason, it's no longer relevant.
		 */
		clear_bit(BTRFS_INODE_NEEDS_FULL_SYNC, &inode->runtime_flags);
		/*
		 * An ordered extent might have started before and completed
		 * already with io errors, in which case the inode was not
		 * updated and we end up here. So check the inode's mapping
		 * for any errors that might have happened since we last
		 * checked called fsync.
		 */
		ret = filemap_check_wb_err(inode->vfs_inode.i_mapping, file->f_wb_err);
		goto out_release_extents;
	}

	btrfs_init_log_ctx_scratch_eb(&ctx);

	/*
	 * We use start here because we will need to wait on the IO to complete
	 * in btrfs_sync_log, which could require joining a transaction (for
	 * example checking cross references in the nocow path).  If we use join
	 * here we could get into a situation where we're waiting on IO to
	 * happen that is blocked on a transaction trying to commit.  With start
	 * we inc the extwriter counter, so we wait for all extwriters to exit
	 * before we start blocking joiners.  This comment is to keep somebody
	 * from thinking they are super smart and changing this to
	 * btrfs_join_transaction *cough*Josef*cough*.
	 */
	trans = btrfs_start_transaction(root, 0);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		goto out_release_extents;
	}
	trans->in_fsync = true;

	ret = btrfs_log_dentry_safe(trans, dentry, &ctx);
	/*
	 * Scratch eb no longer needed, release before syncing log or commit
	 * transaction, to avoid holding unnecessary memory during such long
	 * operations.
	 */
	if (ctx.scratch_eb) {
		free_extent_buffer(ctx.scratch_eb);
		ctx.scratch_eb = NULL;
	}
	btrfs_release_log_ctx_extents(&ctx);
	if (ret < 0) {
		/* Fallthrough and commit/free transaction. */
		ret = BTRFS_LOG_FORCE_COMMIT;
	}

	/* we've logged all the items and now have a consistent
	 * version of the file in the log.  It is possible that
	 * someone will come in and modify the file, but that's
	 * fine because the log is consistent on disk, and we
	 * have references to all of the file's extents
	 *
	 * It is possible that someone will come in and log the
	 * file again, but that will end up using the synchronization
	 * inside btrfs_sync_log to keep things safe.
	 */
	if (skip_ilock)
		up_write(&inode->i_mmap_lock);
	else
		btrfs_inode_unlock(inode, BTRFS_ILOCK_MMAP);

	if (ret == BTRFS_NO_LOG_SYNC) {
		ret = btrfs_end_transaction(trans);
		goto out;
	}

	/* We successfully logged the inode, attempt to sync the log. */
	if (!ret) {
		ret = btrfs_sync_log(trans, root, &ctx);
		if (!ret) {
			ret = btrfs_end_transaction(trans);
			goto out;
		}
	}

	/*
	 * At this point we need to commit the transaction because we had
	 * btrfs_need_log_full_commit() or some other error.
	 *
	 * If we didn't do a full sync we have to stop the trans handle, wait on
	 * the ordered extents, start it again and commit the transaction.  If
	 * we attempt to wait on the ordered extents here we could deadlock with
	 * something like fallocate() that is holding the extent lock trying to
	 * start a transaction while some other thread is trying to commit the
	 * transaction while we (fsync) are currently holding the transaction
	 * open.
	 */
	if (!full_sync) {
		ret = btrfs_end_transaction(trans);
		if (ret)
			goto out;
		ret = btrfs_wait_ordered_range(inode, start, len);
		if (ret)
			goto out;

		/*
		 * This is safe to use here because we're only interested in
		 * making sure the transaction that had the ordered extents is
		 * committed.  We aren't waiting on anything past this point,
		 * we're purely getting the transaction and committing it.
		 */
		trans = btrfs_attach_transaction_barrier(root);
		if (IS_ERR(trans)) {
			ret = PTR_ERR(trans);

			/*
			 * We committed the transaction and there's no currently
			 * running transaction, this means everything we care
			 * about made it to disk and we are done.
			 */
			if (ret == -ENOENT)
				ret = 0;
			goto out;
		}
	}

	ret = btrfs_commit_transaction(trans);
out:
	free_extent_buffer(ctx.scratch_eb);
	ASSERT(list_empty(&ctx.list));
	ASSERT(list_empty(&ctx.conflict_inodes));
	err = file_check_and_advance_wb_err(file);
	if (!ret)
		ret = err;
	return ret > 0 ? -EIO : ret;

out_release_extents:
	btrfs_release_log_ctx_extents(&ctx);
	if (skip_ilock)
		up_write(&inode->i_mmap_lock);
	else
		btrfs_inode_unlock(inode, BTRFS_ILOCK_MMAP);
	goto out;
}

/*
 * btrfs_page_mkwrite() is not allowed to change the file size as it gets
 * called from a page fault handler when a page is first dirtied. Hence we must
 * be careful to check for EOF conditions here. We set the page up correctly
 * for a written page which means we get ENOSPC checking when writing into
 * holes and correct delalloc and unwritten extent mapping on filesystems that
 * support these features.
 *
 * We are not allowed to take the i_mutex here so we have to play games to
 * protect against truncate races as the page could now be beyond EOF.  Because
 * truncate_setsize() writes the inode size before removing pages, once we have
 * the page lock we can determine safely if the page is beyond EOF. If it is not
 * beyond EOF, then the page is guaranteed safe against truncation until we
 * unlock the page.
 */
static vm_fault_t btrfs_page_mkwrite(struct vm_fault *vmf)
{
	struct page *page = vmf->page;
	struct folio *folio = page_folio(page);
	struct btrfs_inode *inode = BTRFS_I(file_inode(vmf->vma->vm_file));
	struct btrfs_fs_info *fs_info = inode->root->fs_info;
	struct extent_io_tree *io_tree = &inode->io_tree;
	struct btrfs_ordered_extent *ordered;
	struct extent_state *cached_state = NULL;
	struct extent_changeset *data_reserved = NULL;
	unsigned long zero_start;
	loff_t size;
	size_t fsize = folio_size(folio);
	int ret;
	bool only_release_metadata = false;
	u64 reserved_space;
	u64 page_start;
	u64 page_end;
	u64 end;

	reserved_space = fsize;

	sb_start_pagefault(inode->vfs_inode.i_sb);
	page_start = folio_pos(folio);
	page_end = page_start + folio_size(folio) - 1;
	end = page_end;

	/*
	 * Reserving delalloc space after obtaining the page lock can lead to
	 * deadlock. For example, if a dirty page is locked by this function
	 * and the call to btrfs_delalloc_reserve_space() ends up triggering
	 * dirty page write out, then the btrfs_writepages() function could
	 * end up waiting indefinitely to get a lock on the page currently
	 * being processed by btrfs_page_mkwrite() function.
	 */
	ret = btrfs_check_data_free_space(inode, &data_reserved, page_start,
					  reserved_space, false);
	if (ret < 0) {
		size_t write_bytes = reserved_space;

		if (btrfs_check_nocow_lock(inode, page_start, &write_bytes, false) <= 0)
			goto out_noreserve;

		only_release_metadata = true;

		/*
		 * Can't write the whole range, there may be shared extents or
		 * holes in the range, bail out with @only_release_metadata set
		 * to true so that we unlock the nocow lock before returning the
		 * error.
		 */
		if (write_bytes < reserved_space)
			goto out_noreserve;
	}
	ret = btrfs_delalloc_reserve_metadata(inode, reserved_space,
					      reserved_space, false);
	if (ret < 0) {
		if (!only_release_metadata)
			btrfs_free_reserved_data_space(inode, data_reserved,
						       page_start, reserved_space);
		goto out_noreserve;
	}

	ret = file_update_time(vmf->vma->vm_file);
	if (ret < 0)
		goto out;
again:
	down_read(&inode->i_mmap_lock);
	folio_lock(folio);
	size = i_size_read(&inode->vfs_inode);

	if ((folio->mapping != inode->vfs_inode.i_mapping) ||
	    (page_start >= size)) {
		/* Page got truncated out from underneath us. */
		goto out_unlock;
	}
	folio_wait_writeback(folio);

	btrfs_lock_extent(io_tree, page_start, page_end, &cached_state);
	ret = set_folio_extent_mapped(folio);
	if (ret < 0) {
		btrfs_unlock_extent(io_tree, page_start, page_end, &cached_state);
		goto out_unlock;
	}

	/*
	 * We can't set the delalloc bits if there are pending ordered
	 * extents.  Drop our locks and wait for them to finish.
	 */
	ordered = btrfs_lookup_ordered_range(inode, page_start, fsize);
	if (ordered) {
		btrfs_unlock_extent(io_tree, page_start, page_end, &cached_state);
		folio_unlock(folio);
		up_read(&inode->i_mmap_lock);
		btrfs_start_ordered_extent(ordered);
		btrfs_put_ordered_extent(ordered);
		goto again;
	}

	if (folio_contains(folio, (size - 1) >> PAGE_SHIFT)) {
		reserved_space = round_up(size - page_start, fs_info->sectorsize);
		if (reserved_space < fsize) {
			const u64 to_free = fsize - reserved_space;

			end = page_start + reserved_space - 1;
			if (only_release_metadata)
				btrfs_delalloc_release_metadata(inode, to_free, true);
			else
				btrfs_delalloc_release_space(inode, data_reserved,
							     end + 1, to_free, true);
		}
	}

	/*
	 * page_mkwrite gets called when the page is firstly dirtied after it's
	 * faulted in, but write(2) could also dirty a page and set delalloc
	 * bits, thus in this case for space account reason, we still need to
	 * clear any delalloc bits within this page range since we have to
	 * reserve data&meta space before lock_page() (see above comments).
	 */
	btrfs_clear_extent_bit(io_tree, page_start, end,
			       EXTENT_DELALLOC | EXTENT_DO_ACCOUNTING |
			       EXTENT_DEFRAG, &cached_state);

	ret = btrfs_set_extent_delalloc(inode, page_start, end, 0, &cached_state);
	if (ret < 0) {
		btrfs_unlock_extent(io_tree, page_start, page_end, &cached_state);
		goto out_unlock;
	}

	/* Page is wholly or partially inside EOF. */
	if (page_start + folio_size(folio) > size)
		zero_start = offset_in_folio(folio, size);
	else
		zero_start = fsize;

	if (zero_start != fsize)
		folio_zero_range(folio, zero_start, folio_size(folio) - zero_start);

	btrfs_folio_clear_checked(fs_info, folio, page_start, fsize);
	btrfs_folio_set_dirty(fs_info, folio, page_start, end + 1 - page_start);
	btrfs_folio_set_uptodate(fs_info, folio, page_start, end + 1 - page_start);

	btrfs_set_inode_last_sub_trans(inode);

	if (only_release_metadata)
		btrfs_set_extent_bit(io_tree, page_start, end, EXTENT_NORESERVE,
				     &cached_state);

	btrfs_unlock_extent(io_tree, page_start, page_end, &cached_state);
	up_read(&inode->i_mmap_lock);

	btrfs_delalloc_release_extents(inode, fsize);
	if (only_release_metadata)
		btrfs_check_nocow_unlock(inode);
	sb_end_pagefault(inode->vfs_inode.i_sb);
	extent_changeset_free(data_reserved);
	return VM_FAULT_LOCKED;

out_unlock:
	folio_unlock(folio);
	up_read(&inode->i_mmap_lock);
out:
	btrfs_delalloc_release_extents(inode, fsize);
	if (only_release_metadata)
		btrfs_delalloc_release_metadata(inode, reserved_space, true);
	else
		btrfs_delalloc_release_space(inode, data_reserved, page_start,
					     reserved_space, true);
	extent_changeset_free(data_reserved);
out_noreserve:
	if (only_release_metadata)
		btrfs_check_nocow_unlock(inode);

	sb_end_pagefault(inode->vfs_inode.i_sb);

	if (ret < 0)
		return vmf_error(ret);

	/* Make the VM retry the fault. */
	return VM_FAULT_NOPAGE;
}

static const struct vm_operations_struct btrfs_file_vm_ops = {
	.fault		= filemap_fault,
	.map_pages	= filemap_map_pages,
	.page_mkwrite	= btrfs_page_mkwrite,
};

static int btrfs_file_mmap_prepare(struct vm_area_desc *desc)
{
	struct file *filp = desc->file;
	struct address_space *mapping = filp->f_mapping;

	if (!mapping->a_ops->read_folio)
		return -ENOEXEC;

	file_accessed(filp);
	desc->vm_ops = &btrfs_file_vm_ops;

	return 0;
}

static bool hole_mergeable(struct btrfs_inode *inode, struct extent_buffer *leaf,
			   int slot, u64 start, u64 end)
{
	struct btrfs_file_extent_item *fi;
	struct btrfs_key key;

	if (slot < 0 || slot >= btrfs_header_nritems(leaf))
		return false;

	btrfs_item_key_to_cpu(leaf, &key, slot);
	if (key.objectid != btrfs_ino(inode) ||
	    key.type != BTRFS_EXTENT_DATA_KEY)
		return false;

	fi = btrfs_item_ptr(leaf, slot, struct btrfs_file_extent_item);

	if (btrfs_file_extent_type(leaf, fi) != BTRFS_FILE_EXTENT_REG)
		return false;

	if (btrfs_file_extent_disk_bytenr(leaf, fi))
		return false;

	if (key.offset == end)
		return true;
	if (key.offset + btrfs_file_extent_num_bytes(leaf, fi) == start)
		return true;
	return false;
}

static int fill_holes(struct btrfs_trans_handle *trans,
		struct btrfs_inode *inode,
		struct btrfs_path *path, u64 offset, u64 end)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_root *root = inode->root;
	struct extent_buffer *leaf;
	struct btrfs_file_extent_item *fi;
	struct extent_map *hole_em;
	struct btrfs_key key;
	int ret;

	if (btrfs_fs_incompat(fs_info, NO_HOLES))
		goto out;

	key.objectid = btrfs_ino(inode);
	key.type = BTRFS_EXTENT_DATA_KEY;
	key.offset = offset;

	ret = btrfs_search_slot(trans, root, &key, path, 0, 1);
	if (ret <= 0) {
		/*
		 * We should have dropped this offset, so if we find it then
		 * something has gone horribly wrong.
		 */
		if (ret == 0)
			ret = -EINVAL;
		return ret;
	}

	leaf = path->nodes[0];
	if (hole_mergeable(inode, leaf, path->slots[0] - 1, offset, end)) {
		u64 num_bytes;

		path->slots[0]--;
		fi = btrfs_item_ptr(leaf, path->slots[0],
				    struct btrfs_file_extent_item);
		num_bytes = btrfs_file_extent_num_bytes(leaf, fi) +
			end - offset;
		btrfs_set_file_extent_num_bytes(leaf, fi, num_bytes);
		btrfs_set_file_extent_ram_bytes(leaf, fi, num_bytes);
		btrfs_set_file_extent_offset(leaf, fi, 0);
		btrfs_set_file_extent_generation(leaf, fi, trans->transid);
		goto out;
	}

	if (hole_mergeable(inode, leaf, path->slots[0], offset, end)) {
		u64 num_bytes;

		key.offset = offset;
		btrfs_set_item_key_safe(trans, path, &key);
		fi = btrfs_item_ptr(leaf, path->slots[0],
				    struct btrfs_file_extent_item);
		num_bytes = btrfs_file_extent_num_bytes(leaf, fi) + end -
			offset;
		btrfs_set_file_extent_num_bytes(leaf, fi, num_bytes);
		btrfs_set_file_extent_ram_bytes(leaf, fi, num_bytes);
		btrfs_set_file_extent_offset(leaf, fi, 0);
		btrfs_set_file_extent_generation(leaf, fi, trans->transid);
		goto out;
	}
	btrfs_release_path(path);

	ret = btrfs_insert_hole_extent(trans, root, btrfs_ino(inode), offset,
				       end - offset);
	if (ret)
		return ret;

out:
	btrfs_release_path(path);

	hole_em = btrfs_alloc_extent_map();
	if (!hole_em) {
		btrfs_drop_extent_map_range(inode, offset, end - 1, false);
		btrfs_set_inode_full_sync(inode);
	} else {
		hole_em->start = offset;
		hole_em->len = end - offset;
		hole_em->ram_bytes = hole_em->len;

		hole_em->disk_bytenr = EXTENT_MAP_HOLE;
		hole_em->disk_num_bytes = 0;
		hole_em->generation = trans->transid;

		ret = btrfs_replace_extent_map_range(inode, hole_em, true);
		btrfs_free_extent_map(hole_em);
		if (ret)
			btrfs_set_inode_full_sync(inode);
	}

	return 0;
}

/*
 * Find a hole extent on given inode and change start/len to the end of hole
 * extent.(hole/vacuum extent whose em->start <= start &&
 *	   em->start + em->len > start)
 * When a hole extent is found, return 1 and modify start/len.
 */
static int find_first_non_hole(struct btrfs_inode *inode, u64 *start, u64 *len)
{
	struct btrfs_fs_info *fs_info = inode->root->fs_info;
	struct extent_map *em;
	int ret = 0;

	em = btrfs_get_extent(inode, NULL,
			      round_down(*start, fs_info->sectorsize),
			      round_up(*len, fs_info->sectorsize));
	if (IS_ERR(em))
		return PTR_ERR(em);

	/* Hole or vacuum extent(only exists in no-hole mode) */
	if (em->disk_bytenr == EXTENT_MAP_HOLE) {
		ret = 1;
		*len = em->start + em->len > *start + *len ?
		       0 : *start + *len - em->start - em->len;
		*start = em->start + em->len;
	}
	btrfs_free_extent_map(em);
	return ret;
}

/*
 * Check if there is no folio in the range.
 *
 * We cannot utilize filemap_range_has_page() in a filemap with large folios
 * as we can hit the following false positive:
 *
 *        start                            end
 *        |                                |
 *  |//|//|//|//|  |  |  |  |  |  |  |  |//|//|
 *   \         /                         \   /
 *    Folio A                            Folio B
 *
 * That large folio A and B cover the start and end indexes.
 * In that case filemap_range_has_page() will always return true, but the above
 * case is fine for btrfs_punch_hole_lock_range() usage.
 *
 * So here we only ensure that no other folios is in the range, excluding the
 * head/tail large folio.
 */
static bool check_range_has_page(struct inode *inode, u64 start, u64 end)
{
	struct folio_batch fbatch;
	bool ret = false;
	/*
	 * For subpage case, if the range is not at page boundary, we could
	 * have pages at the leading/tailing part of the range.
	 * This could lead to dead loop since filemap_range_has_page()
	 * will always return true.
	 * So here we need to do extra page alignment for
	 * filemap_range_has_page().
	 *
	 * And do not decrease page_lockend right now, as it can be 0.
	 */
	const u64 page_lockstart = round_up(start, PAGE_SIZE);
	const u64 page_lockend = round_down(end + 1, PAGE_SIZE);
	const pgoff_t start_index = page_lockstart >> PAGE_SHIFT;
	const pgoff_t end_index = (page_lockend - 1) >> PAGE_SHIFT;
	pgoff_t tmp = start_index;
	int found_folios;

	/* The same page or adjacent pages. */
	if (page_lockend <= page_lockstart)
		return false;

	folio_batch_init(&fbatch);
	found_folios = filemap_get_folios(inode->i_mapping, &tmp, end_index, &fbatch);
	for (int i = 0; i < found_folios; i++) {
		struct folio *folio = fbatch.folios[i];

		/* A large folio begins before the start. Not a target. */
		if (folio->index < start_index)
			continue;
		/* A large folio extends beyond the end. Not a target. */
		if (folio_next_index(folio) > end_index)
			continue;
		/* A folio doesn't cover the head/tail index. Found a target. */
		ret = true;
		break;
	}
	folio_batch_release(&fbatch);
	return ret;
}

static void btrfs_punch_hole_lock_range(struct inode *inode,
					const u64 lockstart, const u64 lockend,
					struct extent_state **cached_state)
{
	while (1) {
		truncate_pagecache_range(inode, lockstart, lockend);

		btrfs_lock_extent(&BTRFS_I(inode)->io_tree, lockstart, lockend,
				  cached_state);
		/*
		 * We can't have ordered extents in the range, nor dirty/writeback
		 * pages, because we have locked the inode's VFS lock in exclusive
		 * mode, we have locked the inode's i_mmap_lock in exclusive mode,
		 * we have flushed all delalloc in the range and we have waited
		 * for any ordered extents in the range to complete.
		 * We can race with anyone reading pages from this range, so after
		 * locking the range check if we have pages in the range, and if
		 * we do, unlock the range and retry.
		 */
		if (!check_range_has_page(inode, lockstart, lockend))
			break;

		btrfs_unlock_extent(&BTRFS_I(inode)->io_tree, lockstart, lockend,
				    cached_state);
	}

	btrfs_assert_inode_range_clean(BTRFS_I(inode), lockstart, lockend);
}

static int btrfs_insert_replace_extent(struct btrfs_trans_handle *trans,
				     struct btrfs_inode *inode,
				     struct btrfs_path *path,
				     struct btrfs_replace_extent_info *extent_info,
				     const u64 replace_len,
				     const u64 bytes_to_drop)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_root *root = inode->root;
	struct btrfs_file_extent_item *extent;
	struct extent_buffer *leaf;
	struct btrfs_key key;
	int slot;
	int ret;

	if (replace_len == 0)
		return 0;

	if (extent_info->disk_offset == 0 &&
	    btrfs_fs_incompat(fs_info, NO_HOLES)) {
		btrfs_update_inode_bytes(inode, 0, bytes_to_drop);
		return 0;
	}

	key.objectid = btrfs_ino(inode);
	key.type = BTRFS_EXTENT_DATA_KEY;
	key.offset = extent_info->file_offset;
	ret = btrfs_insert_empty_item(trans, root, path, &key,
				      sizeof(struct btrfs_file_extent_item));
	if (ret)
		return ret;
	leaf = path->nodes[0];
	slot = path->slots[0];
	write_extent_buffer(leaf, extent_info->extent_buf,
			    btrfs_item_ptr_offset(leaf, slot),
			    sizeof(struct btrfs_file_extent_item));
	extent = btrfs_item_ptr(leaf, slot, struct btrfs_file_extent_item);
	ASSERT(btrfs_file_extent_type(leaf, extent) != BTRFS_FILE_EXTENT_INLINE);
	btrfs_set_file_extent_offset(leaf, extent, extent_info->data_offset);
	btrfs_set_file_extent_num_bytes(leaf, extent, replace_len);
	if (extent_info->is_new_extent)
		btrfs_set_file_extent_generation(leaf, extent, trans->transid);
	btrfs_release_path(path);

	ret = btrfs_inode_set_file_extent_range(inode, extent_info->file_offset,
						replace_len);
	if (ret)
		return ret;

	/* If it's a hole, nothing more needs to be done. */
	if (extent_info->disk_offset == 0) {
		btrfs_update_inode_bytes(inode, 0, bytes_to_drop);
		return 0;
	}

	btrfs_update_inode_bytes(inode, replace_len, bytes_to_drop);

	if (extent_info->is_new_extent && extent_info->insertions == 0) {
		key.objectid = extent_info->disk_offset;
		key.type = BTRFS_EXTENT_ITEM_KEY;
		key.offset = extent_info->disk_len;
		ret = btrfs_alloc_reserved_file_extent(trans, root,
						       btrfs_ino(inode),
						       extent_info->file_offset,
						       extent_info->qgroup_reserved,
						       &key);
	} else {
		struct btrfs_ref ref = {
			.action = BTRFS_ADD_DELAYED_REF,
			.bytenr = extent_info->disk_offset,
			.num_bytes = extent_info->disk_len,
			.owning_root = btrfs_root_id(root),
			.ref_root = btrfs_root_id(root),
		};
		u64 ref_offset;

		ref_offset = extent_info->file_offset - extent_info->data_offset;
		btrfs_init_data_ref(&ref, btrfs_ino(inode), ref_offset, 0, false);
		ret = btrfs_inc_extent_ref(trans, &ref);
	}

	extent_info->insertions++;

	return ret;
}

/*
 * The respective range must have been previously locked, as well as the inode.
 * The end offset is inclusive (last byte of the range).
 * @extent_info is NULL for fallocate's hole punching and non-NULL when replacing
 * the file range with an extent.
 * When not punching a hole, we don't want to end up in a state where we dropped
 * extents without inserting a new one, so we must abort the transaction to avoid
 * a corruption.
 */
int btrfs_replace_file_extents(struct btrfs_inode *inode,
			       struct btrfs_path *path, const u64 start,
			       const u64 end,
			       struct btrfs_replace_extent_info *extent_info,
			       struct btrfs_trans_handle **trans_out)
{
	struct btrfs_drop_extents_args drop_args = { 0 };
	struct btrfs_root *root = inode->root;
	struct btrfs_fs_info *fs_info = root->fs_info;
	u64 min_size = btrfs_calc_insert_metadata_size(fs_info, 1);
	u64 ino_size = round_up(inode->vfs_inode.i_size, fs_info->sectorsize);
	struct btrfs_trans_handle *trans = NULL;
	struct btrfs_block_rsv rsv;
	unsigned int rsv_count;
	u64 cur_offset;
	u64 len = end - start;
	int ret = 0;

	if (end <= start)
		return -EINVAL;

	btrfs_init_metadata_block_rsv(fs_info, &rsv, BTRFS_BLOCK_RSV_TEMP);
	rsv.size = btrfs_calc_insert_metadata_size(fs_info, 1);
	rsv.failfast = true;

	/*
	 * 1 - update the inode
	 * 1 - removing the extents in the range
	 * 1 - adding the hole extent if no_holes isn't set or if we are
	 *     replacing the range with a new extent
	 */
	if (!btrfs_fs_incompat(fs_info, NO_HOLES) || extent_info)
		rsv_count = 3;
	else
		rsv_count = 2;

	trans = btrfs_start_transaction(root, rsv_count);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		trans = NULL;
		goto out_release;
	}

	ret = btrfs_block_rsv_migrate(&fs_info->trans_block_rsv, &rsv,
				      min_size, false);
	if (WARN_ON(ret))
		goto out_trans;
	trans->block_rsv = &rsv;

	cur_offset = start;
	drop_args.path = path;
	drop_args.end = end + 1;
	drop_args.drop_cache = true;
	while (cur_offset < end) {
		drop_args.start = cur_offset;
		ret = btrfs_drop_extents(trans, root, inode, &drop_args);
		/* If we are punching a hole decrement the inode's byte count */
		if (!extent_info)
			btrfs_update_inode_bytes(inode, 0,
						 drop_args.bytes_found);
		if (ret != -ENOSPC) {
			/*
			 * The only time we don't want to abort is if we are
			 * attempting to clone a partial inline extent, in which
			 * case we'll get EOPNOTSUPP.  However if we aren't
			 * clone we need to abort no matter what, because if we
			 * got EOPNOTSUPP via prealloc then we messed up and
			 * need to abort.
			 */
			if (ret &&
			    (ret != -EOPNOTSUPP ||
			     (extent_info && extent_info->is_new_extent)))
				btrfs_abort_transaction(trans, ret);
			break;
		}

		trans->block_rsv = &fs_info->trans_block_rsv;

		if (!extent_info && cur_offset < drop_args.drop_end &&
		    cur_offset < ino_size) {
			ret = fill_holes(trans, inode, path, cur_offset,
					 drop_args.drop_end);
			if (ret) {
				/*
				 * If we failed then we didn't insert our hole
				 * entries for the area we dropped, so now the
				 * fs is corrupted, so we must abort the
				 * transaction.
				 */
				btrfs_abort_transaction(trans, ret);
				break;
			}
		} else if (!extent_info && cur_offset < drop_args.drop_end) {
			/*
			 * We are past the i_size here, but since we didn't
			 * insert holes we need to clear the mapped area so we
			 * know to not set disk_i_size in this area until a new
			 * file extent is inserted here.
			 */
			ret = btrfs_inode_clear_file_extent_range(inode,
					cur_offset,
					drop_args.drop_end - cur_offset);
			if (ret) {
				/*
				 * We couldn't clear our area, so we could
				 * presumably adjust up and corrupt the fs, so
				 * we need to abort.
				 */
				btrfs_abort_transaction(trans, ret);
				break;
			}
		}

		if (extent_info &&
		    drop_args.drop_end > extent_info->file_offset) {
			u64 replace_len = drop_args.drop_end -
					  extent_info->file_offset;

			ret = btrfs_insert_replace_extent(trans, inode,	path,
					extent_info, replace_len,
					drop_args.bytes_found);
			if (ret) {
				btrfs_abort_transaction(trans, ret);
				break;
			}
			extent_info->data_len -= replace_len;
			extent_info->data_offset += replace_len;
			extent_info->file_offset += replace_len;
		}

		/*
		 * We are releasing our handle on the transaction, balance the
		 * dirty pages of the btree inode and flush delayed items, and
		 * then get a new transaction handle, which may now point to a
		 * new transaction in case someone else may have committed the
		 * transaction we used to replace/drop file extent items. So
		 * bump the inode's iversion and update mtime and ctime except
		 * if we are called from a dedupe context. This is because a
		 * power failure/crash may happen after the transaction is
		 * committed and before we finish replacing/dropping all the
		 * file extent items we need.
		 */
		inode_inc_iversion(&inode->vfs_inode);

		if (!extent_info || extent_info->update_times)
			inode_set_mtime_to_ts(&inode->vfs_inode,
					      inode_set_ctime_current(&inode->vfs_inode));

		ret = btrfs_update_inode(trans, inode);
		if (ret)
			break;

		btrfs_end_transaction(trans);
		btrfs_btree_balance_dirty(fs_info);

		trans = btrfs_start_transaction(root, rsv_count);
		if (IS_ERR(trans)) {
			ret = PTR_ERR(trans);
			trans = NULL;
			break;
		}

		ret = btrfs_block_rsv_migrate(&fs_info->trans_block_rsv,
					      &rsv, min_size, false);
		if (WARN_ON(ret))
			break;
		trans->block_rsv = &rsv;

		cur_offset = drop_args.drop_end;
		len = end - cur_offset;
		if (!extent_info && len) {
			ret = find_first_non_hole(inode, &cur_offset, &len);
			if (unlikely(ret < 0))
				break;
			if (ret && !len) {
				ret = 0;
				break;
			}
		}
	}

	/*
	 * If we were cloning, force the next fsync to be a full one since we
	 * we replaced (or just dropped in the case of cloning holes when
	 * NO_HOLES is enabled) file extent items and did not setup new extent
	 * maps for the replacement extents (or holes).
	 */
	if (extent_info && !extent_info->is_new_extent)
		btrfs_set_inode_full_sync(inode);

	if (ret)
		goto out_trans;

	trans->block_rsv = &fs_info->trans_block_rsv;
	/*
	 * If we are using the NO_HOLES feature we might have had already an
	 * hole that overlaps a part of the region [lockstart, lockend] and
	 * ends at (or beyond) lockend. Since we have no file extent items to
	 * represent holes, drop_end can be less than lockend and so we must
	 * make sure we have an extent map representing the existing hole (the
	 * call to __btrfs_drop_extents() might have dropped the existing extent
	 * map representing the existing hole), otherwise the fast fsync path
	 * will not record the existence of the hole region
	 * [existing_hole_start, lockend].
	 */
	if (drop_args.drop_end <= end)
		drop_args.drop_end = end + 1;
	/*
	 * Don't insert file hole extent item if it's for a range beyond eof
	 * (because it's useless) or if it represents a 0 bytes range (when
	 * cur_offset == drop_end).
	 */
	if (!extent_info && cur_offset < ino_size &&
	    cur_offset < drop_args.drop_end) {
		ret = fill_holes(trans, inode, path, cur_offset,
				 drop_args.drop_end);
		if (ret) {
			/* Same comment as above. */
			btrfs_abort_transaction(trans, ret);
			goto out_trans;
		}
	} else if (!extent_info && cur_offset < drop_args.drop_end) {
		/* See the comment in the loop above for the reasoning here. */
		ret = btrfs_inode_clear_file_extent_range(inode, cur_offset,
					drop_args.drop_end - cur_offset);
		if (ret) {
			btrfs_abort_transaction(trans, ret);
			goto out_trans;
		}

	}
	if (extent_info) {
		ret = btrfs_insert_replace_extent(trans, inode, path,
				extent_info, extent_info->data_len,
				drop_args.bytes_found);
		if (ret) {
			btrfs_abort_transaction(trans, ret);
			goto out_trans;
		}
	}

out_trans:
	if (!trans)
		goto out_release;

	trans->block_rsv = &fs_info->trans_block_rsv;
	if (ret)
		btrfs_end_transaction(trans);
	else
		*trans_out = trans;
out_release:
	btrfs_block_rsv_release(fs_info, &rsv, (u64)-1, NULL);
	return ret;
}

static int btrfs_punch_hole(struct file *file, loff_t offset, loff_t len)
{
	struct inode *inode = file_inode(file);
	struct btrfs_fs_info *fs_info = inode_to_fs_info(inode);
	struct btrfs_root *root = BTRFS_I(inode)->root;
	struct extent_state *cached_state = NULL;
	struct btrfs_path *path;
	struct btrfs_trans_handle *trans = NULL;
	u64 lockstart;
	u64 lockend;
	u64 tail_start;
	u64 tail_len;
	const u64 orig_start = offset;
	const u64 orig_end = offset + len - 1;
	int ret = 0;
	bool same_block;
	u64 ino_size;
	bool truncated_block = false;
	bool updated_inode = false;

	btrfs_inode_lock(BTRFS_I(inode), BTRFS_ILOCK_MMAP);

	ret = btrfs_wait_ordered_range(BTRFS_I(inode), offset, len);
	if (ret)
		goto out_only_mutex;

	ino_size = round_up(inode->i_size, fs_info->sectorsize);
	ret = find_first_non_hole(BTRFS_I(inode), &offset, &len);
	if (ret < 0)
		goto out_only_mutex;
	if (ret && !len) {
		/* Already in a large hole */
		ret = 0;
		goto out_only_mutex;
	}

	ret = file_modified(file);
	if (ret)
		goto out_only_mutex;

	lockstart = round_up(offset, fs_info->sectorsize);
	lockend = round_down(offset + len, fs_info->sectorsize) - 1;
	same_block = (BTRFS_BYTES_TO_BLKS(fs_info, offset))
		== (BTRFS_BYTES_TO_BLKS(fs_info, offset + len - 1));
	/*
	 * Only do this if we are in the same block and we aren't doing the
	 * entire block.
	 */
	if (same_block && len < fs_info->sectorsize) {
		if (offset < ino_size) {
			truncated_block = true;
			ret = btrfs_truncate_block(BTRFS_I(inode), offset + len - 1,
						   orig_start, orig_end);
		} else {
			ret = 0;
		}
		goto out_only_mutex;
	}

	/* zero back part of the first block */
	if (offset < ino_size) {
		truncated_block = true;
		ret = btrfs_truncate_block(BTRFS_I(inode), offset, orig_start, orig_end);
		if (ret) {
			btrfs_inode_unlock(BTRFS_I(inode), BTRFS_ILOCK_MMAP);
			return ret;
		}
	}

	/* Check the aligned pages after the first unaligned page,
	 * if offset != orig_start, which means the first unaligned page
	 * including several following pages are already in holes,
	 * the extra check can be skipped */
	if (offset == orig_start) {
		/* after truncate page, check hole again */
		len = offset + len - lockstart;
		offset = lockstart;
		ret = find_first_non_hole(BTRFS_I(inode), &offset, &len);
		if (ret < 0)
			goto out_only_mutex;
		if (ret && !len) {
			ret = 0;
			goto out_only_mutex;
		}
		lockstart = offset;
	}

	/* Check the tail unaligned part is in a hole */
	tail_start = lockend + 1;
	tail_len = offset + len - tail_start;
	if (tail_len) {
		ret = find_first_non_hole(BTRFS_I(inode), &tail_start, &tail_len);
		if (unlikely(ret < 0))
			goto out_only_mutex;
		if (!ret) {
			/* zero the front end of the last page */
			if (tail_start + tail_len < ino_size) {
				truncated_block = true;
				ret = btrfs_truncate_block(BTRFS_I(inode),
							tail_start + tail_len - 1,
							orig_start, orig_end);
				if (ret)
					goto out_only_mutex;
			}
		}
	}

	if (lockend < lockstart) {
		ret = 0;
		goto out_only_mutex;
	}

	btrfs_punch_hole_lock_range(inode, lockstart, lockend, &cached_state);

	path = btrfs_alloc_path();
	if (!path) {
		ret = -ENOMEM;
		goto out;
	}

	ret = btrfs_replace_file_extents(BTRFS_I(inode), path, lockstart,
					 lockend, NULL, &trans);
	btrfs_free_path(path);
	if (ret)
		goto out;

	ASSERT(trans != NULL);
	inode_inc_iversion(inode);
	inode_set_mtime_to_ts(inode, inode_set_ctime_current(inode));
	ret = btrfs_update_inode(trans, BTRFS_I(inode));
	updated_inode = true;
	btrfs_end_transaction(trans);
	btrfs_btree_balance_dirty(fs_info);
out:
	btrfs_unlock_extent(&BTRFS_I(inode)->io_tree, lockstart, lockend,
			    &cached_state);
out_only_mutex:
	if (!updated_inode && truncated_block && !ret) {
		/*
		 * If we only end up zeroing part of a page, we still need to
		 * update the inode item, so that all the time fields are
		 * updated as well as the necessary btrfs inode in memory fields
		 * for detecting, at fsync time, if the inode isn't yet in the
		 * log tree or it's there but not up to date.
		 */
		struct timespec64 now = inode_set_ctime_current(inode);

		inode_inc_iversion(inode);
		inode_set_mtime_to_ts(inode, now);
		trans = btrfs_start_transaction(root, 1);
		if (IS_ERR(trans)) {
			ret = PTR_ERR(trans);
		} else {
			int ret2;

			ret = btrfs_update_inode(trans, BTRFS_I(inode));
			ret2 = btrfs_end_transaction(trans);
			if (!ret)
				ret = ret2;
		}
	}
	btrfs_inode_unlock(BTRFS_I(inode), BTRFS_ILOCK_MMAP);
	return ret;
}

/* Helper structure to record which range is already reserved */
struct falloc_range {
	struct list_head list;
	u64 start;
	u64 len;
};

/*
 * Helper function to add falloc range
 *
 * Caller should have locked the larger range of extent containing
 * [start, len)
 */
static int add_falloc_range(struct list_head *head, u64 start, u64 len)
{
	struct falloc_range *range = NULL;

	if (!list_empty(head)) {
		/*
		 * As fallocate iterates by bytenr order, we only need to check
		 * the last range.
		 */
		range = list_last_entry(head, struct falloc_range, list);
		if (range->start + range->len == start) {
			range->len += len;
			return 0;
		}
	}

	range = kmalloc(sizeof(*range), GFP_KERNEL);
	if (!range)
		return -ENOMEM;
	range->start = start;
	range->len = len;
	list_add_tail(&range->list, head);
	return 0;
}

static int btrfs_fallocate_update_isize(struct inode *inode,
					const u64 end,
					const int mode)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_root *root = BTRFS_I(inode)->root;
	int ret;
	int ret2;

	if (mode & FALLOC_FL_KEEP_SIZE || end <= i_size_read(inode))
		return 0;

	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans))
		return PTR_ERR(trans);

	inode_set_ctime_current(inode);
	i_size_write(inode, end);
	btrfs_inode_safe_disk_i_size_write(BTRFS_I(inode), 0);
	ret = btrfs_update_inode(trans, BTRFS_I(inode));
	ret2 = btrfs_end_transaction(trans);

	return ret ? ret : ret2;
}

enum {
	RANGE_BOUNDARY_WRITTEN_EXTENT,
	RANGE_BOUNDARY_PREALLOC_EXTENT,
	RANGE_BOUNDARY_HOLE,
};

static int btrfs_zero_range_check_range_boundary(struct btrfs_inode *inode,
						 u64 offset)
{
	const u64 sectorsize = inode->root->fs_info->sectorsize;
	struct extent_map *em;
	int ret;

	offset = round_down(offset, sectorsize);
	em = btrfs_get_extent(inode, NULL, offset, sectorsize);
	if (IS_ERR(em))
		return PTR_ERR(em);

	if (em->disk_bytenr == EXTENT_MAP_HOLE)
		ret = RANGE_BOUNDARY_HOLE;
	else if (em->flags & EXTENT_FLAG_PREALLOC)
		ret = RANGE_BOUNDARY_PREALLOC_EXTENT;
	else
		ret = RANGE_BOUNDARY_WRITTEN_EXTENT;

	btrfs_free_extent_map(em);
	return ret;
}

static int btrfs_zero_range(struct inode *inode,
			    loff_t offset,
			    loff_t len,
			    const int mode)
{
	struct btrfs_fs_info *fs_info = BTRFS_I(inode)->root->fs_info;
	struct extent_map *em;
	struct extent_changeset *data_reserved = NULL;
	int ret;
	u64 alloc_hint = 0;
	const u64 sectorsize = fs_info->sectorsize;
	const u64 orig_start = offset;
	const u64 orig_end = offset + len - 1;
	u64 alloc_start = round_down(offset, sectorsize);
	u64 alloc_end = round_up(offset + len, sectorsize);
	u64 bytes_to_reserve = 0;
	bool space_reserved = false;

	em = btrfs_get_extent(BTRFS_I(inode), NULL, alloc_start,
			      alloc_end - alloc_start);
	if (IS_ERR(em)) {
		ret = PTR_ERR(em);
		goto out;
	}

	/*
	 * Avoid hole punching and extent allocation for some cases. More cases
	 * could be considered, but these are unlikely common and we keep things
	 * as simple as possible for now. Also, intentionally, if the target
	 * range contains one or more prealloc extents together with regular
	 * extents and holes, we drop all the existing extents and allocate a
	 * new prealloc extent, so that we get a larger contiguous disk extent.
	 */
	if (em->start <= alloc_start && (em->flags & EXTENT_FLAG_PREALLOC)) {
		const u64 em_end = em->start + em->len;

		if (em_end >= offset + len) {
			/*
			 * The whole range is already a prealloc extent,
			 * do nothing except updating the inode's i_size if
			 * needed.
			 */
			btrfs_free_extent_map(em);
			ret = btrfs_fallocate_update_isize(inode, offset + len,
							   mode);
			goto out;
		}
		/*
		 * Part of the range is already a prealloc extent, so operate
		 * only on the remaining part of the range.
		 */
		alloc_start = em_end;
		ASSERT(IS_ALIGNED(alloc_start, sectorsize));
		len = offset + len - alloc_start;
		offset = alloc_start;
		alloc_hint = btrfs_extent_map_block_start(em) + em->len;
	}
	btrfs_free_extent_map(em);

	if (BTRFS_BYTES_TO_BLKS(fs_info, offset) ==
	    BTRFS_BYTES_TO_BLKS(fs_info, offset + len - 1)) {
		em = btrfs_get_extent(BTRFS_I(inode), NULL, alloc_start, sectorsize);
		if (IS_ERR(em)) {
			ret = PTR_ERR(em);
			goto out;
		}

		if (em->flags & EXTENT_FLAG_PREALLOC) {
			btrfs_free_extent_map(em);
			ret = btrfs_fallocate_update_isize(inode, offset + len,
							   mode);
			goto out;
		}
		if (len < sectorsize && em->disk_bytenr != EXTENT_MAP_HOLE) {
			btrfs_free_extent_map(em);
			ret = btrfs_truncate_block(BTRFS_I(inode), offset + len - 1,
						   orig_start, orig_end);
			if (!ret)
				ret = btrfs_fallocate_update_isize(inode,
								   offset + len,
								   mode);
			return ret;
		}
		btrfs_free_extent_map(em);
		alloc_start = round_down(offset, sectorsize);
		alloc_end = alloc_start + sectorsize;
		goto reserve_space;
	}

	alloc_start = round_up(offset, sectorsize);
	alloc_end = round_down(offset + len, sectorsize);

	/*
	 * For unaligned ranges, check the pages at the boundaries, they might
	 * map to an extent, in which case we need to partially zero them, or
	 * they might map to a hole, in which case we need our allocation range
	 * to cover them.
	 */
	if (!IS_ALIGNED(offset, sectorsize)) {
		ret = btrfs_zero_range_check_range_boundary(BTRFS_I(inode),
							    offset);
		if (ret < 0)
			goto out;
		if (ret == RANGE_BOUNDARY_HOLE) {
			alloc_start = round_down(offset, sectorsize);
			ret = 0;
		} else if (ret == RANGE_BOUNDARY_WRITTEN_EXTENT) {
			ret = btrfs_truncate_block(BTRFS_I(inode), offset,
						   orig_start, orig_end);
			if (ret)
				goto out;
		} else {
			ret = 0;
		}
	}

	if (!IS_ALIGNED(offset + len, sectorsize)) {
		ret = btrfs_zero_range_check_range_boundary(BTRFS_I(inode),
							    offset + len);
		if (ret < 0)
			goto out;
		if (ret == RANGE_BOUNDARY_HOLE) {
			alloc_end = round_up(offset + len, sectorsize);
			ret = 0;
		} else if (ret == RANGE_BOUNDARY_WRITTEN_EXTENT) {
			ret = btrfs_truncate_block(BTRFS_I(inode), offset + len - 1,
						   orig_start, orig_end);
			if (ret)
				goto out;
		} else {
			ret = 0;
		}
	}

reserve_space:
	if (alloc_start < alloc_end) {
		struct extent_state *cached_state = NULL;
		const u64 lockstart = alloc_start;
		const u64 lockend = alloc_end - 1;

		bytes_to_reserve = alloc_end - alloc_start;
		ret = btrfs_alloc_data_chunk_ondemand(BTRFS_I(inode),
						      bytes_to_reserve);
		if (ret < 0)
			goto out;
		space_reserved = true;
		btrfs_punch_hole_lock_range(inode, lockstart, lockend,
					    &cached_state);
		ret = btrfs_qgroup_reserve_data(BTRFS_I(inode), &data_reserved,
						alloc_start, bytes_to_reserve);
		if (ret) {
			btrfs_unlock_extent(&BTRFS_I(inode)->io_tree, lockstart,
					    lockend, &cached_state);
			goto out;
		}
		ret = btrfs_prealloc_file_range(inode, mode, alloc_start,
						alloc_end - alloc_start,
						fs_info->sectorsize,
						offset + len, &alloc_hint);
		btrfs_unlock_extent(&BTRFS_I(inode)->io_tree, lockstart, lockend,
				    &cached_state);
		/* btrfs_prealloc_file_range releases reserved space on error */
		if (ret) {
			space_reserved = false;
			goto out;
		}
	}
	ret = btrfs_fallocate_update_isize(inode, offset + len, mode);
 out:
	if (ret && space_reserved)
		btrfs_free_reserved_data_space(BTRFS_I(inode), data_reserved,
					       alloc_start, bytes_to_reserve);
	extent_changeset_free(data_reserved);

	return ret;
}

static long btrfs_fallocate(struct file *file, int mode,
			    loff_t offset, loff_t len)
{
	struct inode *inode = file_inode(file);
	struct extent_state *cached_state = NULL;
	struct extent_changeset *data_reserved = NULL;
	struct falloc_range *range;
	struct falloc_range *tmp;
	LIST_HEAD(reserve_list);
	u64 cur_offset;
	u64 last_byte;
	u64 alloc_start;
	u64 alloc_end;
	u64 alloc_hint = 0;
	u64 locked_end;
	u64 actual_end = 0;
	u64 data_space_needed = 0;
	u64 data_space_reserved = 0;
	u64 qgroup_reserved = 0;
	struct extent_map *em;
	int blocksize = BTRFS_I(inode)->root->fs_info->sectorsize;
	int ret;

	/* Do not allow fallocate in ZONED mode */
	if (btrfs_is_zoned(inode_to_fs_info(inode)))
		return -EOPNOTSUPP;

	alloc_start = round_down(offset, blocksize);
	alloc_end = round_up(offset + len, blocksize);
	cur_offset = alloc_start;

	/* Make sure we aren't being give some crap mode */
	if (mode & ~(FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE |
		     FALLOC_FL_ZERO_RANGE))
		return -EOPNOTSUPP;

	if (mode & FALLOC_FL_PUNCH_HOLE)
		return btrfs_punch_hole(file, offset, len);

	btrfs_inode_lock(BTRFS_I(inode), BTRFS_ILOCK_MMAP);

	if (!(mode & FALLOC_FL_KEEP_SIZE) && offset + len > inode->i_size) {
		ret = inode_newsize_ok(inode, offset + len);
		if (ret)
			goto out;
	}

	ret = file_modified(file);
	if (ret)
		goto out;

	/*
	 * TODO: Move these two operations after we have checked
	 * accurate reserved space, or fallocate can still fail but
	 * with page truncated or size expanded.
	 *
	 * But that's a minor problem and won't do much harm BTW.
	 */
	if (alloc_start > inode->i_size) {
		ret = btrfs_cont_expand(BTRFS_I(inode), i_size_read(inode),
					alloc_start);
		if (ret)
			goto out;
	} else if (offset + len > inode->i_size) {
		/*
		 * If we are fallocating from the end of the file onward we
		 * need to zero out the end of the block if i_size lands in the
		 * middle of a block.
		 */
		ret = btrfs_truncate_block(BTRFS_I(inode), inode->i_size,
					   inode->i_size, (u64)-1);
		if (ret)
			goto out;
	}

	/*
	 * We have locked the inode at the VFS level (in exclusive mode) and we
	 * have locked the i_mmap_lock lock (in exclusive mode). Now before
	 * locking the file range, flush all dealloc in the range and wait for
	 * all ordered extents in the range to complete. After this we can lock
	 * the file range and, due to the previous locking we did, we know there
	 * can't be more delalloc or ordered extents in the range.
	 */
	ret = btrfs_wait_ordered_range(BTRFS_I(inode), alloc_start,
				       alloc_end - alloc_start);
	if (ret)
		goto out;

	if (mode & FALLOC_FL_ZERO_RANGE) {
		ret = btrfs_zero_range(inode, offset, len, mode);
		btrfs_inode_unlock(BTRFS_I(inode), BTRFS_ILOCK_MMAP);
		return ret;
	}

	locked_end = alloc_end - 1;
	btrfs_lock_extent(&BTRFS_I(inode)->io_tree, alloc_start, locked_end,
			  &cached_state);

	btrfs_assert_inode_range_clean(BTRFS_I(inode), alloc_start, locked_end);

	/* First, check if we exceed the qgroup limit */
	while (cur_offset < alloc_end) {
		em = btrfs_get_extent(BTRFS_I(inode), NULL, cur_offset,
				      alloc_end - cur_offset);
		if (IS_ERR(em)) {
			ret = PTR_ERR(em);
			break;
		}
		last_byte = min(btrfs_extent_map_end(em), alloc_end);
		actual_end = min_t(u64, btrfs_extent_map_end(em), offset + len);
		last_byte = ALIGN(last_byte, blocksize);
		if (em->disk_bytenr == EXTENT_MAP_HOLE ||
		    (cur_offset >= inode->i_size &&
		     !(em->flags & EXTENT_FLAG_PREALLOC))) {
			const u64 range_len = last_byte - cur_offset;

			ret = add_falloc_range(&reserve_list, cur_offset, range_len);
			if (ret < 0) {
				btrfs_free_extent_map(em);
				break;
			}
			ret = btrfs_qgroup_reserve_data(BTRFS_I(inode),
					&data_reserved, cur_offset, range_len);
			if (ret < 0) {
				btrfs_free_extent_map(em);
				break;
			}
			qgroup_reserved += range_len;
			data_space_needed += range_len;
		}
		btrfs_free_extent_map(em);
		cur_offset = last_byte;
	}

	if (!ret && data_space_needed > 0) {
		/*
		 * We are safe to reserve space here as we can't have delalloc
		 * in the range, see above.
		 */
		ret = btrfs_alloc_data_chunk_ondemand(BTRFS_I(inode),
						      data_space_needed);
		if (!ret)
			data_space_reserved = data_space_needed;
	}

	/*
	 * If ret is still 0, means we're OK to fallocate.
	 * Or just cleanup the list and exit.
	 */
	list_for_each_entry_safe(range, tmp, &reserve_list, list) {
		if (!ret) {
			ret = btrfs_prealloc_file_range(inode, mode,
					range->start,
					range->len, blocksize,
					offset + len, &alloc_hint);
			/*
			 * btrfs_prealloc_file_range() releases space even
			 * if it returns an error.
			 */
			data_space_reserved -= range->len;
			qgroup_reserved -= range->len;
		} else if (data_space_reserved > 0) {
			btrfs_free_reserved_data_space(BTRFS_I(inode),
					       data_reserved, range->start,
					       range->len);
			data_space_reserved -= range->len;
			qgroup_reserved -= range->len;
		} else if (qgroup_reserved > 0) {
			btrfs_qgroup_free_data(BTRFS_I(inode), data_reserved,
					       range->start, range->len, NULL);
			qgroup_reserved -= range->len;
		}
		list_del(&range->list);
		kfree(range);
	}
	if (ret < 0)
		goto out_unlock;

	/*
	 * We didn't need to allocate any more space, but we still extended the
	 * size of the file so we need to update i_size and the inode item.
	 */
	ret = btrfs_fallocate_update_isize(inode, actual_end, mode);
out_unlock:
	btrfs_unlock_extent(&BTRFS_I(inode)->io_tree, alloc_start, locked_end,
			    &cached_state);
out:
	btrfs_inode_unlock(BTRFS_I(inode), BTRFS_ILOCK_MMAP);
	extent_changeset_free(data_reserved);
	return ret;
}

/*
 * Helper for btrfs_find_delalloc_in_range(). Find a subrange in a given range
 * that has unflushed and/or flushing delalloc. There might be other adjacent
 * subranges after the one it found, so btrfs_find_delalloc_in_range() keeps
 * looping while it gets adjacent subranges, and merging them together.
 */
static bool find_delalloc_subrange(struct btrfs_inode *inode, u64 start, u64 end,
				   struct extent_state **cached_state,
				   bool *search_io_tree,
				   u64 *delalloc_start_ret, u64 *delalloc_end_ret)
{
	u64 len = end + 1 - start;
	u64 delalloc_len = 0;
	struct btrfs_ordered_extent *oe;
	u64 oe_start;
	u64 oe_end;

	/*
	 * Search the io tree first for EXTENT_DELALLOC. If we find any, it
	 * means we have delalloc (dirty pages) for which writeback has not
	 * started yet.
	 */
	if (*search_io_tree) {
		spin_lock(&inode->lock);
		if (inode->delalloc_bytes > 0) {
			spin_unlock(&inode->lock);
			*delalloc_start_ret = start;
			delalloc_len = btrfs_count_range_bits(&inode->io_tree,
							      delalloc_start_ret, end,
							      len, EXTENT_DELALLOC, 1,
							      cached_state);
		} else {
			spin_unlock(&inode->lock);
		}
	}

	if (delalloc_len > 0) {
		/*
		 * If delalloc was found then *delalloc_start_ret has a sector size
		 * aligned value (rounded down).
		 */
		*delalloc_end_ret = *delalloc_start_ret + delalloc_len - 1;

		if (*delalloc_start_ret == start) {
			/* Delalloc for the whole range, nothing more to do. */
			if (*delalloc_end_ret == end)
				return true;
			/* Else trim our search range for ordered extents. */
			start = *delalloc_end_ret + 1;
			len = end + 1 - start;
		}
	} else {
		/* No delalloc, future calls don't need to search again. */
		*search_io_tree = false;
	}

	/*
	 * Now also check if there's any ordered extent in the range.
	 * We do this because:
	 *
	 * 1) When delalloc is flushed, the file range is locked, we clear the
	 *    EXTENT_DELALLOC bit from the io tree and create an extent map and
	 *    an ordered extent for the write. So we might just have been called
	 *    after delalloc is flushed and before the ordered extent completes
	 *    and inserts the new file extent item in the subvolume's btree;
	 *
	 * 2) We may have an ordered extent created by flushing delalloc for a
	 *    subrange that starts before the subrange we found marked with
	 *    EXTENT_DELALLOC in the io tree.
	 *
	 * We could also use the extent map tree to find such delalloc that is
	 * being flushed, but using the ordered extents tree is more efficient
	 * because it's usually much smaller as ordered extents are removed from
	 * the tree once they complete. With the extent maps, we mau have them
	 * in the extent map tree for a very long time, and they were either
	 * created by previous writes or loaded by read operations.
	 */
	oe = btrfs_lookup_first_ordered_range(inode, start, len);
	if (!oe)
		return (delalloc_len > 0);

	/* The ordered extent may span beyond our search range. */
	oe_start = max(oe->file_offset, start);
	oe_end = min(oe->file_offset + oe->num_bytes - 1, end);

	btrfs_put_ordered_extent(oe);

	/* Don't have unflushed delalloc, return the ordered extent range. */
	if (delalloc_len == 0) {
		*delalloc_start_ret = oe_start;
		*delalloc_end_ret = oe_end;
		return true;
	}

	/*
	 * We have both unflushed delalloc (io_tree) and an ordered extent.
	 * If the ranges are adjacent returned a combined range, otherwise
	 * return the leftmost range.
	 */
	if (oe_start < *delalloc_start_ret) {
		if (oe_end < *delalloc_start_ret)
			*delalloc_end_ret = oe_end;
		*delalloc_start_ret = oe_start;
	} else if (*delalloc_end_ret + 1 == oe_start) {
		*delalloc_end_ret = oe_end;
	}

	return true;
}

/*
 * Check if there's delalloc in a given range.
 *
 * @inode:               The inode.
 * @start:               The start offset of the range. It does not need to be
 *                       sector size aligned.
 * @end:                 The end offset (inclusive value) of the search range.
 *                       It does not need to be sector size aligned.
 * @cached_state:        Extent state record used for speeding up delalloc
 *                       searches in the inode's io_tree. Can be NULL.
 * @delalloc_start_ret:  Output argument, set to the start offset of the
 *                       subrange found with delalloc (may not be sector size
 *                       aligned).
 * @delalloc_end_ret:    Output argument, set to he end offset (inclusive value)
 *                       of the subrange found with delalloc.
 *
 * Returns true if a subrange with delalloc is found within the given range, and
 * if so it sets @delalloc_start_ret and @delalloc_end_ret with the start and
 * end offsets of the subrange.
 */
bool btrfs_find_delalloc_in_range(struct btrfs_inode *inode, u64 start, u64 end,
				  struct extent_state **cached_state,
				  u64 *delalloc_start_ret, u64 *delalloc_end_ret)
{
	u64 cur_offset = round_down(start, inode->root->fs_info->sectorsize);
	u64 prev_delalloc_end = 0;
	bool search_io_tree = true;
	bool ret = false;

	while (cur_offset <= end) {
		u64 delalloc_start;
		u64 delalloc_end;
		bool delalloc;

		delalloc = find_delalloc_subrange(inode, cur_offset, end,
						  cached_state, &search_io_tree,
						  &delalloc_start,
						  &delalloc_end);
		if (!delalloc)
			break;

		if (prev_delalloc_end == 0) {
			/* First subrange found. */
			*delalloc_start_ret = max(delalloc_start, start);
			*delalloc_end_ret = delalloc_end;
			ret = true;
		} else if (delalloc_start == prev_delalloc_end + 1) {
			/* Subrange adjacent to the previous one, merge them. */
			*delalloc_end_ret = delalloc_end;
		} else {
			/* Subrange not adjacent to the previous one, exit. */
			break;
		}

		prev_delalloc_end = delalloc_end;
		cur_offset = delalloc_end + 1;
		cond_resched();
	}

	return ret;
}

/*
 * Check if there's a hole or delalloc range in a range representing a hole (or
 * prealloc extent) found in the inode's subvolume btree.
 *
 * @inode:      The inode.
 * @whence:     Seek mode (SEEK_DATA or SEEK_HOLE).
 * @start:      Start offset of the hole region. It does not need to be sector
 *              size aligned.
 * @end:        End offset (inclusive value) of the hole region. It does not
 *              need to be sector size aligned.
 * @start_ret:  Return parameter, used to set the start of the subrange in the
 *              hole that matches the search criteria (seek mode), if such
 *              subrange is found (return value of the function is true).
 *              The value returned here may not be sector size aligned.
 *
 * Returns true if a subrange matching the given seek mode is found, and if one
 * is found, it updates @start_ret with the start of the subrange.
 */
static bool find_desired_extent_in_hole(struct btrfs_inode *inode, int whence,
					struct extent_state **cached_state,
					u64 start, u64 end, u64 *start_ret)
{
	u64 delalloc_start;
	u64 delalloc_end;
	bool delalloc;

	delalloc = btrfs_find_delalloc_in_range(inode, start, end, cached_state,
						&delalloc_start, &delalloc_end);
	if (delalloc && whence == SEEK_DATA) {
		*start_ret = delalloc_start;
		return true;
	}

	if (delalloc && whence == SEEK_HOLE) {
		/*
		 * We found delalloc but it starts after out start offset. So we
		 * have a hole between our start offset and the delalloc start.
		 */
		if (start < delalloc_start) {
			*start_ret = start;
			return true;
		}
		/*
		 * Delalloc range starts at our start offset.
		 * If the delalloc range's length is smaller than our range,
		 * then it means we have a hole that starts where the delalloc
		 * subrange ends.
		 */
		if (delalloc_end < end) {
			*start_ret = delalloc_end + 1;
			return true;
		}

		/* There's delalloc for the whole range. */
		return false;
	}

	if (!delalloc && whence == SEEK_HOLE) {
		*start_ret = start;
		return true;
	}

	/*
	 * No delalloc in the range and we are seeking for data. The caller has
	 * to iterate to the next extent item in the subvolume btree.
	 */
	return false;
}

static loff_t find_desired_extent(struct file *file, loff_t offset, int whence)
{
	struct btrfs_inode *inode = BTRFS_I(file->f_mapping->host);
	struct btrfs_file_private *private;
	struct btrfs_fs_info *fs_info = inode->root->fs_info;
	struct extent_state *cached_state = NULL;
	struct extent_state **delalloc_cached_state;
	const loff_t i_size = i_size_read(&inode->vfs_inode);
	const u64 ino = btrfs_ino(inode);
	struct btrfs_root *root = inode->root;
	struct btrfs_path *path;
	struct btrfs_key key;
	u64 last_extent_end;
	u64 lockstart;
	u64 lockend;
	u64 start;
	int ret;
	bool found = false;

	if (i_size == 0 || offset >= i_size)
		return -ENXIO;

	/*
	 * Quick path. If the inode has no prealloc extents and its number of
	 * bytes used matches its i_size, then it can not have holes.
	 */
	if (whence == SEEK_HOLE &&
	    !(inode->flags & BTRFS_INODE_PREALLOC) &&
	    inode_get_bytes(&inode->vfs_inode) == i_size)
		return i_size;

	spin_lock(&inode->lock);
	private = file->private_data;
	spin_unlock(&inode->lock);

	if (private && private->owner_task != current) {
		/*
		 * Not allocated by us, don't use it as its cached state is used
		 * by the task that allocated it and we don't want neither to
		 * mess with it nor get incorrect results because it reflects an
		 * invalid state for the current task.
		 */
		private = NULL;
	} else if (!private) {
		private = kzalloc(sizeof(*private), GFP_KERNEL);
		/*
		 * No worries if memory allocation failed.
		 * The private structure is used only for speeding up multiple
		 * lseek SEEK_HOLE/DATA calls to a file when there's delalloc,
		 * so everything will still be correct.
		 */
		if (private) {
			bool free = false;

			private->owner_task = current;

			spin_lock(&inode->lock);
			if (file->private_data)
				free = true;
			else
				file->private_data = private;
			spin_unlock(&inode->lock);

			if (free) {
				kfree(private);
				private = NULL;
			}
		}
	}

	if (private)
		delalloc_cached_state = &private->llseek_cached_state;
	else
		delalloc_cached_state = NULL;

	/*
	 * offset can be negative, in this case we start finding DATA/HOLE from
	 * the very start of the file.
	 */
	start = max_t(loff_t, 0, offset);

	lockstart = round_down(start, fs_info->sectorsize);
	lockend = round_up(i_size, fs_info->sectorsize);
	if (lockend <= lockstart)
		lockend = lockstart + fs_info->sectorsize;
	lockend--;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;
	path->reada = READA_FORWARD;

	key.objectid = ino;
	key.type = BTRFS_EXTENT_DATA_KEY;
	key.offset = start;

	last_extent_end = lockstart;

	btrfs_lock_extent(&inode->io_tree, lockstart, lockend, &cached_state);

	ret = btrfs_search_slot(NULL, root, &key, path, 0, 0);
	if (ret < 0) {
		goto out;
	} else if (ret > 0 && path->slots[0] > 0) {
		btrfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0] - 1);
		if (key.objectid == ino && key.type == BTRFS_EXTENT_DATA_KEY)
			path->slots[0]--;
	}

	while (start < i_size) {
		struct extent_buffer *leaf = path->nodes[0];
		struct btrfs_file_extent_item *extent;
		u64 extent_end;
		u8 type;

		if (path->slots[0] >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(root, path);
			if (ret < 0)
				goto out;
			else if (ret > 0)
				break;

			leaf = path->nodes[0];
		}

		btrfs_item_key_to_cpu(leaf, &key, path->slots[0]);
		if (key.objectid != ino || key.type != BTRFS_EXTENT_DATA_KEY)
			break;

		extent_end = btrfs_file_extent_end(path);

		/*
		 * In the first iteration we may have a slot that points to an
		 * extent that ends before our start offset, so skip it.
		 */
		if (extent_end <= start) {
			path->slots[0]++;
			continue;
		}

		/* We have an implicit hole, NO_HOLES feature is likely set. */
		if (last_extent_end < key.offset) {
			u64 search_start = last_extent_end;
			u64 found_start;

			/*
			 * First iteration, @start matches @offset and it's
			 * within the hole.
			 */
			if (start == offset)
				search_start = offset;

			found = find_desired_extent_in_hole(inode, whence,
							    delalloc_cached_state,
							    search_start,
							    key.offset - 1,
							    &found_start);
			if (found) {
				start = found_start;
				break;
			}
			/*
			 * Didn't find data or a hole (due to delalloc) in the
			 * implicit hole range, so need to analyze the extent.
			 */
		}

		extent = btrfs_item_ptr(leaf, path->slots[0],
					struct btrfs_file_extent_item);
		type = btrfs_file_extent_type(leaf, extent);

		/*
		 * Can't access the extent's disk_bytenr field if this is an
		 * inline extent, since at that offset, it's where the extent
		 * data starts.
		 */
		if (type == BTRFS_FILE_EXTENT_PREALLOC ||
		    (type == BTRFS_FILE_EXTENT_REG &&
		     btrfs_file_extent_disk_bytenr(leaf, extent) == 0)) {
			/*
			 * Explicit hole or prealloc extent, search for delalloc.
			 * A prealloc extent is treated like a hole.
			 */
			u64 search_start = key.offset;
			u64 found_start;

			/*
			 * First iteration, @start matches @offset and it's
			 * within the hole.
			 */
			if (start == offset)
				search_start = offset;

			found = find_desired_extent_in_hole(inode, whence,
							    delalloc_cached_state,
							    search_start,
							    extent_end - 1,
							    &found_start);
			if (found) {
				start = found_start;
				break;
			}
			/*
			 * Didn't find data or a hole (due to delalloc) in the
			 * implicit hole range, so need to analyze the next
			 * extent item.
			 */
		} else {
			/*
			 * Found a regular or inline extent.
			 * If we are seeking for data, adjust the start offset
			 * and stop, we're done.
			 */
			if (whence == SEEK_DATA) {
				start = max_t(u64, key.offset, offset);
				found = true;
				break;
			}
			/*
			 * Else, we are seeking for a hole, check the next file
			 * extent item.
			 */
		}

		start = extent_end;
		last_extent_end = extent_end;
		path->slots[0]++;
		if (fatal_signal_pending(current)) {
			ret = -EINTR;
			goto out;
		}
		cond_resched();
	}

	/* We have an implicit hole from the last extent found up to i_size. */
	if (!found && start < i_size) {
		found = find_desired_extent_in_hole(inode, whence,
						    delalloc_cached_state, start,
						    i_size - 1, &start);
		if (!found)
			start = i_size;
	}

out:
	btrfs_unlock_extent(&inode->io_tree, lockstart, lockend, &cached_state);
	btrfs_free_path(path);

	if (ret < 0)
		return ret;

	if (whence == SEEK_DATA && start >= i_size)
		return -ENXIO;

	return min_t(loff_t, start, i_size);
}

static loff_t btrfs_file_llseek(struct file *file, loff_t offset, int whence)
{
	struct inode *inode = file->f_mapping->host;

	switch (whence) {
	default:
		return generic_file_llseek(file, offset, whence);
	case SEEK_DATA:
	case SEEK_HOLE:
		btrfs_inode_lock(BTRFS_I(inode), BTRFS_ILOCK_SHARED);
		offset = find_desired_extent(file, offset, whence);
		btrfs_inode_unlock(BTRFS_I(inode), BTRFS_ILOCK_SHARED);
		break;
	}

	if (offset < 0)
		return offset;

	return vfs_setpos(file, offset, inode->i_sb->s_maxbytes);
}

static int btrfs_file_open(struct inode *inode, struct file *filp)
{
	int ret;

	filp->f_mode |= FMODE_NOWAIT | FMODE_CAN_ODIRECT;

	ret = fsverity_file_open(inode, filp);
	if (ret)
		return ret;
	return generic_file_open(inode, filp);
}

static ssize_t btrfs_file_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	ssize_t ret = 0;

	if (iocb->ki_flags & IOCB_DIRECT) {
		ret = btrfs_direct_read(iocb, to);
		if (ret < 0 || !iov_iter_count(to) ||
		    iocb->ki_pos >= i_size_read(file_inode(iocb->ki_filp)))
			return ret;
	}

	return filemap_read(iocb, to, ret);
}

const struct file_operations btrfs_file_operations = {
	.llseek		= btrfs_file_llseek,
	.read_iter      = btrfs_file_read_iter,
	.splice_read	= filemap_splice_read,
	.write_iter	= btrfs_file_write_iter,
	.splice_write	= iter_file_splice_write,
	.mmap_prepare	= btrfs_file_mmap_prepare,
	.open		= btrfs_file_open,
	.release	= btrfs_release_file,
	.get_unmapped_area = thp_get_unmapped_area,
	.fsync		= btrfs_sync_file,
	.fallocate	= btrfs_fallocate,
	.unlocked_ioctl	= btrfs_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= btrfs_compat_ioctl,
#endif
	.remap_file_range = btrfs_remap_file_range,
	.uring_cmd	= btrfs_uring_cmd,
	.fop_flags	= FOP_BUFFER_RASYNC | FOP_BUFFER_WASYNC,
};

int btrfs_fdatawrite_range(struct btrfs_inode *inode, loff_t start, loff_t end)
{
	struct address_space *mapping = inode->vfs_inode.i_mapping;
	int ret;

	/*
	 * So with compression we will find and lock a dirty page and clear the
	 * first one as dirty, setup an async extent, and immediately return
	 * with the entire range locked but with nobody actually marked with
	 * writeback.  So we can't just filemap_write_and_wait_range() and
	 * expect it to work since it will just kick off a thread to do the
	 * actual work.  So we need to call filemap_fdatawrite_range _again_
	 * since it will wait on the page lock, which won't be unlocked until
	 * after the pages have been marked as writeback and so we're good to go
	 * from there.  We have to do this otherwise we'll miss the ordered
	 * extents and that results in badness.  Please Josef, do not think you
	 * know better and pull this out at some point in the future, it is
	 * right and you are wrong.
	 */
	ret = filemap_fdatawrite_range(mapping, start, end);
	if (!ret && test_bit(BTRFS_INODE_HAS_ASYNC_EXTENT, &inode->runtime_flags))
		ret = filemap_fdatawrite_range(mapping, start, end);

	return ret;
}
