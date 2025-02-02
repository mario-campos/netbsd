/*	$NetBSD: pmap_segtab.c,v 1.28 2022/09/25 06:21:58 skrll Exp $	*/

/*-
 * Copyright (c) 1998, 2001 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Jason R. Thorpe of the Numerical Aerospace Simulation Facility,
 * NASA Ames Research Center and by Chris G. Demetriou.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Copyright (c) 1992, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * the Systems Programming Group of the University of Utah Computer
 * Science Department and Ralph Campbell.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)pmap.c	8.4 (Berkeley) 1/26/94
 */

#include <sys/cdefs.h>

__KERNEL_RCSID(0, "$NetBSD: pmap_segtab.c,v 1.28 2022/09/25 06:21:58 skrll Exp $");

/*
 *	Manages physical address maps.
 *
 *	In addition to hardware address maps, this
 *	module is called upon to provide software-use-only
 *	maps which may or may not be stored in the same
 *	form as hardware maps.  These pseudo-maps are
 *	used to store intermediate results from copy
 *	operations to and from address spaces.
 *
 *	Since the information managed by this module is
 *	also stored by the logical address mapping module,
 *	this module may throw away valid virtual-to-physical
 *	mappings at almost any time.  However, invalidations
 *	of virtual-to-physical mappings must be done as
 *	requested.
 *
 *	In order to cope with hardware architectures which
 *	make virtual-to-physical map invalidates expensive,
 *	this module may delay invalidate or reduced protection
 *	operations until such time as they are actually
 *	necessary.  This module is given full information as
 *	to which processors are currently using which maps,
 *	and to when physical maps must be made correct.
 */

#define __PMAP_PRIVATE

#include "opt_multiprocessor.h"

#include <sys/param.h>

#include <sys/atomic.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/systm.h>

#include <uvm/uvm.h>

CTASSERT(NBPG >= sizeof(pmap_segtab_t));

struct pmap_segtab_info {
	pmap_segtab_t *free_segtab;	/* free list kept locally */
#ifdef DEBUG
	uint32_t nget_segtab;
	uint32_t nput_segtab;
	uint32_t npage_segtab;
#define	SEGTAB_ADD(n, v)	(pmap_segtab_info.n ## _segtab += (v))
#else
#define	SEGTAB_ADD(n, v)	((void) 0)
#endif
#ifdef PMAP_PTP_CACHE
	struct pgflist ptp_pgflist;	/* Keep a list of idle page tables. */
#endif
} pmap_segtab_info = {
#ifdef PMAP_PTP_CACHE
	.ptp_pgflist = LIST_HEAD_INITIALIZER(pmap_segtab_info.ptp_pgflist),
#endif
};

kmutex_t pmap_segtab_lock __cacheline_aligned;

/*
 * Check that a seg_tab[] array is empty.
 *
 * This is used when allocating or freeing a pmap_segtab_t.  The stb
 * should be unused -- meaning, none of the seg_tab[] pointers are
 * not NULL, as it transitions from either freshly allocated segtab from
 * pmap pool, an unused allocated page segtab alloc from the SMP case,
 * where two CPUs attempt to allocate the same underlying segtab, the
 * release of a segtab entry to the freelist, or for SMP, where reserve
 * also frees a freshly allocated but unused entry.
 */
static void
pmap_check_stb(pmap_segtab_t *stb, const char *caller, const char *why)
{
#ifdef DEBUG
	for (size_t i = 0; i < PMAP_SEGTABSIZE; i++) {
		if (stb->seg_tab[i] != NULL) {
#define DEBUG_NOISY
#ifdef DEBUG_NOISY
			UVMHIST_FUNC(__func__);
			UVMHIST_CALLARGS(pmapsegtabhist, "stb=%#jx",
			    (uintptr_t)stb, 0, 0, 0);
			for (size_t j = i; j < PMAP_SEGTABSIZE; j++)
				if (stb->seg_tab[j] != NULL)
					printf("%s: stb->seg_tab[%zu] = %p\n",
					    caller, j, stb->seg_tab[j]);
#endif
			panic("%s: pm_segtab.seg_tab[%zu] != 0 (%p): %s",
			    caller, i, stb->seg_tab[i], why);
		}
	}
#endif
}

/*
 * Check that an array of ptes is actually zero.
 */
static void
pmap_check_ptes(pt_entry_t *pte, const char *caller)
{
	/*
	 * All pte arrays should be page aligned.
	 */
	if (((uintptr_t)pte & PAGE_MASK) != 0) {
		panic("%s: pte entry at %p not page aligned", caller, pte);
	}

#ifdef DEBUG
	for (size_t i = 0; i < NPTEPG; i++)
		if (pte[i] != 0) {
#ifdef DEBUG_NOISY
			UVMHIST_FUNC(__func__);
			UVMHIST_CALLARGS(pmapsegtabhist, "pte=%#jx",
			    (uintptr_t)pte, 0, 0, 0);
			for (size_t j = i + 1; j < NPTEPG; j++)
				if (pte[j] != 0)
					UVMHIST_LOG(pmapsegtabhist,
					    "pte[%zu] = %#"PRIxPTE,
					    j, pte_value(pte[j]), 0, 0);
#endif
			panic("%s: pte[%zu] entry at %p not 0 (%#"PRIxPTE")",
			    caller, i, &pte[i], pte_value(pte[i]));
		}
#endif
}

static inline struct vm_page *
pmap_pte_pagealloc(void)
{
	struct vm_page *pg;

	pg = PMAP_ALLOC_POOLPAGE(UVM_PGA_ZERO|UVM_PGA_USERESERVE);
	if (pg) {
#ifdef UVM_PAGE_TRKOWN
		pg->owner_tag = NULL;
#endif
		UVM_PAGE_OWN(pg, "pmap-ptp");
	}

	return pg;
}

static inline pt_entry_t *
pmap_segmap(struct pmap *pmap, vaddr_t va)
{
	pmap_segtab_t *stb = pmap->pm_segtab;
	KASSERTMSG(pmap != pmap_kernel() || !pmap_md_direct_mapped_vaddr_p(va),
	    "pmap %p va %#" PRIxVADDR, pmap, va);
#ifdef _LP64
	stb = stb->seg_seg[(va >> XSEGSHIFT) & (NSEGPG - 1)];
	if (stb == NULL)
		return NULL;
#endif

	return stb->seg_tab[(va >> SEGSHIFT) & (PMAP_SEGTABSIZE - 1)];
}

pt_entry_t *
pmap_pte_lookup(pmap_t pmap, vaddr_t va)
{
	pt_entry_t *pte = pmap_segmap(pmap, va);
	if (pte == NULL)
		return NULL;

	return pte + ((va >> PGSHIFT) & (NPTEPG - 1));
}

/*
 * Insert the segtab into the segtab freelist.
 */
static void
pmap_segtab_free(pmap_segtab_t *stb)
{
	UVMHIST_FUNC(__func__);

	UVMHIST_CALLARGS(pmapsegtabhist, "stb=%#jx", (uintptr_t)stb, 0, 0, 0);

	mutex_spin_enter(&pmap_segtab_lock);
	stb->seg_seg[0] = pmap_segtab_info.free_segtab;
	pmap_segtab_info.free_segtab = stb;
	SEGTAB_ADD(nput, 1);
	mutex_spin_exit(&pmap_segtab_lock);
}

static void
pmap_segtab_release(pmap_t pmap, pmap_segtab_t **stb_p, bool free_stb,
	pte_callback_t callback, uintptr_t flags,
	vaddr_t va, vsize_t vinc)
{
	pmap_segtab_t *stb = *stb_p;

	UVMHIST_FUNC(__func__);
	UVMHIST_CALLARGS(pmapsegtabhist, "pm=%#jx stb_p=%#jx free=%jd",
	    (uintptr_t)pmap, (uintptr_t)stb_p, free_stb, 0);
	UVMHIST_LOG(pmapsegtabhist, " callback=%#jx flags=%#jx va=%#jx vinc=%#jx",
	    (uintptr_t)callback, flags, (uintptr_t)va, (uintptr_t)vinc);
	for (size_t i = (va / vinc) & (PMAP_SEGTABSIZE - 1);
	     i < PMAP_SEGTABSIZE;
	     i++, va += vinc) {
#ifdef _LP64
		if (vinc > NBSEG) {
			if (stb->seg_seg[i] != NULL) {
				UVMHIST_LOG(pmapsegtabhist,
				    " recursing %jd", i, 0, 0, 0);
				pmap_segtab_release(pmap, &stb->seg_seg[i],
				    true, callback, flags, va, vinc / NSEGPG);
				KASSERT(stb->seg_seg[i] == NULL);
			}
			continue;
		}
#endif
		KASSERT(vinc == NBSEG);

		/* get pointer to segment map */
		pt_entry_t *pte = stb->seg_tab[i];
		if (pte == NULL)
			continue;
		pmap_check_ptes(pte, __func__);

		/*
		 * If our caller wants a callback, do so.
		 */
		if (callback != NULL) {
			(*callback)(pmap, va, va + vinc, pte, flags);
		}

		// PMAP_UNMAP_POOLPAGE should handle any VCA issues itself
		paddr_t pa = PMAP_UNMAP_POOLPAGE((vaddr_t)pte);
		struct vm_page *pg = PHYS_TO_VM_PAGE(pa);
#ifdef PMAP_PTP_CACHE
		mutex_spin_enter(&pmap_segtab_lock);
		LIST_INSERT_HEAD(&pmap_segtab_info.ptp_pgflist, pg, pageq.list);
		mutex_spin_exit(&pmap_segtab_lock);
#else
		uvm_pagefree(pg);
#endif

		stb->seg_tab[i] = NULL;
		UVMHIST_LOG(pmapsegtabhist, " zeroing tab[%jd]", i, 0, 0, 0);
	}

	if (free_stb) {
		pmap_check_stb(stb, __func__,
			       vinc == NBSEG ? "release seg" : "release xseg");
		pmap_segtab_free(stb);
		*stb_p = NULL;
	}
}

/*
 *	Create and return a physical map.
 *
 *	If the size specified for the map
 *	is zero, the map is an actual physical
 *	map, and may be referenced by the
 *	hardware.
 *
 *	If the size specified is non-zero,
 *	the map will be used in software only, and
 *	is bounded by that size.
 */
static pmap_segtab_t *
pmap_segtab_alloc(void)
{
	pmap_segtab_t *stb;
	bool found_on_freelist = false;

	UVMHIST_FUNC(__func__);
 again:
	mutex_spin_enter(&pmap_segtab_lock);
	if (__predict_true((stb = pmap_segtab_info.free_segtab) != NULL)) {
		pmap_segtab_info.free_segtab = stb->seg_seg[0];
		stb->seg_seg[0] = NULL;
		SEGTAB_ADD(nget, 1);
		found_on_freelist = true;
		UVMHIST_CALLARGS(pmapsegtabhist, "freelist stb=%#jx",
		    (uintptr_t)stb, 0, 0, 0);
	}
	mutex_spin_exit(&pmap_segtab_lock);

	if (__predict_false(stb == NULL)) {
		struct vm_page * const stb_pg = pmap_pte_pagealloc();

		if (__predict_false(stb_pg == NULL)) {
			/*
			 * XXX What else can we do?  Could we deadlock here?
			 */
			uvm_wait("segtab");
			goto again;
		}
		SEGTAB_ADD(npage, 1);
		const paddr_t stb_pa = VM_PAGE_TO_PHYS(stb_pg);

		stb = (pmap_segtab_t *)PMAP_MAP_POOLPAGE(stb_pa);
		UVMHIST_CALLARGS(pmapsegtabhist, "new stb=%#jx",
		    (uintptr_t)stb, 0, 0, 0);
		const size_t n = NBPG / sizeof(*stb);
		if (n > 1) {
			/*
			 * link all the segtabs in this page together
			 */
			for (size_t i = 1; i < n - 1; i++) {
				stb[i].seg_seg[0] = &stb[i+1];
			}
			/*
			 * Now link the new segtabs into the free segtab list.
			 */
			mutex_spin_enter(&pmap_segtab_lock);
			stb[n-1].seg_seg[0] = pmap_segtab_info.free_segtab;
			pmap_segtab_info.free_segtab = stb + 1;
			SEGTAB_ADD(nput, n - 1);
			mutex_spin_exit(&pmap_segtab_lock);
		}
	}

	pmap_check_stb(stb, __func__,
		       found_on_freelist ? "from free list" : "allocated");

	return stb;
}

/*
 * Allocate the top segment table for the pmap.
 */
void
pmap_segtab_init(pmap_t pmap)
{

	pmap->pm_segtab = pmap_segtab_alloc();
}

/*
 *	Retire the given physical map from service.
 *	Should only be called if the map contains
 *	no valid mappings.
 */
void
pmap_segtab_destroy(pmap_t pmap, pte_callback_t func, uintptr_t flags)
{
	if (pmap->pm_segtab == NULL)
		return;

#ifdef _LP64
	const vsize_t vinc = NBXSEG;
#else
	const vsize_t vinc = NBSEG;
#endif
	pmap_segtab_release(pmap, &pmap->pm_segtab,
	    func == NULL, func, flags, pmap->pm_minaddr, vinc);
}

/*
 *	Make a new pmap (vmspace) active for the given process.
 */
void
pmap_segtab_activate(struct pmap *pm, struct lwp *l)
{
	if (l == curlwp) {
		struct cpu_info * const ci = l->l_cpu;
		pmap_md_xtab_activate(pm, l);
		KASSERT(pm == l->l_proc->p_vmspace->vm_map.pmap);
		if (pm == pmap_kernel()) {
			ci->ci_pmap_user_segtab = PMAP_INVALID_SEGTAB_ADDRESS;
#ifdef _LP64
			ci->ci_pmap_user_seg0tab = PMAP_INVALID_SEGTAB_ADDRESS;
#endif
		} else {
			ci->ci_pmap_user_segtab = pm->pm_segtab;
#ifdef _LP64
			ci->ci_pmap_user_seg0tab = pm->pm_segtab->seg_seg[0];
#endif
		}
	}
}


void
pmap_segtab_deactivate(pmap_t pm)
{

	pmap_md_xtab_deactivate(pm);

	curcpu()->ci_pmap_user_segtab = PMAP_INVALID_SEGTAB_ADDRESS;
#ifdef _LP64
	curcpu()->ci_pmap_user_seg0tab = NULL;
#endif

}

/*
 *	Act on the given range of addresses from the specified map.
 *
 *	It is assumed that the start and end are properly rounded to
 *	the page size.
 */
void
pmap_pte_process(pmap_t pmap, vaddr_t sva, vaddr_t eva,
    pte_callback_t callback, uintptr_t flags)
{
#if 0
	printf("%s: %p, %"PRIxVADDR", %"PRIxVADDR", %p, %"PRIxPTR"\n",
	    __func__, pmap, sva, eva, callback, flags);
#endif
	while (sva < eva) {
		vaddr_t lastseg_va = pmap_trunc_seg(sva) + NBSEG;
		if (lastseg_va == 0 || lastseg_va > eva)
			lastseg_va = eva;

		/*
		 * If VA belongs to an unallocated segment,
		 * skip to the next segment boundary.
		 */
		pt_entry_t * const ptep = pmap_pte_lookup(pmap, sva);
		if (ptep != NULL) {
			/*
			 * Callback to deal with the ptes for this segment.
			 */
			(*callback)(pmap, sva, lastseg_va, ptep, flags);
		}
		/*
		 * In theory we could release pages with no entries,
		 * but that takes more effort than we want here.
		 */
		sva = lastseg_va;
	}
}

/*
 *	Return a pointer for the pte that corresponds to the specified virtual
 *	address (va) in the target physical map, allocating if needed.
 */
pt_entry_t *
pmap_pte_reserve(pmap_t pmap, vaddr_t va, int flags)
{
	pmap_segtab_t *stb = pmap->pm_segtab;
	pt_entry_t *pte;
	UVMHIST_FUNC(__func__);

	pte = pmap_pte_lookup(pmap, va);
	if (__predict_false(pte == NULL)) {
#ifdef _LP64
		pmap_segtab_t ** const stb_p =
		    &stb->seg_seg[(va >> XSEGSHIFT) & (NSEGPG - 1)];
		if (__predict_false((stb = *stb_p) == NULL)) {
			pmap_segtab_t *nstb = pmap_segtab_alloc();
#ifdef MULTIPROCESSOR
			pmap_segtab_t *ostb = atomic_cas_ptr(stb_p, NULL, nstb);
			if (__predict_false(ostb != NULL)) {
				pmap_check_stb(nstb, __func__, "reserve");
				pmap_segtab_free(nstb);
				nstb = ostb;
			}
#else
			*stb_p = nstb;
#endif /* MULTIPROCESSOR */
			stb = nstb;
		}
		KASSERT(stb == pmap->pm_segtab->seg_seg[(va >> XSEGSHIFT) & (NSEGPG - 1)]);
#endif /* _LP64 */
		struct vm_page *pg = NULL;
#ifdef PMAP_PTP_CACHE
		mutex_spin_enter(&pmap_segtab_lock);
		if ((pg = LIST_FIRST(&pmap_segtab_info.ptp_pgflist)) != NULL) {
			LIST_REMOVE(pg, pageq.list);
			KASSERT(LIST_FIRST(&pmap_segtab_info.ptp_pgflist) != pg);
		}
		mutex_spin_exit(&pmap_segtab_lock);
#endif
		if (pg == NULL)
			pg = pmap_pte_pagealloc();
		if (pg == NULL) {
			if (flags & PMAP_CANFAIL)
				return NULL;
			panic("%s: cannot allocate page table page "
			    "for va %" PRIxVADDR, __func__, va);
		}

		const paddr_t pa = VM_PAGE_TO_PHYS(pg);
		pte = (pt_entry_t *)PMAP_MAP_POOLPAGE(pa);
		pt_entry_t ** const pte_p =
		    &stb->seg_tab[(va >> SEGSHIFT) & (PMAP_SEGTABSIZE - 1)];
#ifdef MULTIPROCESSOR
		pt_entry_t *opte = atomic_cas_ptr(pte_p, NULL, pte);
		/*
		 * If another thread allocated the segtab needed for this va
		 * free the page we just allocated.
		 */
		if (__predict_false(opte != NULL)) {
#ifdef PMAP_PTP_CACHE
			mutex_spin_enter(&pmap_segtab_lock);
			LIST_INSERT_HEAD(&pmap_segtab_info.ptp_pgflist,
			    pg, pageq.list);
			mutex_spin_exit(&pmap_segtab_lock);
#else
			PMAP_UNMAP_POOLPAGE((vaddr_t)pte);
			uvm_pagefree(pg);
#endif
			pte = opte;
		}
#else
		*pte_p = pte;
#endif
		KASSERT(pte == stb->seg_tab[(va >> SEGSHIFT) & (PMAP_SEGTABSIZE - 1)]);
		UVMHIST_CALLARGS(pmapsegtabhist, "pm=%#jx va=%#jx -> tab[%jd]=%#jx",
		    (uintptr_t)pmap, (uintptr_t)va,
		    (va >> SEGSHIFT) & (PMAP_SEGTABSIZE - 1), (uintptr_t)pte);

		pmap_check_ptes(pte, __func__);
		pte += (va >> PGSHIFT) & (NPTEPG - 1);
	}

	return pte;
}
