# SPDX-License-Identifier: GPL-2.0
#
# Makefile for io_uring

ifdef CONFIG_GCOV_PROFILE_URING
GCOV_PROFILE := y
endif

obj-$(CONFIG_IO_URING)		+= io_uring.o opdef.o kbuf.o rsrc.o notif.o \
					tctx.o filetable.o rw.o poll.o \
					eventfd.o uring_cmd.o openclose.o \
					sqpoll.o xattr.o nop.o fs.o splice.o \
					sync.o msg_ring.o advise.o openclose.o \
					statx.o timeout.o cancel.o \
					waitid.o register.o truncate.o \
					memmap.o alloc_cache.o
obj-$(CONFIG_IO_URING_ZCRX)	+= zcrx.o
obj-$(CONFIG_IO_WQ)		+= io-wq.o
obj-$(CONFIG_FUTEX)		+= futex.o
obj-$(CONFIG_EPOLL)		+= epoll.o
obj-$(CONFIG_NET_RX_BUSY_POLL)	+= napi.o
obj-$(CONFIG_NET) += net.o cmd_net.o
obj-$(CONFIG_PROC_FS) += fdinfo.o
obj-$(CONFIG_IO_URING_MOCK_FILE) += mock_file.o
