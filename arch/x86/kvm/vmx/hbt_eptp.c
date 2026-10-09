// SPDX-License-Identifier: GPL-2.0
/*
 * Opt-in bounded VMFUNC experiment. CONFIG copies a small test image;
 * LIVE_CONFIG pins explicitly listed user RAM pages and copies private overlays.
 * No arbitrary userspace physical addresses enter the EPTs. Every exit returns
 * to userspace for state recovery before ordinary KVM handles guest events.
 * This is a fixed-RAM, single-vCPU experiment, not a general memory backend.
 */
#include <linux/moduleparam.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <asm/tsc.h>

#include "vmx.h"
#include "x86_ops.h"
#include "tdx.h"
#include "../irq.h"
#include "../x86.h"
#include "../smm.h"

#define HBT_LAZY_DATA_PAGES 256

static bool __read_mostly hbt_eptp_probe;
module_param(hbt_eptp_probe, bool, 0444);
MODULE_PARM_DESC(hbt_eptp_probe, "Enable the bounded HBT VMFUNC test interface");

struct vmx_hbt_eptp {
	u64 *list;
	u64 *tables[2][4];
	void *ram[KVM_HBT_EPTP_MAX_PAGES];
	void *overlays[KVM_HBT_EPTP_MAX_PAGES];
	u32 pages, view;
	u64 saved_eptp, saved_function, saved_list;
	u32 saved_secondary, saved_exceptions;
	u32 saved_pin, saved_timer, deferred_irq;
	bool entered;
	bool pending_event;
	bool must_recover;
	bool live;
	struct page **pinned;
	u32 pinned_count, map_count;
	u64 ram_hva;
	u32 ram_pages, data_count, data_pages[HBT_LAZY_DATA_PAGES];
	u64 *live_pts[2][512];
	struct kvm_hbt_eptp_live_map maps[KVM_HBT_EPTP_MAX_OVERLAYS];
	void *map_pages[2][KVM_HBT_EPTP_MAX_OVERLAYS];
};

static void free_probe(struct vmx_hbt_eptp *p)
{
	unsigned int v, l, i;

	if (!p)
		return;
	if (p->pinned_count)
		unpin_user_pages_dirty_lock(p->pinned, p->pinned_count, true);
	kvfree(p->pinned);
	for (v = 0; v < 2; v++) {
		for (i = 1; i < 512; i++)
			free_page((unsigned long)p->live_pts[v][i]);
		for (i = 0; i < p->map_count; i++)
			free_page((unsigned long)p->map_pages[v][i]);
	}
	for (i = 0; i < p->pages; i++) {
		free_page((unsigned long)p->ram[i]);
		free_page((unsigned long)p->overlays[i]);
	}
	for (v = 0; v < 2; v++)
		for (l = 0; l < 4; l++)
			free_page((unsigned long)p->tables[v][l]);
	free_page((unsigned long)p->list);
	kfree(p);
}

void vmx_hbt_eptp_free(struct vcpu_vmx *vmx)
{
	free_probe(vmx->hbt_eptp);
	vmx->hbt_eptp = NULL;
}

static void *copy_page_from_user(u64 address)
{
	void *page = (void *)get_zeroed_page(GFP_KERNEL_ACCOUNT);

	if (!page)
		return ERR_PTR(-ENOMEM);
	if (copy_from_user(page, u64_to_user_ptr(address), PAGE_SIZE)) {
		free_page((unsigned long)page);
		return ERR_PTR(-EFAULT);
	}
	return page;
}

static int configure_probe(struct vcpu_vmx *vmx, struct kvm_hbt_eptp_probe *req)
{
	struct kvm_hbt_eptp_overlay overlays[KVM_HBT_EPTP_MAX_OVERLAYS];
	struct vmx_hbt_eptp *p;
	unsigned int i, v, l;
	void *page;
	int ret = -ENOMEM;

	if (!req->nr_pages || req->nr_pages > KVM_HBT_EPTP_MAX_PAGES ||
	    req->nr_overlays > KVM_HBT_EPTP_MAX_OVERLAYS || req->view ||
	    req->image_addr > U64_MAX - (u64)req->nr_pages * PAGE_SIZE)
		return -EINVAL;
	if (vmx->hbt_eptp)
		return -EBUSY;
	if (copy_from_user(overlays, u64_to_user_ptr(req->overlays_addr),
			   req->nr_overlays * sizeof(overlays[0])))
		return -EFAULT;
	for (i = 0; i < req->nr_overlays; i++) {
		if (overlays[i].reserved || overlays[i].page >= req->nr_pages)
			return -EINVAL;
		for (l = 0; l < i; l++)
			if (overlays[l].page == overlays[i].page)
				return -EINVAL;
	}
	p = kzalloc_obj(*p, GFP_KERNEL_ACCOUNT);
	if (!p)
		return -ENOMEM;
	p->pages = req->nr_pages;
	p->list = (void *)get_zeroed_page(GFP_KERNEL_ACCOUNT);
	if (!p->list)
		goto fail;
	for (v = 0; v < 2; v++) {
		for (l = 0; l < 4; l++) {
			p->tables[v][l] = (void *)get_zeroed_page(GFP_KERNEL_ACCOUNT);
			if (!p->tables[v][l])
				goto fail;
			if (l)
				p->tables[v][l - 1][0] = __pa(p->tables[v][l]) | 7;
		}
		p->list[v] = __pa(p->tables[v][0]) | VMX_EPTP_MT_WB | VMX_EPTP_PWL_4;
	}
	for (i = 0; i < p->pages; i++) {
		page = copy_page_from_user(req->image_addr + (u64)i * PAGE_SIZE);
		if (IS_ERR(page)) {
			ret = PTR_ERR(page);
			goto fail;
		}
		p->ram[i] = page;
		/* WB, ignore guest PAT, R/W/X; A/D tracking is deliberately off. */
		p->tables[0][3][i] = p->tables[1][3][i] = __pa(page) | 0x77;
	}
	for (i = 0; i < req->nr_overlays; i++) {
		page = copy_page_from_user(overlays[i].image_addr);
		if (IS_ERR(page)) {
			ret = PTR_ERR(page);
			goto fail;
		}
		p->overlays[overlays[i].page] = page;
		p->tables[1][3][overlays[i].page] = __pa(page) | 0x77;
	}
	vmx->hbt_eptp = p;
	return 0;
fail:
	free_probe(p);
	return ret;
}

static int live_leaf(struct vmx_hbt_eptp *p, unsigned int view,
		     unsigned int page, u64 value)
{
	unsigned int pt = page / 512;

	if (!p->live_pts[view][pt]) {
		p->live_pts[view][pt] = (void *)get_zeroed_page(GFP_KERNEL_ACCOUNT);
		if (!p->live_pts[view][pt])
			return -ENOMEM;
		p->tables[view][2][pt] = __pa(p->live_pts[view][pt]) | 7;
	}
	p->live_pts[view][pt][page % 512] = value;
	return 0;
}

/* Identical nonzero image pointers describe shared private backing. Each
 * mapping owns a page reference, so free_probe() handles aliases normally.
 */
static void *live_overlay_copy(struct vmx_hbt_eptp *p, unsigned int view,
			      unsigned int index, u64 address)
{
	unsigned int v, i;

	for (v = 0; v <= view; v++) {
		for (i = 0; i < (v == view ? index : p->map_count); i++) {
			u64 prior = v ? p->maps[i].helper_addr : p->maps[i].normal_addr;
			void *page = p->map_pages[v][i];

			if (prior == address && page) {
				get_page(virt_to_page(page));
				return page;
			}
		}
	}
	return copy_page_from_user(address);
}

static int configure_live(struct vcpu_vmx *vmx, struct kvm_hbt_eptp_probe *req)
{
	struct vmx_hbt_eptp *p;
	unsigned int i, j, v, l;
	u64 ram_leaves[KVM_HBT_EPTP_MAX_OVERLAYS] = {};
	long pinned;
	int ret = -ENOMEM;

	if (!cpu_has_vmx_ept_execute_only() || !cpu_has_vmx_preemption_timer() ||
	    !vmx->vcpu.kvm->arch.hbt_virtual_xstate_enabled ||
	    !vmx->vcpu.kvm->arch.hbt_retry_enabled ||
	    vmx->vcpu.kvm->max_vcpus != 1)
		return -EOPNOTSUPP;
	if (vmx->hbt_eptp)
		return -EBUSY;
	if (!req->nr_pages || req->nr_pages > KVM_HBT_EPTP_LIVE_MAX_PAGES ||
	    req->nr_overlays > KVM_HBT_EPTP_MAX_OVERLAYS || req->view ||
	    offset_in_page(req->image_addr) ||
	    req->image_addr > ULONG_MAX - (u64)req->nr_pages * PAGE_SIZE)
		return -EINVAL;
	p = kzalloc_obj(*p, GFP_KERNEL_ACCOUNT);
	if (!p)
		return -ENOMEM;
	p->live = true;
	p->ram_hva = req->image_addr;
	p->ram_pages = req->nr_pages;
	p->map_count = req->nr_overlays;
	if (copy_from_user(p->maps, u64_to_user_ptr(req->overlays_addr),
			   p->map_count * sizeof(p->maps[0]))) {
		ret = -EFAULT;
		goto fail;
	}
	for (i = 0; i < p->map_count; i++) {
		struct kvm_hbt_eptp_live_map *m = &p->maps[i];

		ret = -EINVAL;
		if (m->page >= KVM_HBT_EPTP_LIVE_MAX_PAGES || m->permissions & ~0x707U)
			goto fail;
		for (j = 0; j < i; j++)
			if (p->maps[j].page == m->page)
				goto fail;
		for (v = 0; v < 2; v++) {
			unsigned int perm = (m->permissions >> (v * 8)) & 7;
			u64 address = v ? m->helper_addr : m->normal_addr;

			if (((perm & 2) && !(perm & 1)) ||
			    (perm && !address && m->page >= req->nr_pages))
				goto fail;
		}
	}
	p->pinned = kvmalloc_array(p->map_count + HBT_LAZY_DATA_PAGES,
				  sizeof(*p->pinned), GFP_KERNEL_ACCOUNT);
	if (!p->pinned) {
		ret = -ENOMEM;
		goto fail;
	}
	/* Map only the original code/page-table pages used by this attempt.
	 * Any other GPA exits so userspace can recover before ordinary KVM.
	 * Pinning all RAM on every #UD can starve guest timer delivery.
	 */
	for (i = 0; i < p->map_count; i++) {
		struct kvm_hbt_eptp_live_map *m = &p->maps[i];

		if (!((m->permissions & 7) && !m->normal_addr) &&
		    !((m->permissions & 0x700) && !m->helper_addr))
			continue;
		pinned = pin_user_pages_fast(req->image_addr + (u64)m->page * PAGE_SIZE,
					     1, FOLL_WRITE | FOLL_LONGTERM,
					     &p->pinned[p->pinned_count]);
		if (pinned != 1) {
			ret = pinned < 0 ? pinned : -EFAULT;
			goto fail;
		}
		ram_leaves[i] = page_to_phys(p->pinned[p->pinned_count++]);
	}
	p->list = (void *)get_zeroed_page(GFP_KERNEL_ACCOUNT);
	if (!p->list) {
		ret = -ENOMEM;
		goto fail;
	}
	for (v = 0; v < 2; v++) {
		for (l = 0; l < 4; l++) {
			p->tables[v][l] = (void *)get_zeroed_page(GFP_KERNEL_ACCOUNT);
			if (!p->tables[v][l]) {
				ret = -ENOMEM;
				goto fail;
			}
			if (l)
				p->tables[v][l - 1][0] = __pa(p->tables[v][l]) | 7;
		}
		p->live_pts[v][0] = p->tables[v][3];
		p->list[v] = __pa(p->tables[v][0]) | VMX_EPTP_MT_WB | VMX_EPTP_PWL_4;
		for (i = 0; i < p->map_count; i++) {
			struct kvm_hbt_eptp_live_map *m = &p->maps[i];
			u64 address = v ? m->helper_addr : m->normal_addr;
			unsigned int perm = (m->permissions >> (v * 8)) & 7;
			u64 value = 0;

			if (perm && address) {
				void *page = live_overlay_copy(p, v, i, address);

				if (IS_ERR(page)) {
					ret = PTR_ERR(page);
					goto fail;
				}
				p->map_pages[v][i] = page;
				value = __pa(page) | 0x70 | perm;
			} else if (perm) {
				value = ram_leaves[i] | 0x70 | perm;
			}
			ret = live_leaf(p, v, m->page, value);
			if (ret)
				goto fail;
		}
	}
	vmx->hbt_eptp = p;
	return 0;
fail:
	free_probe(p);
	return ret;
}

/* Add ordinary RAM as data only. Explicit overlays keep their permissions;
 * code writes and all instruction fetches still require canonical recovery.
 * The VMM's original LIVE_CONFIG image bounds every userspace pin.
 */
static int map_live_data(struct kvm_vcpu *vcpu, struct vmx_hbt_eptp *p,
			 struct kvm_hbt_eptp_probe *req)
{
	unsigned int page = req->nr_pages, i;
	struct page *pinned;
	long count;
	int ret;

	if (!p || !p->live || page >= p->ram_pages || req->view ||
	    req->image_addr || req->overlays_addr || req->nr_overlays)
		return -EINVAL;
	for (i = 0; i < p->map_count; i++)
		if (p->maps[i].page == page)
			return -EPERM;
	for (i = 0; i < p->data_count; i++)
		if (p->data_pages[i] == page)
			return 0;
	if (p->data_count == HBT_LAZY_DATA_PAGES)
		return -ENOSPC;
	count = pin_user_pages_fast(p->ram_hva + (u64)page * PAGE_SIZE, 1,
				   FOLL_WRITE | FOLL_LONGTERM, &pinned);
	if (count != 1)
		return count < 0 ? count : -EFAULT;
	p->pinned[p->pinned_count++] = pinned;
	p->data_pages[p->data_count++] = page;
	for (i = 0; i < 2; i++) {
		ret = live_leaf(p, i, page, page_to_phys(pinned) | 0x73);
		if (ret)
			return ret;
	}
	kvm_vcpu_srcu_read_lock(vcpu);
	kvm_vcpu_mark_page_dirty(vcpu, page);
	kvm_vcpu_srcu_read_unlock(vcpu);
	return 0;
}

static int read_maps(struct vmx_hbt_eptp *p, struct kvm_hbt_eptp_probe *req)
{
	struct kvm_hbt_eptp_overlay maps[KVM_HBT_EPTP_MAX_OVERLAYS];
	unsigned int i, j;

	if (!p || !p->live)
		return -ENOENT;
	if (req->view > 1 || req->nr_pages || req->image_addr ||
	    req->nr_overlays > KVM_HBT_EPTP_MAX_OVERLAYS)
		return -EINVAL;
	if (copy_from_user(maps, u64_to_user_ptr(req->overlays_addr),
			   req->nr_overlays * sizeof(maps[0])))
		return -EFAULT;
	for (i = 0; i < req->nr_overlays; i++) {
		void *page = NULL;

		if (maps[i].reserved)
			return -EINVAL;
		for (j = 0; j < p->map_count; j++)
			if (p->maps[j].page == maps[i].page)
				page = p->map_pages[req->view][j];
		if (!page)
			return -ENOENT;
		if (copy_to_user(u64_to_user_ptr(maps[i].image_addr), page, PAGE_SIZE))
			return -EFAULT;
	}
	return 0;
}

long vmx_hbt_eptp_probe(struct kvm_vcpu *vcpu, void __user *argp)
{
	struct kvm_hbt_eptp_probe req;
	struct vcpu_vmx *vmx;
	struct vmx_hbt_eptp *p;
	u64 funcs;
	unsigned int i;

	if (!hbt_eptp_probe || is_td_vcpu(vcpu) || !enable_ept || !enable_vpid ||
	    !cpu_has_vmx_vmfunc() || rdmsrq_safe(MSR_IA32_VMX_VMFUNC, &funcs) ||
	    !(funcs & VMX_VMFUNC_EPTP_SWITCHING))
		return -EOPNOTSUPP;
	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;
	if (req.version != KVM_HBT_EPTP_VERSION || req.flags ||
	    memchr_inv(req.reserved, 0, sizeof(req.reserved)))
		return -EINVAL;
	vmx = to_vmx(vcpu);
	p = vmx->hbt_eptp;
	if (is_guest_mode(vcpu) || vmx->nested.vmxon || is_smm(vcpu) ||
	    (irqchip_in_kernel(vcpu->kvm) && enable_apicv))
		return -EOPNOTSUPP;
	switch (req.operation) {
	case KVM_HBT_EPTP_QUERY:
		if (req.nr_pages || req.view || req.image_addr || req.overlays_addr || req.nr_overlays)
			return -EINVAL;
		req.nr_pages = KVM_HBT_EPTP_MAX_PAGES;
		req.nr_overlays = KVM_HBT_EPTP_MAX_OVERLAYS;
		req.view = p ? p->view : 0;
		req.flags = boot_cpu_has(X86_FEATURE_HYPERVISOR) ? KVM_HBT_EPTP_UNDER_HYPERVISOR : 0;
		req.flags |= KVM_HBT_EPTP_SHARED_OVERLAY | KVM_HBT_EPTP_LAZY_DATA;
		return copy_to_user(argp, &req, sizeof(req)) ? -EFAULT : 0;
	case KVM_HBT_EPTP_CONFIG:
		return configure_probe(vmx, &req);
	case KVM_HBT_EPTP_LIVE_CONFIG:
		return configure_live(vmx, &req);
	case KVM_HBT_EPTP_MAP_DATA:
		return map_live_data(vcpu, p, &req);
	case KVM_HBT_EPTP_READ_MAPS:
		return read_maps(p, &req);
	case KVM_HBT_EPTP_READ:
		if (!p)
			return -ENOENT;
		if (p->live || req.view > 1 || req.nr_pages != p->pages ||
		    req.overlays_addr || req.nr_overlays ||
		    req.image_addr > U64_MAX - (u64)p->pages * PAGE_SIZE)
			return -EINVAL;
		for (i = 0; i < p->pages; i++) {
			void *page = req.view && p->overlays[i] ? p->overlays[i] : p->ram[i];

			if (copy_to_user(u64_to_user_ptr(req.image_addr + (u64)i * PAGE_SIZE),
					 page, PAGE_SIZE))
				return -EFAULT;
		}
		return 0;
	case KVM_HBT_EPTP_DESTROY:
		if (req.nr_pages || req.view || req.image_addr || req.overlays_addr || req.nr_overlays)
			return -EINVAL;
		vmx_hbt_eptp_free(vmx);
		return 0;
	default:
		return -EINVAL;
	}
}

/* Guest IRQs may wait for the bounded private execution slice. Never let
 * an injected event expose helper state to guest handlers. Non-IRQ events
 * instead ask userspace to recover before any VM entry.
 */
bool vmx_hbt_eptp_pending_event(struct vcpu_vmx *vmx)
{
	struct vmx_hbt_eptp *p = vmx->hbt_eptp;
	u32 info = vmcs_read32(VM_ENTRY_INTR_INFO_FIELD);

	if (!(info & INTR_INFO_VALID_MASK))
		return false;
	if (p->live && (info & INTR_INFO_INTR_TYPE_MASK) == INTR_TYPE_EXT_INTR) {
		p->deferred_irq = info;
		p->must_recover = true;
		vmx_cancel_injection(&vmx->vcpu);
		return false;
	}
	p->pending_event = true;
	return true;
}

void vmx_hbt_eptp_complete_interrupts(struct vcpu_vmx *vmx)
{
	struct vmx_hbt_eptp *p = vmx->hbt_eptp;

	if (p->deferred_irq) {
		kvm_queue_interrupt(&vmx->vcpu, p->deferred_irq & INTR_INFO_VECTOR_MASK, false);
		kvm_make_request(KVM_REQ_EVENT, &vmx->vcpu);
		p->deferred_irq = 0;
	}
}

void vmx_hbt_eptp_enter(struct vcpu_vmx *vmx)
{
	struct vmx_hbt_eptp *p = vmx->hbt_eptp;

	p->saved_eptp = vmcs_read64(EPT_POINTER);
	p->saved_function = vmcs_read64(VM_FUNCTION_CONTROL);
	p->saved_list = vmcs_read64(EPTP_LIST_ADDRESS);
	p->saved_secondary = secondary_exec_controls_get(vmx);
	p->saved_exceptions = vmcs_read32(EXCEPTION_BITMAP);
	if (p->live) {
		/* At most 100 us of guest execution before a checkpoint. This timer
		 * belongs to the private slice, not to the guest's LAPIC timer.
		 */
		p->saved_pin = pin_controls_get(vmx);
		p->saved_timer = vmcs_read32(VMX_PREEMPTION_TIMER_VALUE);
		pin_controls_set(vmx, p->saved_pin | PIN_BASED_VMX_PREEMPTION_TIMER);
		vmcs_write32(VMX_PREEMPTION_TIMER_VALUE,
			max_t(u32, 1, (tsc_khz / 10) >>
			      vmx_misc_preemption_timer_rate(vmcs_config.misc)));
	}
	/* Also handles CPU migration and reuse of freed physical EPT root pages. */
	ept_sync_context(p->list[0]);
	ept_sync_context(p->list[1]);
	vmcs_write64(EPTP_LIST_ADDRESS, __pa(p->list));
	vmcs_write64(VM_FUNCTION_CONTROL, VMX_VMFUNC_EPTP_SWITCHING);
	vmcs_write64(EPT_POINTER, p->list[p->view]);
	vmcs_write32(EXCEPTION_BITMAP, ~0U);
	secondary_exec_controls_set(vmx, (p->saved_secondary | SECONDARY_EXEC_ENABLE_VMFUNC) &
				    ~(SECONDARY_EXEC_ENABLE_PML | SECONDARY_EXEC_EPT_VIOLATION_VE));
	p->entered = true;
}

void vmx_hbt_eptp_leave(struct vcpu_vmx *vmx)
{
	struct vmx_hbt_eptp *p = vmx->hbt_eptp;
	u64 active = vmcs_read64(EPT_POINTER);

	p->view = active == p->list[1];
	if (p->live) {
		pin_controls_set(vmx, p->saved_pin);
		vmcs_write32(VMX_PREEMPTION_TIMER_VALUE, p->saved_timer);
	}
	vmcs_write64(VM_FUNCTION_CONTROL, p->saved_function);
	vmcs_write64(EPTP_LIST_ADDRESS, p->saved_list);
	vmcs_write64(EPT_POINTER, p->saved_eptp);
	vmcs_write32(EXCEPTION_BITMAP, p->saved_exceptions);
	secondary_exec_controls_set(vmx, p->saved_secondary);
}

int vmx_hbt_eptp_exit(struct vcpu_vmx *vmx)
{
	struct kvm_vcpu *vcpu = &vmx->vcpu;
	struct kvm_run *run = vcpu->run;

	if (vmx->hbt_eptp->pending_event) {
		vmx->hbt_eptp->pending_event = false;
		run->exit_reason = KVM_EXIT_HBT_EPTP;
		run->internal.suberror = KVM_HBT_EPTP_PENDING_EVENT;
		run->internal.ndata = 7;
		run->internal.data[0] = vmx->hbt_eptp->view;
		run->internal.data[1] = 0;
		run->internal.data[2] = 0;
		run->internal.data[3] = vmcs_read32(VM_ENTRY_INTR_INFO_FIELD);
		run->internal.data[4] = kvm_rip_read(vcpu);
		run->internal.data[5] = 1;
		run->internal.data[6] = 0;
		return 0;
	}
	if (!vmx->hbt_eptp->entered || vmx->fail || vmx_get_exit_reason(vcpu).failed_vmentry) {
		run->exit_reason = KVM_EXIT_FAIL_ENTRY;
		run->fail_entry.hardware_entry_failure_reason = vmx_get_exit_reason(vcpu).full;
		run->fail_entry.cpu = vcpu->cpu;
		return 0;
	}
	vmx->hbt_eptp->entered = false;
	run->exit_reason = KVM_EXIT_HBT_EPTP;
	run->internal.suberror = vmx_get_exit_reason(vcpu).full;
	run->internal.ndata = 7;
	run->internal.data[0] = vmx->hbt_eptp->view;
	run->internal.data[1] = vmcs_readl(EXIT_QUALIFICATION);
	run->internal.data[2] = vmcs_read32(VM_EXIT_INSTRUCTION_LEN);
	run->internal.data[3] = vmcs_read32(VM_EXIT_INTR_INFO);
	run->internal.data[4] = kvm_rip_read(vcpu);
	run->internal.data[5] = vmx->hbt_eptp->must_recover;
	run->internal.data[6] = vmx_get_exit_reason(vcpu).basic == EXIT_REASON_EPT_VIOLATION ?
		vmcs_read64(GUEST_PHYSICAL_ADDRESS) : 0;
	return 0;
}
