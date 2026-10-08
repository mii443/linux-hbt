// SPDX-License-Identifier: GPL-2.0
/*
 * Opt-in bounded VMFUNC experiment. RAM is copied into accounted, kernel-owned
 * pages. No memslot PFNs or arbitrary userspace physical addresses enter these
 * EPTs. Every exit returns to the test driver; ordinary KVM emulation never
 * interprets a private view as memslot RAM. This is deliberately not a general
 * VM memory backend or an unmodified-guest translation implementation.
 */
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "vmx.h"
#include "tdx.h"
#include "../irq.h"
#include "../smm.h"

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
	bool entered;
};

static void free_probe(struct vmx_hbt_eptp *p)
{
	unsigned int v, l, i;

	if (!p)
		return;
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
	    irqchip_in_kernel(vcpu->kvm))
		return -EOPNOTSUPP;
	switch (req.operation) {
	case KVM_HBT_EPTP_QUERY:
		if (req.nr_pages || req.view || req.image_addr || req.overlays_addr || req.nr_overlays)
			return -EINVAL;
		req.nr_pages = KVM_HBT_EPTP_MAX_PAGES;
		req.nr_overlays = KVM_HBT_EPTP_MAX_OVERLAYS;
		req.flags = boot_cpu_has(X86_FEATURE_HYPERVISOR) ? KVM_HBT_EPTP_UNDER_HYPERVISOR : 0;
		return copy_to_user(argp, &req, sizeof(req)) ? -EFAULT : 0;
	case KVM_HBT_EPTP_CONFIG:
		return configure_probe(vmx, &req);
	case KVM_HBT_EPTP_READ:
		if (!p)
			return -ENOENT;
		if (req.view > 1 || req.nr_pages != p->pages || req.overlays_addr || req.nr_overlays ||
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

void vmx_hbt_eptp_enter(struct vcpu_vmx *vmx)
{
	struct vmx_hbt_eptp *p = vmx->hbt_eptp;

	p->saved_eptp = vmcs_read64(EPT_POINTER);
	p->saved_function = vmcs_read64(VM_FUNCTION_CONTROL);
	p->saved_list = vmcs_read64(EPTP_LIST_ADDRESS);
	p->saved_secondary = secondary_exec_controls_get(vmx);
	p->saved_exceptions = vmcs_read32(EXCEPTION_BITMAP);
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

	if (!vmx->hbt_eptp->entered || vmx->fail || vmx_get_exit_reason(vcpu).failed_vmentry) {
		run->exit_reason = KVM_EXIT_FAIL_ENTRY;
		run->fail_entry.hardware_entry_failure_reason = vmx_get_exit_reason(vcpu).full;
		run->fail_entry.cpu = vcpu->cpu;
		return 0;
	}
	vmx->hbt_eptp->entered = false;
	run->exit_reason = KVM_EXIT_HBT_EPTP;
	run->internal.suberror = vmx_get_exit_reason(vcpu).full;
	run->internal.ndata = 5;
	run->internal.data[0] = vmx->hbt_eptp->view;
	run->internal.data[1] = vmcs_readl(EXIT_QUALIFICATION);
	run->internal.data[2] = vmcs_read32(VM_EXIT_INSTRUCTION_LEN);
	run->internal.data[3] = vmcs_read32(VM_EXIT_INTR_INFO);
	run->internal.data[4] = kvm_rip_read(vcpu);
	return 0;
}
