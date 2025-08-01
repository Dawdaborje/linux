// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/arm64/mm/hugetlbpage.c
 *
 * Copyright (C) 2013 Linaro Ltd.
 *
 * Based on arch/x86/mm/hugetlbpage.c.
 */

#include <linux/init.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/hugetlb.h>
#include <linux/pagemap.h>
#include <linux/err.h>
#include <linux/sysctl.h>
#include <asm/mman.h>
#include <asm/tlb.h>
#include <asm/tlbflush.h>

/*
 * HugeTLB Support Matrix
 *
 * ---------------------------------------------------
 * | Page Size | CONT PTE |  PMD  | CONT PMD |  PUD  |
 * ---------------------------------------------------
 * |     4K    |   64K    |   2M  |    32M   |   1G  |
 * |    16K    |    2M    |  32M  |     1G   |       |
 * |    64K    |    2M    | 512M  |    16G   |       |
 * ---------------------------------------------------
 */

/*
 * Reserve CMA areas for the largest supported gigantic
 * huge page when requested. Any other smaller gigantic
 * huge pages could still be served from those areas.
 */
#ifdef CONFIG_CMA
void __init arm64_hugetlb_cma_reserve(void)
{
	int order;

	if (pud_sect_supported())
		order = PUD_SHIFT - PAGE_SHIFT;
	else
		order = CONT_PMD_SHIFT - PAGE_SHIFT;

	hugetlb_cma_reserve(order);
}
#endif /* CONFIG_CMA */

static bool __hugetlb_valid_size(unsigned long size)
{
	switch (size) {
#ifndef __PAGETABLE_PMD_FOLDED
	case PUD_SIZE:
		return pud_sect_supported();
#endif
	case CONT_PMD_SIZE:
	case PMD_SIZE:
	case CONT_PTE_SIZE:
		return true;
	}

	return false;
}

#ifdef CONFIG_ARCH_ENABLE_HUGEPAGE_MIGRATION
bool arch_hugetlb_migration_supported(struct hstate *h)
{
	size_t pagesize = huge_page_size(h);

	if (!__hugetlb_valid_size(pagesize)) {
		pr_warn("%s: unrecognized huge page size 0x%lx\n",
			__func__, pagesize);
		return false;
	}
	return true;
}
#endif

static int find_num_contig(struct mm_struct *mm, unsigned long addr,
			   pte_t *ptep, size_t *pgsize)
{
	pgd_t *pgdp = pgd_offset(mm, addr);
	p4d_t *p4dp;
	pud_t *pudp;
	pmd_t *pmdp;

	*pgsize = PAGE_SIZE;
	p4dp = p4d_offset(pgdp, addr);
	pudp = pud_offset(p4dp, addr);
	pmdp = pmd_offset(pudp, addr);
	if ((pte_t *)pmdp == ptep) {
		*pgsize = PMD_SIZE;
		return CONT_PMDS;
	}
	return CONT_PTES;
}

static inline int num_contig_ptes(unsigned long size, size_t *pgsize)
{
	int contig_ptes = 1;

	*pgsize = size;

	switch (size) {
	case CONT_PMD_SIZE:
		*pgsize = PMD_SIZE;
		contig_ptes = CONT_PMDS;
		break;
	case CONT_PTE_SIZE:
		*pgsize = PAGE_SIZE;
		contig_ptes = CONT_PTES;
		break;
	default:
		WARN_ON(!__hugetlb_valid_size(size));
	}

	return contig_ptes;
}

pte_t huge_ptep_get(struct mm_struct *mm, unsigned long addr, pte_t *ptep)
{
	int ncontig, i;
	size_t pgsize;
	pte_t orig_pte = __ptep_get(ptep);

	if (!pte_present(orig_pte) || !pte_cont(orig_pte))
		return orig_pte;

	ncontig = find_num_contig(mm, addr, ptep, &pgsize);
	for (i = 0; i < ncontig; i++, ptep++) {
		pte_t pte = __ptep_get(ptep);

		if (pte_dirty(pte))
			orig_pte = pte_mkdirty(orig_pte);

		if (pte_young(pte))
			orig_pte = pte_mkyoung(orig_pte);
	}
	return orig_pte;
}

/*
 * Changing some bits of contiguous entries requires us to follow a
 * Break-Before-Make approach, breaking the whole contiguous set
 * before we can change any entries. See ARM DDI 0487A.k_iss10775,
 * "Misprogramming of the Contiguous bit", page D4-1762.
 *
 * This helper performs the break step.
 */
static pte_t get_clear_contig(struct mm_struct *mm,
			     unsigned long addr,
			     pte_t *ptep,
			     unsigned long pgsize,
			     unsigned long ncontig)
{
	pte_t pte, tmp_pte;
	bool present;

	pte = __ptep_get_and_clear_anysz(mm, ptep, pgsize);
	present = pte_present(pte);
	while (--ncontig) {
		ptep++;
		tmp_pte = __ptep_get_and_clear_anysz(mm, ptep, pgsize);
		if (present) {
			if (pte_dirty(tmp_pte))
				pte = pte_mkdirty(pte);
			if (pte_young(tmp_pte))
				pte = pte_mkyoung(pte);
		}
	}
	return pte;
}

static pte_t get_clear_contig_flush(struct mm_struct *mm,
				    unsigned long addr,
				    pte_t *ptep,
				    unsigned long pgsize,
				    unsigned long ncontig)
{
	pte_t orig_pte = get_clear_contig(mm, addr, ptep, pgsize, ncontig);
	struct vm_area_struct vma = TLB_FLUSH_VMA(mm, 0);
	unsigned long end = addr + (pgsize * ncontig);

	__flush_hugetlb_tlb_range(&vma, addr, end, pgsize, true);
	return orig_pte;
}

/*
 * Changing some bits of contiguous entries requires us to follow a
 * Break-Before-Make approach, breaking the whole contiguous set
 * before we can change any entries. See ARM DDI 0487A.k_iss10775,
 * "Misprogramming of the Contiguous bit", page D4-1762.
 *
 * This helper performs the break step for use cases where the
 * original pte is not needed.
 */
static void clear_flush(struct mm_struct *mm,
			     unsigned long addr,
			     pte_t *ptep,
			     unsigned long pgsize,
			     unsigned long ncontig)
{
	struct vm_area_struct vma = TLB_FLUSH_VMA(mm, 0);
	unsigned long i, saddr = addr;

	for (i = 0; i < ncontig; i++, addr += pgsize, ptep++)
		__ptep_get_and_clear_anysz(mm, ptep, pgsize);

	if (mm == &init_mm)
		flush_tlb_kernel_range(saddr, addr);
	else
		__flush_hugetlb_tlb_range(&vma, saddr, addr, pgsize, true);
}

void set_huge_pte_at(struct mm_struct *mm, unsigned long addr,
			    pte_t *ptep, pte_t pte, unsigned long sz)
{
	size_t pgsize;
	int i;
	int ncontig;

	ncontig = num_contig_ptes(sz, &pgsize);

	if (!pte_present(pte)) {
		for (i = 0; i < ncontig; i++, ptep++)
			__set_ptes_anysz(mm, ptep, pte, 1, pgsize);
		return;
	}

	/* Only need to "break" if transitioning valid -> valid. */
	if (pte_cont(pte) && pte_valid(__ptep_get(ptep)))
		clear_flush(mm, addr, ptep, pgsize, ncontig);

	__set_ptes_anysz(mm, ptep, pte, ncontig, pgsize);
}

pte_t *huge_pte_alloc(struct mm_struct *mm, struct vm_area_struct *vma,
		      unsigned long addr, unsigned long sz)
{
	pgd_t *pgdp;
	p4d_t *p4dp;
	pud_t *pudp;
	pmd_t *pmdp;
	pte_t *ptep = NULL;

	pgdp = pgd_offset(mm, addr);
	p4dp = p4d_alloc(mm, pgdp, addr);
	if (!p4dp)
		return NULL;

	pudp = pud_alloc(mm, p4dp, addr);
	if (!pudp)
		return NULL;

	if (sz == PUD_SIZE) {
		ptep = (pte_t *)pudp;
	} else if (sz == (CONT_PTE_SIZE)) {
		pmdp = pmd_alloc(mm, pudp, addr);
		if (!pmdp)
			return NULL;

		WARN_ON(addr & (sz - 1));
		ptep = pte_alloc_huge(mm, pmdp, addr);
	} else if (sz == PMD_SIZE) {
		if (want_pmd_share(vma, addr) && pud_none(READ_ONCE(*pudp)))
			ptep = huge_pmd_share(mm, vma, addr, pudp);
		else
			ptep = (pte_t *)pmd_alloc(mm, pudp, addr);
	} else if (sz == (CONT_PMD_SIZE)) {
		pmdp = pmd_alloc(mm, pudp, addr);
		WARN_ON(addr & (sz - 1));
		return (pte_t *)pmdp;
	}

	return ptep;
}

pte_t *huge_pte_offset(struct mm_struct *mm,
		       unsigned long addr, unsigned long sz)
{
	pgd_t *pgdp;
	p4d_t *p4dp;
	pud_t *pudp, pud;
	pmd_t *pmdp, pmd;

	pgdp = pgd_offset(mm, addr);
	if (!pgd_present(READ_ONCE(*pgdp)))
		return NULL;

	p4dp = p4d_offset(pgdp, addr);
	if (!p4d_present(READ_ONCE(*p4dp)))
		return NULL;

	pudp = pud_offset(p4dp, addr);
	pud = READ_ONCE(*pudp);
	if (sz != PUD_SIZE && pud_none(pud))
		return NULL;
	/* hugepage or swap? */
	if (pud_leaf(pud) || !pud_present(pud))
		return (pte_t *)pudp;
	/* table; check the next level */

	if (sz == CONT_PMD_SIZE)
		addr &= CONT_PMD_MASK;

	pmdp = pmd_offset(pudp, addr);
	pmd = READ_ONCE(*pmdp);
	if (!(sz == PMD_SIZE || sz == CONT_PMD_SIZE) &&
	    pmd_none(pmd))
		return NULL;
	if (pmd_leaf(pmd) || !pmd_present(pmd))
		return (pte_t *)pmdp;

	if (sz == CONT_PTE_SIZE)
		return pte_offset_huge(pmdp, (addr & CONT_PTE_MASK));

	return NULL;
}

unsigned long hugetlb_mask_last_page(struct hstate *h)
{
	unsigned long hp_size = huge_page_size(h);

	switch (hp_size) {
#ifndef __PAGETABLE_PMD_FOLDED
	case PUD_SIZE:
		if (pud_sect_supported())
			return PGDIR_SIZE - PUD_SIZE;
		break;
#endif
	case CONT_PMD_SIZE:
		return PUD_SIZE - CONT_PMD_SIZE;
	case PMD_SIZE:
		return PUD_SIZE - PMD_SIZE;
	case CONT_PTE_SIZE:
		return PMD_SIZE - CONT_PTE_SIZE;
	default:
		break;
	}

	return 0UL;
}

pte_t arch_make_huge_pte(pte_t entry, unsigned int shift, vm_flags_t flags)
{
	size_t pagesize = 1UL << shift;

	switch (pagesize) {
#ifndef __PAGETABLE_PMD_FOLDED
	case PUD_SIZE:
		if (pud_sect_supported())
			return pud_pte(pud_mkhuge(pte_pud(entry)));
		break;
#endif
	case CONT_PMD_SIZE:
		return pmd_pte(pmd_mkhuge(pmd_mkcont(pte_pmd(entry))));
	case PMD_SIZE:
		return pmd_pte(pmd_mkhuge(pte_pmd(entry)));
	case CONT_PTE_SIZE:
		return pte_mkcont(entry);
	default:
		break;
	}
	pr_warn("%s: unrecognized huge page size 0x%lx\n",
		__func__, pagesize);
	return entry;
}

void huge_pte_clear(struct mm_struct *mm, unsigned long addr,
		    pte_t *ptep, unsigned long sz)
{
	int i, ncontig;
	size_t pgsize;

	ncontig = num_contig_ptes(sz, &pgsize);

	for (i = 0; i < ncontig; i++, addr += pgsize, ptep++)
		__pte_clear(mm, addr, ptep);
}

pte_t huge_ptep_get_and_clear(struct mm_struct *mm, unsigned long addr,
			      pte_t *ptep, unsigned long sz)
{
	int ncontig;
	size_t pgsize;

	ncontig = num_contig_ptes(sz, &pgsize);
	return get_clear_contig(mm, addr, ptep, pgsize, ncontig);
}

/*
 * huge_ptep_set_access_flags will update access flags (dirty, accesssed)
 * and write permission.
 *
 * For a contiguous huge pte range we need to check whether or not write
 * permission has to change only on the first pte in the set. Then for
 * all the contiguous ptes we need to check whether or not there is a
 * discrepancy between dirty or young.
 */
static int __cont_access_flags_changed(pte_t *ptep, pte_t pte, int ncontig)
{
	int i;

	if (pte_write(pte) != pte_write(__ptep_get(ptep)))
		return 1;

	for (i = 0; i < ncontig; i++) {
		pte_t orig_pte = __ptep_get(ptep + i);

		if (pte_dirty(pte) != pte_dirty(orig_pte))
			return 1;

		if (pte_young(pte) != pte_young(orig_pte))
			return 1;
	}

	return 0;
}

int huge_ptep_set_access_flags(struct vm_area_struct *vma,
			       unsigned long addr, pte_t *ptep,
			       pte_t pte, int dirty)
{
	int ncontig;
	size_t pgsize = 0;
	struct mm_struct *mm = vma->vm_mm;
	pte_t orig_pte;

	VM_WARN_ON(!pte_present(pte));

	if (!pte_cont(pte))
		return __ptep_set_access_flags(vma, addr, ptep, pte, dirty);

	ncontig = num_contig_ptes(huge_page_size(hstate_vma(vma)), &pgsize);

	if (!__cont_access_flags_changed(ptep, pte, ncontig))
		return 0;

	orig_pte = get_clear_contig_flush(mm, addr, ptep, pgsize, ncontig);
	VM_WARN_ON(!pte_present(orig_pte));

	/* Make sure we don't lose the dirty or young state */
	if (pte_dirty(orig_pte))
		pte = pte_mkdirty(pte);

	if (pte_young(orig_pte))
		pte = pte_mkyoung(pte);

	__set_ptes_anysz(mm, ptep, pte, ncontig, pgsize);
	return 1;
}

void huge_ptep_set_wrprotect(struct mm_struct *mm,
			     unsigned long addr, pte_t *ptep)
{
	int ncontig;
	size_t pgsize;
	pte_t pte;

	pte = __ptep_get(ptep);
	VM_WARN_ON(!pte_present(pte));

	if (!pte_cont(pte)) {
		__ptep_set_wrprotect(mm, addr, ptep);
		return;
	}

	ncontig = find_num_contig(mm, addr, ptep, &pgsize);

	pte = get_clear_contig_flush(mm, addr, ptep, pgsize, ncontig);
	pte = pte_wrprotect(pte);

	__set_ptes_anysz(mm, ptep, pte, ncontig, pgsize);
}

pte_t huge_ptep_clear_flush(struct vm_area_struct *vma,
			    unsigned long addr, pte_t *ptep)
{
	struct mm_struct *mm = vma->vm_mm;
	size_t pgsize;
	int ncontig;

	ncontig = num_contig_ptes(huge_page_size(hstate_vma(vma)), &pgsize);
	return get_clear_contig_flush(mm, addr, ptep, pgsize, ncontig);
}

static int __init hugetlbpage_init(void)
{
	/*
	 * HugeTLB pages are supported on maximum four page table
	 * levels (PUD, CONT PMD, PMD, CONT PTE) for a given base
	 * page size, corresponding to hugetlb_add_hstate() calls
	 * here.
	 *
	 * HUGE_MAX_HSTATE should at least match maximum supported
	 * HugeTLB page sizes on the platform. Any new addition to
	 * supported HugeTLB page sizes will also require changing
	 * HUGE_MAX_HSTATE as well.
	 */
	BUILD_BUG_ON(HUGE_MAX_HSTATE < 4);
	if (pud_sect_supported())
		hugetlb_add_hstate(PUD_SHIFT - PAGE_SHIFT);

	hugetlb_add_hstate(CONT_PMD_SHIFT - PAGE_SHIFT);
	hugetlb_add_hstate(PMD_SHIFT - PAGE_SHIFT);
	hugetlb_add_hstate(CONT_PTE_SHIFT - PAGE_SHIFT);

	return 0;
}
arch_initcall(hugetlbpage_init);

bool __init arch_hugetlb_valid_size(unsigned long size)
{
	return __hugetlb_valid_size(size);
}

pte_t huge_ptep_modify_prot_start(struct vm_area_struct *vma, unsigned long addr, pte_t *ptep)
{
	unsigned long psize = huge_page_size(hstate_vma(vma));

	if (alternative_has_cap_unlikely(ARM64_WORKAROUND_2645198)) {
		/*
		 * Break-before-make (BBM) is required for all user space mappings
		 * when the permission changes from executable to non-executable
		 * in cases where cpu is affected with errata #2645198.
		 */
		if (pte_user_exec(__ptep_get(ptep)))
			return huge_ptep_clear_flush(vma, addr, ptep);
	}
	return huge_ptep_get_and_clear(vma->vm_mm, addr, ptep, psize);
}

void huge_ptep_modify_prot_commit(struct vm_area_struct *vma, unsigned long addr, pte_t *ptep,
				  pte_t old_pte, pte_t pte)
{
	unsigned long psize = huge_page_size(hstate_vma(vma));

	set_huge_pte_at(vma->vm_mm, addr, ptep, pte, psize);
}
