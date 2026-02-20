// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023-2025 ARM Ltd.
 */

#include <linux/kvm_host.h>

#include <asm/rmi_cmds.h>
#include <asm/virt.h>

static unsigned long rmm_feat_reg0;
static unsigned long rmm_feat_reg1;

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

void kvm_init_rmi(void)
{
	/* Continue without realm support if we can't agree on a version */
	if (rmi_check_version())
		return;

	if (WARN_ON(rmi_features(0, &rmm_feat_reg0)))
		return;
	if (WARN_ON(rmi_features(1, &rmm_feat_reg1)))
		return;

	if (rmi_configure())
		return;

	/* Future patch will enable static branch kvm_rmi_is_available */
}
