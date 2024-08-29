/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2019 Western Digital Corporation or its affiliates.
 *
 * Authors:
 *   Atish Patra <atish.patra@wdc.com>
 *   Anup Patel <anup.patel@wdc.com>
 */

#include <sbi/riscv_asm.h>
#include <sbi/riscv_atomic.h>
#include <sbi/riscv_barrier.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_fifo.h>
#include <sbi/sbi_hart.h>
#include <sbi/sbi_heap.h>
#include <sbi/sbi_ipi.h>
#include <sbi/sbi_scratch.h>
#include <sbi/sbi_tlb.h>
#include <sbi/sbi_hfence.h>
#include <sbi/sbi_string.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_platform.h>
#include <sbi/sbi_pmu.h>

static unsigned long tlb_sync_off;
static unsigned long tlb_fifo_off;
static unsigned long tlb_fifo_mem_off;
static unsigned long tlb_range_flush_limit;

static void tlb_flush_all(void)
{
	__asm__ __volatile("sfence.vma");
}

static void sbi_tlb_local_hfence_vvma(struct sbi_tlb_info *tinfo)
{
	unsigned long start = tinfo->start;
	unsigned long size  = tinfo->size;
	unsigned long vmid  = tinfo->vmid;
	unsigned long i, hgatp;

	sbi_pmu_ctr_incr_fw(SBI_PMU_FW_HFENCE_VVMA_RCVD);

	hgatp = csr_swap(CSR_HGATP,
			 (vmid << HGATP_VMID_SHIFT) & HGATP_VMID_MASK);

	if ((start == 0 && size == 0) || (size == SBI_TLB_FLUSH_ALL)) {
		__sbi_hfence_vvma_all();
		goto done;
	}

	for (i = 0; i < size; i += PAGE_SIZE) {
		__sbi_hfence_vvma_va(start+i);
	}

done:
	csr_write(CSR_HGATP, hgatp);
}

static void sbi_tlb_local_hfence_gvma(struct sbi_tlb_info *tinfo)
{
	unsigned long start = tinfo->start;
	unsigned long size  = tinfo->size;
	unsigned long i;

	sbi_pmu_ctr_incr_fw(SBI_PMU_FW_HFENCE_GVMA_RCVD);

	if ((start == 0 && size == 0) || (size == SBI_TLB_FLUSH_ALL)) {
		__sbi_hfence_gvma_all();
		return;
	}

	for (i = 0; i < size; i += PAGE_SIZE) {
		__sbi_hfence_gvma_gpa((start + i) >> 2);
	}
}

static void sbi_tlb_local_sfence_vma(struct sbi_tlb_info *tinfo)
{
#ifndef CONFIG_THEAD_C9XX_ERRATA_JTLB
	unsigned long start = tinfo->start;
	unsigned long size  = tinfo->size;
	unsigned long i;

	sbi_pmu_ctr_incr_fw(SBI_PMU_FW_SFENCE_VMA_RCVD);

	if ((start == 0 && size == 0) || (size == SBI_TLB_FLUSH_ALL)) {
		tlb_flush_all();
		return;
	}

	for (i = 0; i < size; i += PAGE_SIZE) {
		__asm__ __volatile__("sfence.vma %0"
				     :
				     : "r"(start + i)
				     : "memory");
	}
#else
	sbi_pmu_ctr_incr_fw(SBI_PMU_FW_SFENCE_VMA_RCVD);

	tlb_flush_all();
#endif
}

static void sbi_tlb_local_hfence_vvma_asid(struct sbi_tlb_info *tinfo)
{
	unsigned long start = tinfo->start;
	unsigned long size  = tinfo->size;
	unsigned long asid  = tinfo->asid;
	unsigned long vmid  = tinfo->vmid;
	unsigned long i, hgatp;

	sbi_pmu_ctr_incr_fw(SBI_PMU_FW_HFENCE_VVMA_ASID_RCVD);

	hgatp = csr_swap(CSR_HGATP,
			 (vmid << HGATP_VMID_SHIFT) & HGATP_VMID_MASK);

	if ((start == 0 && size == 0) || (size == SBI_TLB_FLUSH_ALL)) {
		__sbi_hfence_vvma_asid(asid);
		goto done;
	}

	for (i = 0; i < size; i += PAGE_SIZE) {
		__sbi_hfence_vvma_asid_va(start + i, asid);
	}

done:
	csr_write(CSR_HGATP, hgatp);
}

static void sbi_tlb_local_hfence_gvma_vmid(struct sbi_tlb_info *tinfo)
{
	unsigned long start = tinfo->start;
	unsigned long size  = tinfo->size;
	unsigned long vmid  = tinfo->vmid;
	unsigned long i;

	sbi_pmu_ctr_incr_fw(SBI_PMU_FW_HFENCE_GVMA_VMID_RCVD);

	if ((start == 0 && size == 0) || (size == SBI_TLB_FLUSH_ALL)) {
		__sbi_hfence_gvma_vmid(vmid);
		return;
	}

	for (i = 0; i < size; i += PAGE_SIZE) {
		__sbi_hfence_gvma_vmid_gpa((start + i) >> 2, vmid);
	}
}

static void sbi_tlb_local_sfence_vma_asid(struct sbi_tlb_info *tinfo)
{
#ifndef CONFIG_THEAD_C9XX_ERRATA_JTLB
	unsigned long start = tinfo->start;
	unsigned long size  = tinfo->size;
	unsigned long asid  = tinfo->asid;
	unsigned long i;

	sbi_pmu_ctr_incr_fw(SBI_PMU_FW_SFENCE_VMA_ASID_RCVD);

	/* Flush entire MM context for a given ASID */
	if ((start == 0 && size == 0) || (size == SBI_TLB_FLUSH_ALL)) {
		__asm__ __volatile__("sfence.vma x0, %0"
				     :
				     : "r"(asid)
				     : "memory");
		return;
	}

	for (i = 0; i < size; i += PAGE_SIZE) {
		__asm__ __volatile__("sfence.vma %0, %1"
				     :
				     : "r"(start + i), "r"(asid)
				     : "memory");
	}
#else
	unsigned long asid  = tinfo->asid;

	sbi_pmu_ctr_incr_fw(SBI_PMU_FW_SFENCE_VMA_ASID_RCVD);

	__asm__ __volatile__("sfence.vma x0, %0"
			:
			: "r"(asid)
			: "memory");
#endif
}

static void sbi_tlb_local_fence_i(struct sbi_tlb_info *tinfo)
{
	sbi_pmu_ctr_incr_fw(SBI_PMU_FW_FENCE_I_RECVD);

	__asm__ __volatile("fence.i");
}

static void tlb_entry_local_process(struct sbi_tlb_info *data)
{
	if (unlikely(!data))
		return;

	switch (data->type) {
	case SBI_TLB_FENCE_I:
		sbi_tlb_local_fence_i(data);
		break;
	case SBI_TLB_SFENCE_VMA:
		sbi_tlb_local_sfence_vma(data);
		break;
	case SBI_TLB_SFENCE_VMA_ASID:
		sbi_tlb_local_sfence_vma_asid(data);
		break;
	case SBI_TLB_HFENCE_GVMA_VMID:
		sbi_tlb_local_hfence_gvma_vmid(data);
		break;
	case SBI_TLB_HFENCE_GVMA:
		sbi_tlb_local_hfence_gvma(data);
		break;
	case SBI_TLB_HFENCE_VVMA_ASID:
		sbi_tlb_local_hfence_vvma_asid(data);
		break;
	case SBI_TLB_HFENCE_VVMA:
		sbi_tlb_local_hfence_vvma(data);
		break;
	default:
		break;
	};
}

static void tlb_entry_process(struct sbi_tlb_info *tinfo)
{
	u32 rindex;
	struct sbi_scratch *rscratch = NULL;
	atomic_t *rtlb_sync = NULL;

	tlb_entry_local_process(tinfo);

	sbi_hartmask_for_each_hartindex(rindex, &tinfo->smask) {
		rscratch = sbi_hartindex_to_scratch(rindex);
		if (!rscratch)
			continue;

		rtlb_sync = sbi_scratch_offset_ptr(rscratch, tlb_sync_off);
		atomic_sub_return(rtlb_sync, 1);
	}
}

static bool tlb_process_once(struct sbi_scratch *scratch)
{
	struct sbi_tlb_info tinfo;
	struct sbi_fifo *tlb_fifo =
			sbi_scratch_offset_ptr(scratch, tlb_fifo_off);

	if (!sbi_fifo_dequeue(tlb_fifo, &tinfo)) {
		tlb_entry_process(&tinfo);
		return true;
	}

	return false;
}

static void tlb_process(struct sbi_scratch *scratch)
{
	while (tlb_process_once(scratch));
}

static void tlb_sync(struct sbi_scratch *scratch)
{
	atomic_t *tlb_sync =
			sbi_scratch_offset_ptr(scratch, tlb_sync_off);

	while (atomic_read(tlb_sync) > 0) {
		/*
		 * While we are waiting for remote hart to set the sync,
		 * consume fifo requests to avoid deadlock.
		 */
		tlb_process_once(scratch);
	}

	return;
}

static inline int tlb_range_check(struct sbi_tlb_info *curr,
					struct sbi_tlb_info *next)
{
	unsigned long curr_end;
	unsigned long next_end;
	int ret = SBI_FIFO_UNCHANGED;

	if (!curr || !next)
		return ret;

	next_end = next->start + next->size;
	curr_end = curr->start + curr->size;
	if (next->start <= curr->start && next_end > curr_end) {
		curr->start = next->start;
		curr->size  = next->size;
		sbi_hartmask_or(&curr->smask, &curr->smask, &next->smask);
		ret = SBI_FIFO_UPDATED;
	} else if (next->start >= curr->start && next_end <= curr_end) {
		sbi_hartmask_or(&curr->smask, &curr->smask, &next->smask);
		ret = SBI_FIFO_SKIP;
	}

	return ret;
}

/**
 * Call back to decide if an inplace fifo update is required or next entry can
 * can be skipped. Here are the different cases that are being handled.
 *
 * Case1:
 *	if next flush request range lies within one of the existing entry, skip
 *	the next entry.
 * Case2:
 *	if flush request range in current fifo entry lies within next flush
 *	request, update the current entry.
 *
 * Note:
 *	We can not issue a fifo reset anymore if a complete vma flush is requested.
 *	This is because we are queueing FENCE.I requests as well now.
 *	To ease up the pressure in enqueue/fifo sync path, try to dequeue 1 element
 *	before continuing the while loop. This method is preferred over wfi/ipi because
 *	of MMIO cost involved in later method.
 */
static int tlb_update_cb(void *in, void *data)
{
	struct sbi_tlb_info *curr;
	struct sbi_tlb_info *next;
	int ret = SBI_FIFO_UNCHANGED;

	if (!in || !data)
		return ret;

	curr = (struct sbi_tlb_info *)data;
	next = (struct sbi_tlb_info *)in;

	if (next->type == SBI_TLB_SFENCE_VMA_ASID &&
	    curr->type == SBI_TLB_SFENCE_VMA_ASID) {
		if (next->asid == curr->asid)
			ret = tlb_range_check(curr, next);
	} else if (next->type == SBI_TLB_SFENCE_VMA &&
		   curr->type == SBI_TLB_SFENCE_VMA) {
		ret = tlb_range_check(curr, next);
	}

	return ret;
}

static int tlb_update(struct sbi_scratch *scratch,
			  struct sbi_scratch *remote_scratch,
			  u32 remote_hartindex, void *data)
{
	int ret;
	atomic_t *tlb_sync;
	struct sbi_fifo *tlb_fifo_r;
	struct sbi_tlb_info *tinfo = data;
	u32 curr_hartid = current_hartid();

	/*
	 * If the request is to queue a tlb flush entry for itself
	 * then just do a local flush and return;
	 */
	if (sbi_hartindex_to_hartid(remote_hartindex) == curr_hartid) {
		tlb_entry_local_process(tinfo);
		return SBI_IPI_UPDATE_BREAK;
	}

	tlb_fifo_r = sbi_scratch_offset_ptr(remote_scratch, tlb_fifo_off);

	ret = sbi_fifo_inplace_update(tlb_fifo_r, data, tlb_update_cb);

	if (ret == SBI_FIFO_UNCHANGED &&
	    sbi_fifo_enqueue(tlb_fifo_r, data, false) < 0) {
		/**
		 * For now, Busy loop until there is space in the fifo.
		 * There may be case where target hart is also
		 * enqueue in source hart's fifo. Both hart may busy
		 * loop leading to a deadlock.
		 * TODO: Introduce a wait/wakeup event mechanism to handle
		 * this properly.
		 */
		tlb_process_once(scratch);
		sbi_dprintf("hart%d: hart%d tlb fifo full\n", curr_hartid,
			    sbi_hartindex_to_hartid(remote_hartindex));
		return SBI_IPI_UPDATE_RETRY;
	}

	tlb_sync = sbi_scratch_offset_ptr(scratch, tlb_sync_off);
	atomic_add_return(tlb_sync, 1);

	return SBI_IPI_UPDATE_SUCCESS;
}

static struct sbi_ipi_event_ops tlb_ops = {
	.name = "IPI_TLB",
	.update = tlb_update,
	.sync = tlb_sync,
	.process = tlb_process,
};

static u32 tlb_event = SBI_IPI_EVENT_MAX;

static const u32 tlb_type_to_pmu_fw_event[SBI_TLB_TYPE_MAX] = {
	[SBI_TLB_FENCE_I] = SBI_PMU_FW_FENCE_I_SENT,
	[SBI_TLB_SFENCE_VMA] = SBI_PMU_FW_SFENCE_VMA_SENT,
	[SBI_TLB_SFENCE_VMA_ASID] = SBI_PMU_FW_SFENCE_VMA_ASID_SENT,
	[SBI_TLB_HFENCE_GVMA_VMID] = SBI_PMU_FW_HFENCE_GVMA_VMID_SENT,
	[SBI_TLB_HFENCE_GVMA] = SBI_PMU_FW_HFENCE_GVMA_SENT,
	[SBI_TLB_HFENCE_VVMA_ASID] = SBI_PMU_FW_HFENCE_VVMA_ASID_SENT,
	[SBI_TLB_HFENCE_VVMA] = SBI_PMU_FW_HFENCE_VVMA_SENT,
};

int sbi_tlb_request(ulong hmask, ulong hbase, struct sbi_tlb_info *tinfo)
{
	if (tinfo->type < 0 || tinfo->type >= SBI_TLB_TYPE_MAX)
		return SBI_EINVAL;

	/*
	 * If address range to flush is too big then simply
	 * upgrade it to flush all because we can only flush
	 * 4KB at a time.
	 */
	if (tinfo->size > tlb_range_flush_limit) {
		tinfo->start = 0;
		tinfo->size = SBI_TLB_FLUSH_ALL;
	}

	sbi_pmu_ctr_incr_fw(tlb_type_to_pmu_fw_event[tinfo->type]);

	return sbi_ipi_send_many(hmask, hbase, tlb_event, tinfo);
}

int sbi_tlb_init(struct sbi_scratch *scratch, bool cold_boot)
{
	int ret;
	void *tlb_mem;
	atomic_t *tlb_sync;
	struct sbi_fifo *tlb_q;
	const struct sbi_platform *plat = sbi_platform_ptr(scratch);

	if (cold_boot) {
		tlb_sync_off = sbi_scratch_alloc_offset(sizeof(*tlb_sync));
		if (!tlb_sync_off)
			return SBI_ENOMEM;
		tlb_fifo_off = sbi_scratch_alloc_offset(sizeof(*tlb_q));
		if (!tlb_fifo_off) {
			sbi_scratch_free_offset(tlb_sync_off);
			return SBI_ENOMEM;
		}
		tlb_fifo_mem_off = sbi_scratch_alloc_offset(sizeof(tlb_mem));
		if (!tlb_fifo_mem_off) {
			sbi_scratch_free_offset(tlb_fifo_off);
			sbi_scratch_free_offset(tlb_sync_off);
			return SBI_ENOMEM;
		}
		ret = sbi_ipi_event_create(&tlb_ops);
		if (ret < 0) {
			sbi_scratch_free_offset(tlb_fifo_mem_off);
			sbi_scratch_free_offset(tlb_fifo_off);
			sbi_scratch_free_offset(tlb_sync_off);
			return ret;
		}
		tlb_event = ret;
		tlb_range_flush_limit = sbi_platform_tlbr_flush_limit(plat);
	} else {
		if (!tlb_sync_off ||
		    !tlb_fifo_off ||
		    !tlb_fifo_mem_off)
			return SBI_ENOMEM;
		if (SBI_IPI_EVENT_MAX <= tlb_event)
			return SBI_ENOSPC;
	}

	tlb_sync = sbi_scratch_offset_ptr(scratch, tlb_sync_off);
	tlb_q = sbi_scratch_offset_ptr(scratch, tlb_fifo_off);
	tlb_mem = sbi_scratch_read_type(scratch, void *, tlb_fifo_mem_off);
	if (!tlb_mem) {
		tlb_mem = sbi_malloc(
				sbi_platform_tlb_fifo_num_entries(plat) * SBI_TLB_INFO_SIZE);
		if (!tlb_mem)
			return SBI_ENOMEM;
		sbi_scratch_write_type(scratch, void *, tlb_fifo_mem_off, tlb_mem);
	}

	ATOMIC_INIT(tlb_sync, 0);

	sbi_fifo_init(tlb_q, tlb_mem,
		      sbi_platform_tlb_fifo_num_entries(plat), SBI_TLB_INFO_SIZE);

	return 0;
}
