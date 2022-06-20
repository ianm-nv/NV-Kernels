// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023-2025 ARM Ltd.
 */

#include <linux/kvm_host.h>
#include <linux/memblock.h>

#include <asm/kvm_emulate.h>
#include <asm/kvm_mmu.h>
#include <asm/kvm_pgtable.h>
#include <asm/rmi_cmds.h>
#include <asm/virt.h>

static unsigned long rmm_feat_reg0;
static unsigned long rmm_feat_reg1;

#define RMM_RTT_BLOCK_LEVEL	2
#define RMM_RTT_MAX_LEVEL	3

#define RMM_L2_BLOCK_SIZE	PMD_SIZE

static inline unsigned long rmi_rtt_level_mapsize(int level)
{
	if (WARN_ON(level > RMM_RTT_MAX_LEVEL))
		return PAGE_SIZE;

	return (1UL << ARM64_HW_PGTABLE_LEVEL_SHIFT(level));
}

static bool rmi_has_feature(unsigned long feature)
{
	return !!u64_get_bits(rmm_feat_reg0, feature);
}

static int rmi_check_version(void)
{
	struct arm_smccc_res res;
	unsigned short version_major, version_minor;
	unsigned long host_version = RMI_ABI_VERSION(RMI_ABI_MAJOR_VERSION,
						     RMI_ABI_MINOR_VERSION);
	unsigned long aa64pfr0 = read_sanitised_ftr_reg(SYS_ID_AA64PFR0_EL1);

	/* If RME isn't supported, then RMI can't be */
	if (cpuid_feature_extract_unsigned_field(aa64pfr0, ID_AA64PFR0_EL1_RME_SHIFT) == 0)
		return -ENXIO;

	arm_smccc_1_1_invoke(SMC_RMI_VERSION, host_version, &res);

	if (res.a0 == SMCCC_RET_NOT_SUPPORTED)
		return -ENXIO;

	version_major = RMI_ABI_VERSION_GET_MAJOR(res.a1);
	version_minor = RMI_ABI_VERSION_GET_MINOR(res.a1);

	if (res.a0 != RMI_SUCCESS) {
		unsigned short high_version_major, high_version_minor;

		high_version_major = RMI_ABI_VERSION_GET_MAJOR(res.a2);
		high_version_minor = RMI_ABI_VERSION_GET_MINOR(res.a2);

		kvm_err("Unsupported RMI ABI (v%d.%d - v%d.%d) we want v%d.%d\n",
			version_major, version_minor,
			high_version_major, high_version_minor,
			RMI_ABI_MAJOR_VERSION,
			RMI_ABI_MINOR_VERSION);
		return -ENXIO;
	}

	kvm_info("RMI ABI version %d.%d\n", version_major, version_minor);

	return 0;
}

/*
 * These are the 'default' sizes when passing 0 as the tracking_region_size.
 * TODO: Support other granule sizes
 */
#ifdef CONFIG_PAGE_SIZE_4KB
#define RMM_GRANULE_TRACKING_SIZE	SZ_1G
#elif defined(CONFIG_PAGE_SIZE_16KB)
#define RMM_GRANULE_TRACKING_SIZE	SZ_32M
#elif defined(CONFIG_PAGE_SIZE_64KB)
#define RMM_GRANULE_TRACKING_SIZE	SZ_512M
#endif

static int rmi_configure(void)
{
	struct rmm_config *config __free(free_page) = NULL;
	unsigned long ret;

	config = (struct rmm_config *)get_zeroed_page(GFP_KERNEL);
	if (!config)
		return -ENOMEM;

	switch (PAGE_SIZE) {
	case SZ_4K:
		config->rmi_granule_size = RMI_GRANULE_SIZE_4KB;
		break;
	case SZ_16K:
		config->rmi_granule_size = RMI_GRANULE_SIZE_16KB;
		break;
	case SZ_64K:
		config->rmi_granule_size = RMI_GRANULE_SIZE_64KB;
		break;
	default:
		kvm_err("Unsupported PAGE_SIZE for RMM\n");
		return -EINVAL;
	}

	ret = rmi_rmm_config_set(virt_to_phys(config));
	if (ret) {
		kvm_err("RMM config set failed\n");
		return -EINVAL;
	}

	ret = rmi_rmm_activate();
	if (ret) {
		kvm_err("RMM activate failed\n");
		return -ENXIO;
	}

	return 0;
}

static int rmi_verify_memory_tracking(phys_addr_t start, phys_addr_t end)
{
	start = ALIGN_DOWN(start, RMM_GRANULE_TRACKING_SIZE);
	end = ALIGN(end, RMM_GRANULE_TRACKING_SIZE);

	while (start < end) {
		unsigned long ret, category, state;

		ret = rmi_granule_tracking_get(start, &category, &state);
		if (ret != RMI_SUCCESS ||
		    state != RMI_TRACKING_FINE ||
		    category != RMI_MEM_CATEGORY_CONVENTIONAL) {
			/* TODO: Set granule tracking in this case */
			kvm_err("Granule tracking for region isn't fine/conventional: %llx",
				start);
			return -ENODEV;
		}
		start += RMM_GRANULE_TRACKING_SIZE;
	}

	return 0;
}

static unsigned long rmi_l0gpt_size(void)
{
	return 1UL << (30 + FIELD_GET(RMI_FEATURE_REGISTER_1_L0GPTSZ,
				      rmm_feat_reg1));
}

static int rmi_create_gpts(phys_addr_t start, phys_addr_t end)
{
	unsigned long l0gpt_sz = rmi_l0gpt_size();

	start = ALIGN_DOWN(start, l0gpt_sz);
	end = ALIGN(end, l0gpt_sz);

	while (start < end) {
		int ret = rmi_gpt_l1_create(start);

		if (ret && ret != RMI_ERROR_GPT) {
			/*
			 * FIXME: Handle SRO so that memory can be donated for
			 * the tables.
			 */
			kvm_err("GPT Level1 table missing for %llx\n", start);
			return -ENOMEM;
		}
		start += l0gpt_sz;
	}

	return 0;
}

static int rmi_init_metadata(void)
{
	phys_addr_t start, end;
	const struct memblock_region *r;

	for_each_mem_region(r) {
		int ret;

		start = memblock_region_memory_base_pfn(r) << PAGE_SHIFT;
		end = memblock_region_memory_end_pfn(r) << PAGE_SHIFT;
		ret = rmi_verify_memory_tracking(start, end);
		if (ret)
			return ret;
		ret = rmi_create_gpts(start, end);
		if (ret)
			return ret;
	}

	return 0;
}

u32 kvm_realm_ipa_limit(void)
{
	return u64_get_bits(rmm_feat_reg0, RMI_FEATURE_REGISTER_0_S2SZ);
}

static int get_start_level(struct realm *realm)
{
	return 4 - stage2_pgtable_levels(realm->ia_bits);
}

static int undelegate_range(phys_addr_t phys, unsigned long size)
{
	unsigned long ret;
	unsigned long top = phys + size;
	unsigned long out_top;

	while (phys < top) {
		ret = rmi_granule_range_undelegate(phys, top, &out_top);
		if (ret == RMI_SUCCESS)
			phys = out_top;
		else if (ret != RMI_BUSY && ret != RMI_BLOCKED)
			return ret;
	}

	return ret;
}

static int undelegate_page(phys_addr_t phys)
{
	return undelegate_range(phys, PAGE_SIZE);
}

static int free_delegated_page(phys_addr_t phys)
{
	if (WARN_ON(undelegate_page(phys))) {
		/* Undelegate failed: leak the page */
		return -EBUSY;
	}

	free_page((unsigned long)phys_to_virt(phys));

	return 0;
}

static void free_rtt(phys_addr_t phys)
{
	if (free_delegated_page(phys))
		return;

	kvm_account_pgtable_pages(phys_to_virt(phys), -1);
}

static int realm_rtt_destroy(struct realm *realm, unsigned long addr,
			     int level, phys_addr_t *rtt_granule,
			     unsigned long *next_addr)
{
	unsigned long out_rtt;
	int ret;

	ret = rmi_rtt_destroy(virt_to_phys(realm->rd), addr, level,
			      &out_rtt, next_addr);

	*rtt_granule = out_rtt;

	return ret;
}

static int realm_tear_down_rtt_level(struct realm *realm, int level,
				     unsigned long start, unsigned long end)
{
	ssize_t map_size;
	unsigned long addr, next_addr;

	if (WARN_ON(level > RMM_RTT_MAX_LEVEL))
		return -EINVAL;

	map_size = rmi_rtt_level_mapsize(level - 1);

	for (addr = start; addr < end; addr = next_addr) {
		phys_addr_t rtt_granule;
		int ret;
		unsigned long align_addr = ALIGN(addr, map_size);

		next_addr = ALIGN(addr + 1, map_size);

		if (next_addr > end || align_addr != addr) {
			/*
			 * The target range is smaller than what this level
			 * covers, recurse deeper.
			 */
			ret = realm_tear_down_rtt_level(realm,
							level + 1,
							addr,
							min(next_addr, end));
			if (ret)
				return ret;
			continue;
		}

		ret = realm_rtt_destroy(realm, addr, level,
					&rtt_granule, &next_addr);

		switch (RMI_RETURN_STATUS(ret)) {
		case RMI_SUCCESS:
			free_rtt(rtt_granule);
			break;
		case RMI_ERROR_RTT:
			if (next_addr > addr) {
				/* Missing RTT, skip */
				break;
			}
			/*
			 * We tear down the RTT range for the full IPA
			 * space, after everything is unmapped. Also we
			 * descend down only if we cannot tear down a
			 * top level RTT. Thus RMM must be able to walk
			 * to the requested level. e.g., a block mapping
			 * exists at L1 or L2.
			 */
			if (WARN_ON(RMI_RETURN_INDEX(ret) != level))
				return -EBUSY;
			if (WARN_ON(level == RMM_RTT_MAX_LEVEL))
				return -EBUSY;

			/*
			 * The table has active entries in it, recurse deeper
			 * and tear down the RTTs.
			 */
			next_addr = ALIGN(addr + 1, map_size);
			ret = realm_tear_down_rtt_level(realm,
							level + 1,
							addr,
							next_addr);
			if (ret)
				return ret;
			/*
			 * Now that the child RTTs are destroyed,
			 * retry at this level.
			 */
			next_addr = addr;
			break;
		default:
			WARN_ON(1);
			return -ENXIO;
		}
	}

	return 0;
}

static int realm_tear_down_rtt_range(struct realm *realm,
				     unsigned long start, unsigned long end)
{
	/*
	 * Root level RTTs can only be destroyed after the RD is destroyed. So
	 * tear down everything below the root level
	 */
	return realm_tear_down_rtt_level(realm, get_start_level(realm) + 1,
					 start, end);
}

void kvm_realm_destroy_rtts(struct kvm *kvm)
{
	struct realm *realm = &kvm->arch.realm;
	unsigned int ia_bits = realm->ia_bits;

	WARN_ON(realm_tear_down_rtt_range(realm, 0, (1UL << ia_bits)));
}

static int realm_ensure_created(struct kvm *kvm)
{
	/* Provided in later patch */
	return -ENXIO;
}

int kvm_activate_realm(struct kvm *kvm)
{
	struct realm *realm = &kvm->arch.realm;
	int ret;

	if (kvm_realm_state(kvm) >= REALM_STATE_ACTIVE)
		return 0;

	if (!irqchip_in_kernel(kvm)) {
		/* Userspace irqchip not yet supported with realms */
		return -EOPNOTSUPP;
	}

	guard(mutex)(&kvm->arch.config_lock);
	/* Check again with the lock held */
	if (kvm_realm_state(kvm) >= REALM_STATE_ACTIVE)
		return 0;

	ret = realm_ensure_created(kvm);
	if (ret)
		return ret;

	/* Mark state as dead in case we fail */
	WRITE_ONCE(realm->state, REALM_STATE_DEAD);

	ret = rmi_realm_activate(virt_to_phys(realm->rd));
	if (ret)
		return -ENXIO;

	WRITE_ONCE(realm->state, REALM_STATE_ACTIVE);
	return 0;
}

void kvm_destroy_realm(struct kvm *kvm)
{
	struct realm *realm = &kvm->arch.realm;
	size_t pgd_size = kvm_pgtable_stage2_pgd_size(kvm->arch.mmu.vtcr);

	write_lock(&kvm->mmu_lock);
	kvm_stage2_unmap_range(&kvm->arch.mmu, 0,
			       BIT(realm->ia_bits - 1), true);
	write_unlock(&kvm->mmu_lock);

	if (realm->params) {
		free_page((unsigned long)realm->params);
		realm->params = NULL;
	}

	if (!kvm_realm_is_created(kvm))
		return;

	WRITE_ONCE(realm->state, REALM_STATE_DYING);

	if (realm->rd) {
		phys_addr_t rd_phys = virt_to_phys(realm->rd);

		kvm_realm_destroy_rtts(kvm);

		if (WARN_ON(rmi_realm_destroy(rd_phys)))
			return;
		free_delegated_page(rd_phys);
		realm->rd = NULL;
	}

	if (WARN_ON(undelegate_range(kvm->arch.mmu.pgd_phys, pgd_size)))
		return;

	WRITE_ONCE(realm->state, REALM_STATE_DEAD);

	/* Now that the Realm is destroyed, free the entry level RTTs */
	kvm_free_stage2_pgd(&kvm->arch.mmu);
}

int kvm_init_realm_vm(struct kvm *kvm)
{
	kvm->arch.realm.params = (void *)get_zeroed_page(GFP_KERNEL);

	if (!kvm->arch.realm.params)
		return -ENOMEM;
	return 0;
}

static int rmm_check_features(void)
{
	if (kvm_lpa2_is_enabled() && !rmi_has_feature(RMI_FEATURE_REGISTER_0_LPA2)) {
		kvm_err("RMM doesn't support LPA2");
		return -ENXIO;
	}

	return 0;
}

void kvm_init_rmi(void)
{
	/* Continue without realm support if we can't agree on a version */
	if (rmi_check_version())
		return;

	if (WARN_ON(rmi_features(0, &rmm_feat_reg0)))
		return;
	if (WARN_ON(rmi_features(1, &rmm_feat_reg1)))
		return;

	if (rmm_check_features())
		return;
	if (rmi_configure())
		return;
	if (rmi_init_metadata())
		return;

	/* Future patch will enable static branch kvm_rmi_is_available */
}
