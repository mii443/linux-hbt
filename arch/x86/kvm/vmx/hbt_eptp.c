// SPDX-License-Identifier: GPL-2.0
/*
 * Opt-in bounded VMFUNC experiment. CONFIG copies a small test image;
 * LIVE_CONFIG pins explicitly listed user RAM pages and copies private overlays.
 * No arbitrary userspace physical addresses enter the EPTs. Every exit returns
 * to userspace for state recovery before ordinary KVM handles guest events.
 * This is a single-vCPU experiment with bounded overlays and pinned data.
 */
#include <linux/moduleparam.h>
#include <linux/hash.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <asm/tsc.h>

#include "vmx.h"
#include "x86_ops.h"
#include "tdx.h"
#include "../irq.h"
#include "../lapic.h"
#include "../x86.h"
#include "../smm.h"

#define HBT_LAZY_DATA_MAX_PAGES 4096
#define HBT_LIVE_TABLE_MAX_PAGES 1024 /* per view, allocated only when needed */

static bool __read_mostly hbt_eptp_probe;
module_param(hbt_eptp_probe, bool, 0444);
MODULE_PARM_DESC(hbt_eptp_probe, "Enable the bounded HBT VMFUNC test interface");

static bool __read_mostly hbt_eptp_tlb_cache = true;
module_param(hbt_eptp_tlb_cache, bool, 0644);
MODULE_PARM_DESC(hbt_eptp_tlb_cache, "Reuse unchanged HBT translations on the same CPU");

/* Applied to newly created contexts only. Keep pinning bounded independently
 * of executable overlays; a larger working set must not flush translated code.
 */
static unsigned int __read_mostly hbt_eptp_data_pages = 1024;
module_param(hbt_eptp_data_pages, uint, 0644);
MODULE_PARM_DESC(hbt_eptp_data_pages, "Maximum lazy data pages per new HBT context (16..4096)");

struct vmx_hbt_eptp {
	u64 key, age;
	u64 *list;
	u64 *tables[2][4];
	void *ram[KVM_HBT_EPTP_MAX_PAGES];
	void *overlays[KVM_HBT_EPTP_MAX_PAGES];
	u32 pages, view;
	u64 saved_eptp, saved_function, saved_list;
	u32 saved_secondary, saved_exceptions;
	u32 saved_pin, saved_timer;
	u32 saved_primary;
	u64 *read_step_leaf, read_step_saved;
	struct page *read_step_page;
	bool read_step_exit;
	bool entered;
	bool pending_event;
	bool tlb_valid;
	int tlb_cpu;
	bool live;
	bool memslots;
	u64 slots_generation;
	u32 table_count[2];
	struct page **pinned;
	u32 pinned_count, map_count;
	u64 ram_hva;
	u32 ram_pages, data_count, data_limit;
	u32 *data_pages;
	u32 native_count, native_pages[HBT_LAZY_DATA_MAX_PAGES];
	struct kvm_hbt_eptp_live_map maps[KVM_HBT_EPTP_MAX_OVERLAYS];
	u16 map_index[1024];
	void *map_pages[2][KVM_HBT_EPTP_MAX_OVERLAYS];
};

static u16 *live_map_slot(struct kvm_hbt_eptp_live_map *maps, u16 *index, u32 page)
{
	unsigned int at = hash_32(page, 10);

	while (index[at] && maps[index[at] - 1].page != page)
		at = (at + 1) & 1023;
	return index + at;
}

static void free_live_tables(u64 *table, unsigned int level)
{
	unsigned int i;

	if (!table)
		return;
	if (level)
		for (i = 0; i < 512; i++)
			if (table[i])
				free_live_tables(__va(table[i] & PAGE_MASK), level - 1);
	free_page((unsigned long)table);
}

/* The canonical alias is permitted for at most one hardware instruction.
 * Restore on EVERY exit/cancellation, including failed entry and exceptions.
 * No guest data or architectural register is emulated or changed here. */
static void cancel_read_step(struct vmx_hbt_eptp *p)
{
	if (!p->read_step_leaf)
		return;
	*p->read_step_leaf = p->read_step_saved;
	p->read_step_leaf = NULL;
	p->tlb_valid = false;
	unpin_user_page(p->read_step_page);
	p->read_step_page = NULL;
}

static void free_probe(struct vmx_hbt_eptp *p)
{
	unsigned int v, l, i;

	if (!p)
		return;
	cancel_read_step(p);
	if (p->pinned_count)
		unpin_user_pages_dirty_lock(p->pinned, p->pinned_count, true);
	kvfree(p->pinned);
	kvfree(p->data_pages);
	for (v = 0; v < 2; v++) {
		for (i = 0; i < p->map_count; i++)
			free_page((unsigned long)p->map_pages[v][i]);
	}
	for (i = 0; i < p->pages; i++) {
		free_page((unsigned long)p->ram[i]);
		free_page((unsigned long)p->overlays[i]);
	}
	for (v = 0; v < 2; v++) {
		if (p->live)
			free_live_tables(p->tables[v][0], 3);
		else
			for (l = 0; l < 4; l++)
				free_page((unsigned long)p->tables[v][l]);
	}
	free_page((unsigned long)p->list);
	kvfree(p);
}

void vmx_hbt_eptp_free(struct vcpu_vmx *vmx)
{
	unsigned int i;

	free_probe(vmx->hbt_eptp);
	vmx->hbt_eptp = NULL;
	free_probe(vmx->hbt_eptp_cache);
	vmx->hbt_eptp_cache = NULL;
	for (i = 0; i < KVM_HBT_EPTP_MAX_CONTEXTS; i++) {
		free_probe(vmx->hbt_eptp_contexts[i]);
		vmx->hbt_eptp_contexts[i] = NULL;
	}
}

static struct vmx_hbt_eptp **cached_context(struct vcpu_vmx *vmx, u64 key)
{
	unsigned int i;

	for (i = 0; i < KVM_HBT_EPTP_MAX_CONTEXTS; i++)
		if (vmx->hbt_eptp_contexts[i] && vmx->hbt_eptp_contexts[i]->key == key)
			return &vmx->hbt_eptp_contexts[i];
	return NULL;
}

static void park_context(struct vcpu_vmx *vmx, struct vmx_hbt_eptp *p)
{
	struct vmx_hbt_eptp **slot = &vmx->hbt_eptp_contexts[0];
	unsigned int i;

	for (i = 0; i < KVM_HBT_EPTP_MAX_CONTEXTS; i++) {
		struct vmx_hbt_eptp **candidate = &vmx->hbt_eptp_contexts[i];

		if (!*candidate) {
			slot = candidate;
			break;
		}
		if ((*candidate)->age < (*slot)->age)
			slot = candidate;
	}
	free_probe(*slot);
	p->age = ++vmx->hbt_eptp_age;
	*slot = p;
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
	struct kvm_hbt_eptp_overlay *overlays __free(kfree) = NULL;
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
	overlays = kmalloc_array(req->nr_overlays, sizeof(*overlays), GFP_KERNEL_ACCOUNT);
	if (!overlays)
		return -ENOMEM;
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
	p = kvzalloc_obj(*p, GFP_KERNEL_ACCOUNT);
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

struct live_table_link {
	u64 *entry;
	unsigned int view;
};

struct live_table_journal {
	unsigned int count;
	struct live_table_link links[];
};

static u64 *live_leaf_pointer(struct vmx_hbt_eptp *p, unsigned int view,
			     unsigned int page, struct live_table_journal *journal)
{
	u64 *table = p->tables[view][0];
	unsigned int shift;

	for (shift = 27; shift; shift -= 9) {
		unsigned int index = (page >> shift) & 511;
		u64 *next;

		if (!table[index]) {
			if (p->table_count[view] == HBT_LIVE_TABLE_MAX_PAGES)
				return ERR_PTR(-ENOSPC);
			next = (void *)get_zeroed_page(GFP_KERNEL_ACCOUNT);
			if (!next)
				return ERR_PTR(-ENOMEM);
			table[index] = __pa(next) | 7;
			p->table_count[view]++;
			if (journal)
				journal->links[journal->count++] =
					(struct live_table_link){ &table[index], view };
		}
		table = __va(table[index] & PAGE_MASK);
	}
	return &table[page & 511];
}

static int live_leaf(struct vmx_hbt_eptp *p, unsigned int view,
		     unsigned int page, u64 value)
{
	u64 *entry = live_leaf_pointer(p, view, page, NULL);

	if (IS_ERR(entry))
		return PTR_ERR(entry);
	*entry = value;
	return 0;
}

/* These canonical execution leaves do not depend on copied guest PTEs.
 * Revoke them before any canonical interval can unmap/reuse a user page,
 * change privilege state, or repurpose that GPA as kernel code. Retained
 * private contexts must relearn them after resuming. Data pinning remains. */
static void revoke_native_pages(struct vmx_hbt_eptp *p)
{
	unsigned int i, v;

	if (!p->native_count)
		return;
	for (i = 0; i < p->native_count; i++)
		for (v = 0; v < 2; v++) {
			u64 *leaf = live_leaf_pointer(p, v, p->native_pages[i], NULL);

			/* Both leaves were installed successfully before publication. */
			if (!WARN_ON_ONCE(IS_ERR(leaf)))
				*leaf &= ~4ULL;
		}
	p->native_count = 0;
	p->tlb_valid = false;
}

static u64 live_slots_generation(struct kvm *kvm)
{
	int idx = srcu_read_lock(&kvm->srcu);
	u64 generation = kvm_memslots(kvm)->generation;

	srcu_read_unlock(&kvm->srcu, idx);
	return generation;
}

static long live_pin(struct kvm_vcpu *vcpu, struct vmx_hbt_eptp *p,
		     unsigned int gfn, struct page **page)
{
	unsigned long hva = p->ram_hva + (u64)gfn * PAGE_SIZE;
	int idx = srcu_read_lock(&vcpu->kvm->srcu);
	long ret;

	if (p->memslots) {
		if (p->slots_generation != kvm_memslots(vcpu->kvm)->generation) {
			ret = -ESTALE;
			goto out;
		}
		/* gfn_to_hva() requires a writable, valid slot. */
		hva = gfn_to_hva(vcpu->kvm, gfn);
		if (kvm_is_error_hva(hva)) {
			ret = -EFAULT;
			goto out;
		}
	}
	ret = pin_user_pages_fast(hva, 1, FOLL_WRITE | FOLL_LONGTERM, page);
out:
	srcu_read_unlock(&vcpu->kvm->srcu, idx);
	return ret;
}

/* Identical nonzero image pointers describe shared private backing. Each
 * mapping owns a page reference, so free_probe() handles aliases normally.
 */
static void *live_overlay_copy(struct vmx_hbt_eptp *p, u64 address)
{
	unsigned int v, i;

	for (v = 0; v < 2; v++) {
		for (i = 0; i < p->map_count; i++) {
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
	struct vmx_hbt_eptp **old;
	unsigned int i, v, data_limit = READ_ONCE(hbt_eptp_data_pages);
	bool memslots = req->flags & KVM_HBT_EPTP_MEMSLOTS;
	unsigned int max_pages = memslots ? KVM_HBT_EPTP_MEMSLOT_MAX_PAGES :
		KVM_HBT_EPTP_LIVE_MAX_PAGES;
	u64 *ram_leaves __free(kfree) = NULL;
	long pinned;
	int ret = -ENOMEM;

	if (!cpu_has_vmx_ept_execute_only() || !cpu_has_vmx_preemption_timer() ||
	    !vmx->vcpu.kvm->arch.hbt_virtual_xstate_enabled ||
	    !vmx->vcpu.kvm->arch.hbt_retry_enabled ||
	    vmx->vcpu.kvm->max_vcpus != 1)
		return -EOPNOTSUPP;
	if (vmx->hbt_eptp)
		return -EBUSY;
	if (data_limit < 16 || data_limit > HBT_LAZY_DATA_MAX_PAGES ||
	    (memslots ? req->nr_pages || req->image_addr :
	     !req->nr_pages || req->nr_pages > max_pages) ||
	    req->nr_overlays > KVM_HBT_EPTP_MAX_OVERLAYS || req->view ||
	    offset_in_page(req->image_addr) ||
	    req->image_addr > ULONG_MAX - (u64)req->nr_pages * PAGE_SIZE)
		return -EINVAL;
	p = kvzalloc_obj(*p, GFP_KERNEL_ACCOUNT);
	if (!p)
		return -ENOMEM;
	ram_leaves = kcalloc(req->nr_overlays, sizeof(*ram_leaves), GFP_KERNEL_ACCOUNT);
	if (!ram_leaves)
		goto fail;
	p->live = true;
	p->memslots = memslots;
	p->slots_generation = live_slots_generation(vmx->vcpu.kvm);
	if (memslots && (p->slots_generation & KVM_MEMSLOT_GEN_UPDATE_IN_PROGRESS)) {
		ret = -EAGAIN;
		goto fail;
	}
	p->key = req->reserved[0];
	p->ram_hva = req->image_addr;
	p->ram_pages = memslots ? max_pages : req->nr_pages;
	p->map_count = req->nr_overlays;
	p->data_limit = min(data_limit, p->ram_pages);
	if (copy_from_user(p->maps, u64_to_user_ptr(req->overlays_addr),
			   p->map_count * sizeof(p->maps[0]))) {
		ret = -EFAULT;
		goto fail;
	}
	for (i = 0; i < p->map_count; i++) {
		struct kvm_hbt_eptp_live_map *m = &p->maps[i];
		u16 *at;

		ret = -EINVAL;
		if (m->page >= max_pages || m->permissions & ~0x707U)
			goto fail;
		at = live_map_slot(p->maps, p->map_index, m->page);
		if (*at)
			goto fail;
		*at = i + 1;
		for (v = 0; v < 2; v++) {
			unsigned int perm = (m->permissions >> (v * 8)) & 7;
			u64 address = v ? m->helper_addr : m->normal_addr;

			if (((perm & 2) && !(perm & 1)) ||
			    (perm && !address && m->page >= p->ram_pages))
				goto fail;
		}
	}
	p->pinned = kvmalloc_array(KVM_HBT_EPTP_MAX_OVERLAYS + p->data_limit,
				   sizeof(*p->pinned), GFP_KERNEL_ACCOUNT);
	p->data_pages = kvmalloc_array(p->data_limit, sizeof(*p->data_pages),
				       GFP_KERNEL_ACCOUNT);
	if (!p->pinned || !p->data_pages) {
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
		pinned = live_pin(&vmx->vcpu, p, m->page, &p->pinned[p->pinned_count]);
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
		p->tables[v][0] = (void *)get_zeroed_page(GFP_KERNEL_ACCOUNT);
		if (!p->tables[v][0]) {
			ret = -ENOMEM;
			goto fail;
		}
		p->table_count[v] = 1;
		p->list[v] = __pa(p->tables[v][0]) | VMX_EPTP_MT_WB | VMX_EPTP_PWL_4;
		for (i = 0; i < p->map_count; i++) {
			struct kvm_hbt_eptp_live_map *m = &p->maps[i];
			u64 address = v ? m->helper_addr : m->normal_addr;
			unsigned int perm = (m->permissions >> (v * 8)) & 7;
			u64 value = 0;

			if (perm && address) {
				void *page = live_overlay_copy(p, address);

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
	old = p->key ? cached_context(vmx, p->key) : &vmx->hbt_eptp_cache;
	if (old) {
		free_probe(*old);
		*old = NULL;
	}
	vmx->hbt_eptp = p;
	return 0;
fail:
	free_probe(p);
	return ret;
}

/* Inactive-only transaction: prepare copies, pins and all EPT paths before
 * changing any leaf. Roll back new paths in reverse allocation order, so even
 * repeated failed appends cannot consume the bounded EPT table budget.
 */
static int append_live(struct vcpu_vmx *vmx, struct kvm_hbt_eptp_probe *req)
{
	struct vmx_hbt_eptp **slot = cached_context(vmx, req->reserved[0]);
	struct vmx_hbt_eptp *p = slot ? *slot : NULL;
	struct live_table_journal *journal __free(kfree) = NULL;
	struct append_leaf {
		u64 *entry[2], value[2], ram;
		bool data;
	} *leaves __free(kfree) = NULL;
	unsigned int count = req->nr_overlays, first, pins, i, j, v, max_pages;
	int ret = -ENOMEM;

	if (vmx->hbt_eptp)
		return -EBUSY;
	if (!p || !p->live)
		return -ENOENT;
	if (req->view || req->nr_pages || req->image_addr || !count ||
	    count > KVM_HBT_EPTP_MAX_OVERLAYS)
		return -EINVAL;
	if (count > KVM_HBT_EPTP_MAX_OVERLAYS - p->map_count)
		return -ENOSPC;
	if (p->memslots && p->slots_generation != live_slots_generation(vmx->vcpu.kvm))
		return -ESTALE;
	first = p->map_count;
	pins = p->pinned_count;
	max_pages = p->memslots ? KVM_HBT_EPTP_MEMSLOT_MAX_PAGES :
		KVM_HBT_EPTP_LIVE_MAX_PAGES;
	leaves = kcalloc(count, sizeof(*leaves), GFP_KERNEL_ACCOUNT);
	journal = kzalloc(struct_size(journal, links, 6 * count), GFP_KERNEL_ACCOUNT);
	if (!leaves || !journal)
		return -ENOMEM;
	if (copy_from_user(p->maps + first, u64_to_user_ptr(req->overlays_addr),
			   count * sizeof(p->maps[0]))) {
		ret = -EFAULT;
		goto fail;
	}
	for (i = first; i < first + count; i++) {
		struct kvm_hbt_eptp_live_map *m = &p->maps[i];

		ret = -EINVAL;
		if (m->page >= max_pages || m->permissions & ~0x707U)
			goto fail;
		for (j = 0; j < i; j++)
			if (p->maps[j].page == m->page)
				goto fail;
		for (v = 0; v < 2; v++) {
			unsigned int perm = (m->permissions >> (v * 8)) & 7;
			u64 address = v ? m->helper_addr : m->normal_addr;

			if (((perm & 2) && !(perm & 1)) ||
			    (perm && !address && m->page >= p->ram_pages))
				goto fail;
		}
	}
	/* New copies may alias either view of any existing private image. */
	p->map_count += count;
	for (i = 0; i < count; i++) {
		struct kvm_hbt_eptp_live_map *m = &p->maps[first + i];
		bool canonical = ((m->permissions & 7) && !m->normal_addr) ||
			((m->permissions & 0x700) && !m->helper_addr);

		for (j = 0; j < p->data_count; j++)
			if (p->data_pages[j] == m->page) {
				leaves[i].data = true;
				break;
			}
		for (v = 0; v < 2; v++) {
			leaves[i].entry[v] = live_leaf_pointer(p, v, m->page, journal);
			if (IS_ERR(leaves[i].entry[v])) {
				ret = PTR_ERR(leaves[i].entry[v]);
				goto fail;
			}
		}
		if (leaves[i].data) {
			/* Reuse its existing pin. A wholly private promotion also keeps
			 * that pin until destruction, bounded by the explicit map count.
			 */
			leaves[i].ram = *leaves[i].entry[0] & PAGE_MASK;
		} else if (canonical) {
			long n;

			if (p->pinned_count == KVM_HBT_EPTP_MAX_OVERLAYS + p->data_limit) {
				ret = -ENOSPC;
				goto fail;
			}
			n = live_pin(&vmx->vcpu, p, m->page, &p->pinned[p->pinned_count]);

			if (n != 1) {
				ret = n < 0 ? n : -EFAULT;
				goto fail;
			}
			leaves[i].ram = page_to_phys(p->pinned[p->pinned_count++]);
		}
		for (v = 0; v < 2; v++) {
			unsigned int perm = (m->permissions >> (v * 8)) & 7;
			u64 address = v ? m->helper_addr : m->normal_addr;

			if (perm && address) {
				void *page = live_overlay_copy(p, address);

				if (IS_ERR(page)) {
					ret = PTR_ERR(page);
					goto fail;
				}
				p->map_pages[v][first + i] = page;
				leaves[i].value[v] = __pa(page) | 0x70 | perm;
			} else if (perm) {
				leaves[i].value[v] = leaves[i].ram | 0x70 | perm;
			}
		}
	}
	if (p->memslots && p->slots_generation != live_slots_generation(vmx->vcpu.kvm)) {
		ret = -ESTALE;
		goto fail;
	}
	for (i = 0; i < count; i++) {
		*live_map_slot(p->maps, p->map_index, p->maps[first + i].page) = first + i + 1;
		for (v = 0; v < 2; v++)
			*leaves[i].entry[v] = leaves[i].value[v];
		if (leaves[i].data)
			for (j = 0; j < p->data_count; j++)
				if (p->data_pages[j] == p->maps[first + i].page) {
					p->data_pages[j] = p->data_pages[--p->data_count];
					break;
				}
	}
	p->tlb_valid = false;
	return 0;
fail:
	while (journal->count) {
		struct live_table_link *link = &journal->links[--journal->count];

		free_page((unsigned long)__va(*link->entry & PAGE_MASK));
		*link->entry = 0;
		p->table_count[link->view]--;
	}
	for (v = 0; v < 2; v++)
		for (i = first; i < first + count; i++) {
			free_page((unsigned long)p->map_pages[v][i]);
			p->map_pages[v][i] = NULL;
		}
	unpin_user_pages(p->pinned + pins, p->pinned_count - pins);
	p->pinned_count = pins;
	p->map_count = first;
	memset(p->maps + first, 0, count * sizeof(p->maps[0]));
	return ret;
}

/* Bounded, build-local indexes. Values are one-based descriptor/view indices;
 * no hash collision can substitute a different GPA or userspace address. */
static u16 *update_image_slot(struct vmx_hbt_eptp *p, u16 *index, u64 address)
{
	unsigned int at = hash_64(address, 11);

	while (index[at]) {
		unsigned int key = index[at] - 1;
		struct kvm_hbt_eptp_live_map *m = p->maps + key / 2;

		if (address == (key & 1 ? m->helper_addr : m->normal_addr))
			break;
		at = (at + 1) & 2047;
	}
	return index + at;
}

/* Replace private images without discarding learned data mappings or EPT
 * paths. The old parked context remains usable on every failure path. */
static int update_live(struct vcpu_vmx *vmx, struct kvm_hbt_eptp_probe *req)
{
	struct vmx_hbt_eptp **slot = cached_context(vmx, req->reserved[0]);
	struct vmx_hbt_eptp *p = slot ? *slot : NULL, *next = NULL;
	struct live_table_journal *journal __free(kfree) = NULL;
	u16 *indexes __free(kfree) = NULL;
	struct update_leaf {
		u64 *entry[2], value[2], ram;
		int old;
		bool data;
	} *leaves __free(kfree) = NULL;
	unsigned int count = req->nr_overlays, pins, i, j, v, max_pages, matched = 0;
	int ret = -ENOMEM;

	if (vmx->hbt_eptp)
		return -EBUSY;
	if (!p || !p->live)
		return -ENOENT;
	if (req->view || req->nr_pages || req->image_addr ||
	    count < p->map_count || count > KVM_HBT_EPTP_MAX_OVERLAYS || !count)
		return -EINVAL;
	if (p->memslots && p->slots_generation != live_slots_generation(vmx->vcpu.kvm))
		return -ESTALE;
	pins = p->pinned_count;
	max_pages = p->memslots ? KVM_HBT_EPTP_MEMSLOT_MAX_PAGES :
		KVM_HBT_EPTP_LIVE_MAX_PAGES;
	next = kvzalloc_obj(*next, GFP_KERNEL_ACCOUNT);
	leaves = kcalloc(count, sizeof(*leaves), GFP_KERNEL_ACCOUNT);
	journal = kzalloc(struct_size(journal, links, 6 * count), GFP_KERNEL_ACCOUNT);
	indexes = kcalloc(4096, sizeof(*indexes), GFP_KERNEL_ACCOUNT);
	if (!next || !leaves || !journal || !indexes)
		goto fail;
	next->map_count = count;
	if (copy_from_user(next->maps, u64_to_user_ptr(req->overlays_addr),
			   count * sizeof(next->maps[0]))) {
		ret = -EFAULT;
		goto fail;
	}
	for (i = 0; i < p->map_count; i++)
		*live_map_slot(p->maps, indexes, p->maps[i].page) = i + 1;
	for (i = 0; i < count; i++) {
		struct kvm_hbt_eptp_live_map *m = next->maps + i;
		u16 *at;

		ret = -EINVAL;
		if (m->page >= max_pages || m->permissions &
		    ~(0x707U | KVM_HBT_EPTP_KEEP_NORMAL | KVM_HBT_EPTP_KEEP_HELPER))
			goto fail;
		at = live_map_slot(next->maps, indexes + 1024, m->page);
		if (*at)
			goto fail;
		*at = i + 1;
		leaves[i].old = (int)*live_map_slot(p->maps, indexes, m->page) - 1;
		matched += leaves[i].old >= 0;
		for (v = 0; v < 2; v++) {
			unsigned int perm = (m->permissions >> (v * 8)) & 7;
			u64 address = v ? m->helper_addr : m->normal_addr;
			bool keep = m->permissions & (KVM_HBT_EPTP_KEEP_NORMAL << v);
			int old = leaves[i].old;

			if (((perm & 2) && !(perm & 1)) ||
			    (perm && !address && m->page >= p->ram_pages))
				goto fail;
			if (keep && (!perm || !address || old < 0 ||
			    !p->map_pages[v][old] ||
			    perm != ((p->maps[old].permissions >> (v * 8)) & 7)))
				goto fail;
		}
	}
	/* Updates may promote or reorder mappings but never silently drop one. */
	if (matched != p->map_count)
		goto fail;
	for (i = 0; i < count; i++) {
		struct kvm_hbt_eptp_live_map *m = next->maps + i;
		bool canonical = ((m->permissions & 7) && !m->normal_addr) ||
			((m->permissions & 0x700) && !m->helper_addr);

		for (v = 0; v < 2; v++) {
			leaves[i].entry[v] = live_leaf_pointer(p, v, m->page, journal);
			if (IS_ERR(leaves[i].entry[v])) {
				ret = PTR_ERR(leaves[i].entry[v]);
				goto fail;
			}
		}
		for (j = 0; leaves[i].old < 0 && j < p->data_count; j++)
			if (p->data_pages[j] == m->page) {
				leaves[i].data = true;
				leaves[i].ram = *leaves[i].entry[0] & PAGE_MASK;
				break;
			}
		if (canonical && leaves[i].old >= 0) {
			struct kvm_hbt_eptp_live_map *old = p->maps + leaves[i].old;

			for (v = 0; v < 2; v++)
				if (((old->permissions >> (v * 8)) & 7) &&
				    !(v ? old->helper_addr : old->normal_addr))
					leaves[i].ram = *leaves[i].entry[v] & PAGE_MASK;
		}
		if (canonical && !leaves[i].ram) {
			long n;

			/* Old promotion pins are retained until context destruction. */
			if (p->pinned_count == KVM_HBT_EPTP_MAX_OVERLAYS + p->data_limit) {
				ret = -ENOSPC;
				goto fail;
			}
			n = live_pin(&vmx->vcpu, p, m->page, &p->pinned[p->pinned_count]);
			if (n != 1) {
				ret = n < 0 ? n : -EFAULT;
				goto fail;
			}
			leaves[i].ram = page_to_phys(p->pinned[p->pinned_count++]);
		}
		for (v = 0; v < 2; v++) {
			unsigned int perm = (m->permissions >> (v * 8)) & 7;
			u64 address = v ? m->helper_addr : m->normal_addr;
			bool keep = m->permissions & (KVM_HBT_EPTP_KEEP_NORMAL << v);
			void *page = NULL;

			if (perm && address) {
				u16 *at = update_image_slot(next, indexes + 2048, address);

				if (*at) {
					unsigned int key = *at - 1, pi = key / 2, pv = key & 1;
					bool prior_keep = next->maps[pi].permissions &
						(KVM_HBT_EPTP_KEEP_NORMAL << pv);

					page = next->map_pages[pv][pi];
					if (keep != prior_keep ||
					    (keep && page != p->map_pages[v][leaves[i].old])) {
						ret = -EINVAL;
						goto fail;
					}
					get_page(virt_to_page(page));
				} else if (keep) {
					page = p->map_pages[v][leaves[i].old];
					get_page(virt_to_page(page));
				} else {
					page = copy_page_from_user(address);
					if (IS_ERR(page)) {
						ret = PTR_ERR(page);
						goto fail;
					}
				}
				*at = 1 + i * 2 + v;
				next->map_pages[v][i] = page;
				leaves[i].value[v] = __pa(page) | 0x70 | perm;
			} else if (perm) {
				leaves[i].value[v] = leaves[i].ram | 0x70 | perm;
			}
		}
	}
	if (p->memslots && p->slots_generation != live_slots_generation(vmx->vcpu.kvm)) {
		ret = -ESTALE;
		goto fail;
	}
	for (i = 0; i < count; i++) {
		for (v = 0; v < 2; v++)
			*leaves[i].entry[v] = leaves[i].value[v];
		if (leaves[i].data)
			for (j = 0; j < p->data_count; j++)
				if (p->data_pages[j] == next->maps[i].page) {
					p->data_pages[j] = p->data_pages[--p->data_count];
					break;
				}
		next->maps[i].permissions &= 0x707;
	}
	for (v = 0; v < 2; v++)
		for (i = 0; i < p->map_count; i++)
			free_page((unsigned long)p->map_pages[v][i]);
	memcpy(p->maps, next->maps, sizeof(p->maps));
	memcpy(p->map_index, indexes + 1024, sizeof(p->map_index));
	memcpy(p->map_pages, next->map_pages, sizeof(p->map_pages));
	p->map_count = count;
	p->tlb_valid = false;
	next->map_count = 0;
	free_probe(next);
	return 0;
fail:
	if (journal)
		while (journal->count) {
			struct live_table_link *link = &journal->links[--journal->count];

			free_page((unsigned long)__va(*link->entry & PAGE_MASK));
			*link->entry = 0;
			p->table_count[link->view]--;
		}
	unpin_user_pages(p->pinned + pins, p->pinned_count - pins);
	p->pinned_count = pins;
	free_probe(next);
	return ret;
}

/* Add ordinary RAM as data only. Explicit overlays keep their permissions;
 * code writes and all instruction fetches still require canonical recovery.
 * Pins are bounded by the original image or by validated KVM memory slots.
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
	if (p->memslots && p->slots_generation != live_slots_generation(vcpu->kvm))
		return -ESTALE;
	if (*live_map_slot(p->maps, p->map_index, page))
		return -EPERM;
	for (i = 0; i < p->data_count; i++)
		if (p->data_pages[i] == page)
			return 0;
	if (p->data_count == p->data_limit ||
	    p->pinned_count == KVM_HBT_EPTP_MAX_OVERLAYS + p->data_limit)
		return -ENOSPC;
	count = live_pin(vcpu, p, page, &pinned);
	if (count != 1)
		return count < 0 ? count : -EFAULT;
	p->tlb_valid = false;
	for (i = 0; i < 2; i++) {
		ret = live_leaf(p, i, page, page_to_phys(pinned) | 0x73);
		if (ret) {
			/* A partial mapping must never survive as a completed pin. */
			while (i--)
				live_leaf(p, i, page, 0);
			unpin_user_page(pinned);
			return ret;
		}
	}
	p->pinned[p->pinned_count++] = pinned;
	p->data_pages[p->data_count++] = page;
	kvm_vcpu_srcu_read_lock(vcpu);
	kvm_vcpu_mark_page_dirty(vcpu, page);
	kvm_vcpu_srcu_read_unlock(vcpu);
	return 0;
}

/* Userspace has verified a canonical instruction boundary (no helper scratch
 * registers). Only the exact faulting execute-only overlay can be relaxed.
 * Its canonical RAM, never the patched image, supplies both reads and fetches
 * until MTF. A write remains prohibited. The next exit restores the overlay. */
static int read_step(struct vcpu_vmx *vmx, struct kvm_hbt_eptp_probe *req)
{
	struct kvm_vcpu *vcpu = &vmx->vcpu;
	struct vmx_hbt_eptp *p = vmx->hbt_eptp;
	unsigned int page = req->nr_pages, at, perm;
	struct page *pinned;
	u64 *leaf;
	long ret;

	if (!p || !p->live || p->entered || p->read_step_leaf ||
	    req->view || req->image_addr || req->overlays_addr || req->nr_overlays ||
	    page >= p->ram_pages)
		return -EINVAL;
	if (!(vmcs_config.cpu_based_exec_ctrl & CPU_BASED_MONITOR_TRAP_FLAG) ||
	    (exec_controls_get(vmx) & CPU_BASED_MONITOR_TRAP_FLAG) || vcpu->guest_debug)
		return -EOPNOTSUPP;
	if (p->pending_event || vmx->fail || vmx_get_exit_reason(vcpu).failed_vmentry ||
	    vmx_get_exit_reason(vcpu).basic != EXIT_REASON_EPT_VIOLATION ||
	    (vmcs_readl(EXIT_QUALIFICATION) & 7) != 1 ||
	    (vmcs_read64(GUEST_PHYSICAL_ADDRESS) >> PAGE_SHIFT) != page)
		return -EPERM;
	at = *live_map_slot(p->maps, p->map_index, page);
	if (!at)
		return -ENOENT;
	perm = (p->maps[at - 1].permissions >> (p->view * 8)) & 7;
	if (perm != 4 || !p->map_pages[p->view][at - 1])
		return -EPERM;
	leaf = live_leaf_pointer(p, p->view, page, NULL);
	if (IS_ERR(leaf))
		return PTR_ERR(leaf);
	ret = live_pin(vcpu, p, page, &pinned);
	if (ret != 1)
		return ret < 0 ? ret : -EFAULT;
	p->read_step_leaf = leaf;
	p->read_step_saved = *leaf;
	p->read_step_page = pinned;
	p->read_step_exit = false;
	*leaf = page_to_phys(pinned) | 0x75; /* WB, ignore PAT, read/execute only */
	p->tlb_valid = false;
	return 0;
}

static int map_native(struct vcpu_vmx *vmx, struct kvm_hbt_eptp_probe *req)
{
	struct kvm_vcpu *vcpu = &vmx->vcpu;
	struct vmx_hbt_eptp *p = vmx->hbt_eptp;
	u64 *leaf[2];
	unsigned int v;
	int ret;

	if (!p || !p->live || p->entered || p->read_step_leaf || p->pending_event)
		return -EBUSY;
	if (vmx->fail || vmx_get_exit_reason(vcpu).failed_vmentry ||
	    vmx_get_exit_reason(vcpu).basic != EXIT_REASON_EPT_VIOLATION ||
	    (vmcs_readl(EXIT_QUALIFICATION) & 7) != 4 ||
	    (vmcs_read64(GUEST_PHYSICAL_ADDRESS) >> PAGE_SHIFT) != req->nr_pages ||
	    vmx_get_cpl_no_cache(vcpu) != 3)
		return -EPERM;
	if (p->native_count == ARRAY_SIZE(p->native_pages))
		return -ENOSPC;
	/* Same bounded, dirty-tracked canonical backing as ordinary data. Never
	 * override an explicit code/PTE/gate overlay or a readonly/MMIO slot. */
	ret = map_live_data(vcpu, p, req);
	if (ret)
		return ret;
	for (v = 0; v < 2; v++) {
		leaf[v] = live_leaf_pointer(p, v, req->nr_pages, NULL);
		if (IS_ERR(leaf[v]))
			return PTR_ERR(leaf[v]);
	}
	if ((*leaf[0] & 4) && (*leaf[1] & 4))
		return 0;
	for (v = 0; v < 2; v++)
		*leaf[v] |= 4;
	p->native_pages[p->native_count++] = req->nr_pages;
	p->tlb_valid = false;
	return 0;
}

static int read_maps(struct vmx_hbt_eptp *p, struct kvm_hbt_eptp_probe *req)
{
	struct kvm_hbt_eptp_overlay *maps __free(kfree) = NULL;
	unsigned int i;

	if (!p || !p->live)
		return -ENOENT;
	if (req->view > 1 || req->nr_pages || req->image_addr ||
	    req->nr_overlays > KVM_HBT_EPTP_MAX_OVERLAYS)
		return -EINVAL;
	maps = kmalloc_array(req->nr_overlays, sizeof(*maps), GFP_KERNEL_ACCOUNT);
	if (!maps)
		return -ENOMEM;
	if (copy_from_user(maps, u64_to_user_ptr(req->overlays_addr),
			   req->nr_overlays * sizeof(maps[0])))
		return -EFAULT;
	for (i = 0; i < req->nr_overlays; i++) {
		void *page = NULL;
		unsigned int at;

		if (maps[i].reserved)
			return -EINVAL;
		at = *live_map_slot(p->maps, p->map_index, maps[i].page);
		if (at)
			page = p->map_pages[req->view][at - 1];
		if (!page)
			return -ENOENT;
		if (copy_to_user(u64_to_user_ptr(maps[i].image_addr), page, PAGE_SIZE))
			return -EFAULT;
	}
	return 0;
}

/* The inactive object is deliberately absent from all VM-entry hooks. Guest
 * events and normal KVM execution therefore cannot see helper mappings/state.
 * Refresh failures leave it inactive, including a partially copied image.
 */
static int transfer_ranges(struct vmx_hbt_eptp *p,
			   struct kvm_hbt_eptp_probe *req, bool write)
{
	struct kvm_hbt_eptp_range *ranges __free(kfree) = NULL;
	void **backing __free(kfree) = NULL;
	unsigned int i, j;

	if (!p || !p->live)
		return -ENOENT;
	if (req->view || req->nr_pages || req->image_addr ||
	    !req->nr_overlays || req->nr_overlays > KVM_HBT_EPTP_MAX_OVERLAYS)
		return -EINVAL;
	ranges = memdup_user(u64_to_user_ptr(req->overlays_addr),
			    req->nr_overlays * sizeof(*ranges));
	if (IS_ERR(ranges)) {
		int error = PTR_ERR(ranges);

		ranges = NULL;
		return error;
	}
	backing = kmalloc_array(req->nr_overlays, sizeof(*backing), GFP_KERNEL_ACCOUNT);
	if (!backing)
		return -ENOMEM;
	for (i = 0; i < req->nr_overlays; i++) {
		struct kvm_hbt_eptp_range *r = &ranges[i];

		if (r->view > 1 || r->flags || !r->length ||
		    (unsigned int)r->offset + r->length > PAGE_SIZE ||
		    r->image_addr > U64_MAX - r->length)
			return -EINVAL;
		j = *live_map_slot(p->maps, p->map_index, r->page);
		if (!j || !p->map_pages[r->view][j - 1])
			return -ENOENT;
		j--;
		if (write && !((p->maps[j].permissions >> (r->view * 8)) & 2))
			return -EPERM;
		backing[i] = p->map_pages[r->view][j] + r->offset;
	}
	for (i = 0; i < req->nr_overlays; i++) {
		struct kvm_hbt_eptp_range *r = &ranges[i];

		if (write ? copy_from_user(backing[i], u64_to_user_ptr(r->image_addr), r->length) :
			    copy_to_user(u64_to_user_ptr(r->image_addr), backing[i], r->length))
			return -EFAULT;
	}
	return 0;
}

static int resume_live(struct vcpu_vmx *vmx, struct vmx_hbt_eptp **slot,
		       struct kvm_hbt_eptp_probe *ranges)
{
	struct vmx_hbt_eptp *p = slot ? *slot : NULL;
	unsigned int v, i;

	if (vmx->hbt_eptp)
		return -EBUSY;
	if (!p)
		return -ENOENT;
	if (p->memslots && p->slots_generation != live_slots_generation(vmx->vcpu.kvm))
		return -ESTALE;
	if (ranges) {
		int ret = transfer_ranges(p, ranges, true);

		if (ret)
			return ret;
	}
	for (v = 0; !ranges && v < 2; v++) {
		for (i = 0; i < p->map_count; i++) {
			u64 address = v ? p->maps[i].helper_addr : p->maps[i].normal_addr;
			unsigned int perm = (p->maps[i].permissions >> (v * 8)) & 7;
			bool copied = false;
			unsigned int pv, pi;

			/* Read/execute-only images are immutable until LIVE_CONFIG.
			 * Writable aliases share both backing and source; refresh once.
			 */
			if (!p->map_pages[v][i] || !(perm & 2))
				continue;
			for (pv = 0; pv <= v && !copied; pv++) {
				for (pi = 0; pi < (pv == v ? i : p->map_count); pi++) {
					if (p->map_pages[pv][pi] == p->map_pages[v][i] &&
					    ((p->maps[pi].permissions >> (pv * 8)) & 2)) {
						copied = true;
						break;
					}
				}
			}
			if (!copied && copy_from_user(p->map_pages[v][i],
						u64_to_user_ptr(address), PAGE_SIZE))
				return -EFAULT;
		}
	}
	p->view = 0;
	p->entered = false;
	p->pending_event = false;
	/* The refreshed overlays can include guest page tables. Invalidate
	 * combined translations even when the EPT leaves themselves are unchanged.
	 */
	p->tlb_valid = false;
	/* A canonical dirty-log collection may have cleared the first MAP_DATA
	 * marks while this context was parked. Retained writable leaves bypass
	 * the ordinary MMU, so conservatively mark them again on reactivation.
	 */
	kvm_vcpu_srcu_read_lock(&vmx->vcpu);
	for (i = 0; i < p->data_count; i++)
		kvm_vcpu_mark_page_dirty(&vmx->vcpu, p->data_pages[i]);
	for (i = 0; i < p->map_count; i++)
		if (((p->maps[i].permissions & 2) && !p->maps[i].normal_addr) ||
		    ((p->maps[i].permissions & 0x200) && !p->maps[i].helper_addr))
			kvm_vcpu_mark_page_dirty(&vmx->vcpu, p->maps[i].page);
	kvm_vcpu_srcu_read_unlock(&vmx->vcpu);
	*slot = NULL;
	vmx->hbt_eptp = p;
	return 0;
}

long vmx_hbt_eptp_probe(struct kvm_vcpu *vcpu, void __user *argp)
{
	struct kvm_hbt_eptp_probe req;
	struct vcpu_vmx *vmx;
	struct vmx_hbt_eptp *p;
	struct vmx_hbt_eptp **slot;
	u64 funcs;
	unsigned int i;
	int ret;

	if (!hbt_eptp_probe || is_td_vcpu(vcpu) || !enable_ept || !enable_vpid ||
	    !cpu_has_vmx_vmfunc() || rdmsrq_safe(MSR_IA32_VMX_VMFUNC, &funcs) ||
	    !(funcs & VMX_VMFUNC_EPTP_SWITCHING))
		return -EOPNOTSUPP;
	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;
	if (req.version != KVM_HBT_EPTP_VERSION ||
	    (req.flags && (req.flags != KVM_HBT_EPTP_MEMSLOTS ||
	     (req.operation != KVM_HBT_EPTP_LIVE_CONFIG &&
	      req.operation != KVM_HBT_EPTP_CACHE_CONFIG))) ||
	    req.reserved[1] || req.reserved[2])
		return -EINVAL;
	if (req.operation != KVM_HBT_EPTP_RESUME_RANGES &&
	    ((req.operation >= KVM_HBT_EPTP_CACHE_CONFIG &&
	      req.operation <= KVM_HBT_EPTP_CACHE_DROP) ||
	      (req.operation == KVM_HBT_EPTP_CACHE_APPEND ||
	       req.operation == KVM_HBT_EPTP_CACHE_UPDATE) ? !req.reserved[0] : req.reserved[0])
	   )
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
		req.flags |= KVM_HBT_EPTP_SHARED_OVERLAY | KVM_HBT_EPTP_LAZY_DATA |
			     KVM_HBT_EPTP_PERSISTENT | KVM_HBT_EPTP_CONTEXT_CACHE |
			     KVM_HBT_EPTP_RANGE_IO | KVM_HBT_EPTP_CONTEXT_IO |
			     KVM_HBT_EPTP_MEMSLOTS | KVM_HBT_EPTP_INCREMENTAL |
			     KVM_HBT_EPTP_UPDATE | KVM_HBT_EPTP_NATIVE_FETCH;
		if (vmcs_config.cpu_based_exec_ctrl & CPU_BASED_MONITOR_TRAP_FLAG)
			req.flags |= KVM_HBT_EPTP_NATIVE_READ;
		req.reserved[0] = KVM_HBT_EPTP_MAX_CONTEXTS;
		return copy_to_user(argp, &req, sizeof(req)) ? -EFAULT : 0;
	case KVM_HBT_EPTP_CONFIG:
		return configure_probe(vmx, &req);
	case KVM_HBT_EPTP_LIVE_CONFIG:
	case KVM_HBT_EPTP_CACHE_CONFIG:
		return configure_live(vmx, &req);
	case KVM_HBT_EPTP_CACHE_APPEND:
		return append_live(vmx, &req);
	case KVM_HBT_EPTP_CACHE_UPDATE:
		return update_live(vmx, &req);
	case KVM_HBT_EPTP_MAP_DATA:
		return map_live_data(vcpu, p, &req);
	case KVM_HBT_EPTP_READ_STEP:
		return read_step(vmx, &req);
	case KVM_HBT_EPTP_MAP_NATIVE:
		return map_native(vmx, &req);
	case KVM_HBT_EPTP_READ_MAPS:
		return read_maps(p, &req);
	case KVM_HBT_EPTP_READ_RANGES:
		ret = transfer_ranges(p, &req, false);
		if (ret)
			return ret;
		req.view = p->view;
		return copy_to_user(argp, &req, sizeof(req)) ? -EFAULT : 0;
	case KVM_HBT_EPTP_RESUME_RANGES:
		slot = req.reserved[0] ? cached_context(vmx, req.reserved[0]) :
			&vmx->hbt_eptp_cache;
		return resume_live(vmx, slot, &req);
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
	case KVM_HBT_EPTP_SUSPEND:
		if (req.nr_pages || req.view || req.image_addr ||
		    req.overlays_addr || req.nr_overlays)
			return -EINVAL;
		if (!p || !p->live || p->entered)
			return -EBUSY;
		cancel_read_step(p);
		revoke_native_pages(p);
		if (p->key) {
			park_context(vmx, p);
		} else {
			free_probe(vmx->hbt_eptp_cache);
			vmx->hbt_eptp_cache = p;
		}
		vmx->hbt_eptp = NULL;
		return 0;
	case KVM_HBT_EPTP_RESUME:
	case KVM_HBT_EPTP_CACHE_RESUME:
	case KVM_HBT_EPTP_CACHE_DROP:
		if (req.nr_pages || req.view || req.image_addr ||
		    req.overlays_addr || req.nr_overlays)
			return -EINVAL;
		slot = req.reserved[0] ? cached_context(vmx, req.reserved[0]) :
			&vmx->hbt_eptp_cache;
		if (req.operation != KVM_HBT_EPTP_CACHE_DROP)
			return resume_live(vmx, slot, NULL);
		if (p && p->key == req.reserved[0])
			return -EBUSY;
		if (!slot || !*slot)
			return -ENOENT;
		free_probe(*slot);
		*slot = NULL;
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

/* Recover before EVERY injected event, including external IRQs. Deferring an
 * IRQ across another private slice permits SYSCALL to clear IF. Requeueing it
 * as already injected then bypasses interrupt_allowed() and can cause an
 * invalid VM entry. Leave the injection intact and never advance guest RIP
 * before userspace has restored a canonical checkpoint.
 */
bool vmx_hbt_eptp_pending_event(struct vcpu_vmx *vmx)
{
	struct vmx_hbt_eptp *p = vmx->hbt_eptp;
	u32 info = vmcs_read32(VM_ENTRY_INTR_INFO_FIELD);

	if (!(info & INTR_INFO_VALID_MASK) &&
	    !(p->memslots && p->slots_generation !=
	      live_slots_generation(vmx->vcpu.kvm)))
		return false;
	p->pending_event = true;
	cancel_read_step(p);
	return true;
}

void vmx_hbt_eptp_enter(struct vcpu_vmx *vmx)
{
	struct vmx_hbt_eptp *p = vmx->hbt_eptp;

	p->saved_eptp = vmcs_read64(EPT_POINTER);
	p->saved_function = vmcs_read64(VM_FUNCTION_CONTROL);
	p->saved_list = vmcs_read64(EPTP_LIST_ADDRESS);
	p->saved_secondary = secondary_exec_controls_get(vmx);
	p->saved_primary = exec_controls_get(vmx);
	p->saved_exceptions = vmcs_read32(EXCEPTION_BITMAP);
	if (p->live) {
		u32 slice = max_t(u32, 1, (tsc_khz / 10) >>
				vmx_misc_preemption_timer_rate(vmcs_config.misc));

		/* Share the hardware timer with KVM's LAPIC deadline and immediate
		 * exit requests. A private slice must never postpone either one. */
		p->saved_pin = pin_controls_get(vmx);
		p->saved_timer = vmcs_read32(VMX_PREEMPTION_TIMER_VALUE);
		if (p->saved_pin & PIN_BASED_VMX_PREEMPTION_TIMER)
			slice = min(slice, p->saved_timer);
		pin_controls_set(vmx, p->saved_pin | PIN_BASED_VMX_PREEMPTION_TIMER);
		vmcs_write32(VMX_PREEMPTION_TIMER_VALUE, slice);
	}
	/* New objects start invalid. Refreshes and leaf changes invalidate them;
	 * movement to another CPU always flushes there, including a return to a
	 * previously used CPU. Thus freed/reused roots cannot inherit stale entries.
	 * The vCPU is serialized and migration is disabled throughout this hook.
	 */
	if (!READ_ONCE(hbt_eptp_tlb_cache) || !p->tlb_valid ||
	    p->tlb_cpu != smp_processor_id()) {
		ept_sync_context(p->list[0]);
		ept_sync_context(p->list[1]);
		p->tlb_cpu = smp_processor_id();
		p->tlb_valid = true;
	}
	vmcs_write64(EPTP_LIST_ADDRESS, __pa(p->list));
	vmcs_write64(VM_FUNCTION_CONTROL, VMX_VMFUNC_EPTP_SWITCHING);
	vmcs_write64(EPT_POINTER, p->list[p->view]);
	vmcs_write32(EXCEPTION_BITMAP, ~0U);
	secondary_exec_controls_set(vmx, (p->saved_secondary | SECONDARY_EXEC_ENABLE_VMFUNC) &
				    ~(SECONDARY_EXEC_ENABLE_PML | SECONDARY_EXEC_EPT_VIOLATION_VE));
	if (p->read_step_leaf)
		exec_controls_set(vmx, p->saved_primary | CPU_BASED_MONITOR_TRAP_FLAG);
	p->entered = true;
}

void vmx_hbt_eptp_leave(struct vcpu_vmx *vmx)
{
	struct vmx_hbt_eptp *p = vmx->hbt_eptp;
	u64 active = vmcs_read64(EPT_POINTER);

	p->view = active == p->list[1];
	p->read_step_exit = !!p->read_step_leaf;
	if (p->read_step_leaf) {
		exec_controls_set(vmx, p->saved_primary);
		cancel_read_step(p);
	}
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
	bool timer_pending = false;
	bool stepped = vmx->hbt_eptp->read_step_exit;

	vmx->hbt_eptp->read_step_exit = false;

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
	/* HBT bypasses the ordinary VMX timer fast path. Account for an expired
	 * guest deadline here, even when another exit won the race. Queue the IRQ
	 * through KVM, then require canonical recovery before guest injection. */
	if (vmx->hbt_eptp->live && vmx->hv_deadline_tsc != -1 &&
	    rdtsc_ordered() >= vmx->hv_deadline_tsc) {
		kvm_lapic_expired_hv_timer(vcpu);
		timer_pending = true;
	}
	if (stepped && !timer_pending &&
	    vmx_get_exit_reason(vcpu).basic == EXIT_REASON_MONITOR_TRAP_FLAG)
		return 1;
	run->exit_reason = KVM_EXIT_HBT_EPTP;
	run->internal.suberror = vmx_get_exit_reason(vcpu).full;
	run->internal.ndata = 7;
	run->internal.data[0] = vmx->hbt_eptp->view;
	run->internal.data[1] = vmcs_readl(EXIT_QUALIFICATION);
	run->internal.data[2] = vmcs_read32(VM_EXIT_INSTRUCTION_LEN);
	run->internal.data[3] = vmcs_read32(VM_EXIT_INTR_INFO);
	run->internal.data[4] = kvm_rip_read(vcpu);
	run->internal.data[5] = timer_pending;
	run->internal.data[6] = vmx_get_exit_reason(vcpu).basic == EXIT_REASON_EPT_VIOLATION ?
		vmcs_read64(GUEST_PHYSICAL_ADDRESS) : 0;
	/* A split read may fault on a second execute-only page. Tell userspace
	 * that this step was rolled back so it does not ping-pong the two aliases. */
	if (stepped) {
		run->internal.ndata = 8;
		run->internal.data[7] = 1;
	}
	return 0;
}
