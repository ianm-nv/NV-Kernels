/* SPDX-License-Identifier: GPL-2.0
 * SPDX-FileCopyrightText: Copyright (C) 2023 ARM Ltd.
 * SPDX-FileCopyrightText: Copyright (C) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *
 * The values and structures in this file are from the Realm Management Monitor
 * specification (DEN0137) version 1.0-rel0:
 * https://developer.arm.com/documentation/den0137/1-0rel0/
 */

#ifndef __ASM_RMI_SMC_H
#define __ASM_RMI_SMC_H

#include <linux/arm-smccc.h>

#define SMC_RMI_CALL(func)				\
	ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL,		\
			   ARM_SMCCC_SMC_64,		\
			   ARM_SMCCC_OWNER_STANDARD,	\
			   (func))

#define SMC_RMI_VERSION			SMC_RMI_CALL(0x0150)
#define SMC_RMI_GRANULE_DELEGATE	SMC_RMI_CALL(0x0151)
#define SMC_RMI_GRANULE_UNDELEGATE	SMC_RMI_CALL(0x0152)
#define SMC_RMI_DATA_CREATE		SMC_RMI_CALL(0x0153)
#define SMC_RMI_DATA_CREATE_UNKNOWN	SMC_RMI_CALL(0x0154)
#define SMC_RMI_DATA_DESTROY		SMC_RMI_CALL(0x0155)

#define SMC_RMI_PDEV_AUX_COUNT		SMC_RMI_CALL(0x0156)

#define SMC_RMI_REALM_ACTIVATE		SMC_RMI_CALL(0x0157)
#define SMC_RMI_REALM_CREATE		SMC_RMI_CALL(0x0158)
#define SMC_RMI_REALM_DESTROY		SMC_RMI_CALL(0x0159)
#define SMC_RMI_REC_CREATE		SMC_RMI_CALL(0x015a)
#define SMC_RMI_REC_DESTROY		SMC_RMI_CALL(0x015b)
#define SMC_RMI_REC_ENTER		SMC_RMI_CALL(0x015c)
#define SMC_RMI_RTT_CREATE		SMC_RMI_CALL(0x015d)
#define SMC_RMI_RTT_DESTROY		SMC_RMI_CALL(0x015e)
#define SMC_RMI_RTT_MAP_UNPROTECTED	SMC_RMI_CALL(0x015f)

#define SMC_RMI_RTT_READ_ENTRY		SMC_RMI_CALL(0x0161)
#define SMC_RMI_RTT_UNMAP_UNPROTECTED	SMC_RMI_CALL(0x0162)

#define SMC_RMI_RTT_DEV_MEM_VALIDATE	SMC_RMI_CALL(0x0163)

#define SMC_RMI_PSCI_COMPLETE		SMC_RMI_CALL(0x0164)
#define SMC_RMI_FEATURES		SMC_RMI_CALL(0x0165)
#define SMC_RMI_RTT_FOLD		SMC_RMI_CALL(0x0166)
#define SMC_RMI_REC_AUX_COUNT		SMC_RMI_CALL(0x0167)
#define SMC_RMI_RTT_INIT_RIPAS		SMC_RMI_CALL(0x0168)
#define SMC_RMI_RTT_SET_RIPAS		SMC_RMI_CALL(0x0169)

#define SMC_RMI_DEV_MEM_MAP		SMC_RMI_CALL(0x0172)
#define SMC_RMI_DEV_MEM_UNMAP		SMC_RMI_CALL(0x0173)

#define SMC_RMI_PDEV_COMMUNICATE	SMC_RMI_CALL(0x0175)
#define SMC_RMI_PDEV_CREATE		SMC_RMI_CALL(0x0176)
#define SMC_RMI_PDEV_DESTROY		SMC_RMI_CALL(0x0177)
#define SMC_RMI_PDEV_GET_STATE		SMC_RMI_CALL(0x0178)
#define SMC_RMI_PDEV_RESET		SMC_RMI_CALL(0x0179)
#define SMC_RMI_PDEV_NOTIFY		SMC_RMI_CALL(0x017A)
#define SMC_RMI_PDEV_SET_PUBKEY		SMC_RMI_CALL(0x017B)
#define SMC_RMI_PDEV_STOP		SMC_RMI_CALL(0x017C)

#define SMC_RMI_VDEV_ABORT		SMC_RMI_CALL(0x0185)
#define SMC_RMI_VDEV_COMMUNICATE	SMC_RMI_CALL(0x0186)
#define SMC_RMI_VDEV_CREATE		SMC_RMI_CALL(0x0187)
#define SMC_RMI_VDEV_DESTROY		SMC_RMI_CALL(0x0188)
#define SMC_RMI_VDEV_GET_STATE		SMC_RMI_CALL(0x0189)
#define SMC_RMI_VDEV_STOP		SMC_RMI_CALL(0x018A)
#define SMC_RMI_VDEV_AUX_COUNT		SMC_RMI_CALL(0x0160)
#define SMC_RMI_VDEV_COMPLETE		SMC_RMI_CALL(0x018E)

#define SMC_RMI_MEC_SET_SHARED		SMC_RMI_CALL(0x018C)
#define SMC_RMI_MEC_SET_PRIVATE		SMC_RMI_CALL(0x018D)

#define RMI_ABI_MAJOR_VERSION	1
#define RMI_ABI_MINOR_VERSION	0

#define RMI_ABI_VERSION_GET_MAJOR(version) ((version) >> 16)
#define RMI_ABI_VERSION_GET_MINOR(version) ((version) & 0xFFFF)
#define RMI_ABI_VERSION(major, minor)      (((major) << 16) | (minor))

#define RMI_UNASSIGNED			0
#define RMI_ASSIGNED			1
#define RMI_TABLE			2
#define RMI_ASSIGNED_DEV		3

#define RMI_RETURN_STATUS(ret)		((ret) & 0xFF)
#define RMI_RETURN_INDEX(ret)		(((ret) >> 8) & 0xFF)

#define RMI_SUCCESS		0
#define RMI_ERROR_INPUT		1
#define RMI_ERROR_REALM		2
#define RMI_ERROR_REC		3
#define RMI_ERROR_RTT		4
#define RMI_ERROR_DEVICE	5

enum rmi_ripas {
	RMI_EMPTY = 0,
	RMI_RAM = 1,
	RMI_DESTROYED = 2,
	RMI_DEV = 3,
};

#define RMI_NO_MEASURE_CONTENT	0
#define RMI_MEASURE_CONTENT	1

#define RMI_FEATURE_REGISTER_0_S2SZ		GENMASK(7, 0)
#define RMI_FEATURE_REGISTER_0_LPA2		BIT(8)
#define RMI_FEATURE_REGISTER_0_SVE_EN		BIT(9)
#define RMI_FEATURE_REGISTER_0_SVE_VL		GENMASK(13, 10)
#define RMI_FEATURE_REGISTER_0_NUM_BPS		GENMASK(19, 14)
#define RMI_FEATURE_REGISTER_0_NUM_WPS		GENMASK(25, 20)
#define RMI_FEATURE_REGISTER_0_PMU_EN		BIT(26)
#define RMI_FEATURE_REGISTER_0_PMU_NUM_CTRS	GENMASK(31, 27)
#define RMI_FEATURE_REGISTER_0_HASH_SHA_256	BIT(32)
#define RMI_FEATURE_REGISTER_0_HASH_SHA_512	BIT(33)
#define RMI_FEATURE_REGISTER_0_GICV3_NUM_LRS	GENMASK(37, 34)
#define RMI_FEATURE_REGISTER_0_MAX_RECS_ORDER	GENMASK(41, 38)
#define RMI_FEATURE_REGISTER_0_DA		BIT(42)
#define RMI_FEATURE_REGISTER_0_Reserved		GENMASK(63, 43)

#define RMI_REALM_PARAM_FLAG_LPA2		BIT(0)
#define RMI_REALM_PARAM_FLAG_SVE		BIT(1)
#define RMI_REALM_PARAM_FLAG_PMU		BIT(2)

/*
 * Note many of these fields are smaller than u64 but all fields have u64
 * alignment, so use u64 to ensure correct alignment.
 */
struct realm_params {
	union { /* 0x0 */
		struct {
			u64 flags;
			u64 s2sz;
			u64 sve_vl;
			u64 num_bps;
			u64 num_wps;
			u64 pmu_num_ctrs;
			u64 hash_algo;
		};
		u8 padding0[0x400];
	};
	union { /* 0x400 */
		u8 rpv[64];
		u8 padding1[0x400];
	};
	union { /* 0x800 */
		struct {
			u64 vmid;
			u64 rtt_base;
			s64 rtt_level_start;
			u64 rtt_num_start;
			u64 flags1;
			u64 mecid;
		};
		u8 padding2[0x800];
	};
};

/*
 * The number of GPRs (starting from X0) that are
 * configured by the host when a REC is created.
 */
#define REC_CREATE_NR_GPRS		8

#define REC_PARAMS_FLAG_RUNNABLE	BIT_ULL(0)

#define REC_PARAMS_AUX_GRANULES		16

struct rec_params {
	union { /* 0x0 */
		u64 flags;
		u8 padding0[0x100];
	};
	union { /* 0x100 */
		u64 mpidr;
		u8 padding1[0x100];
	};
	union { /* 0x200 */
		u64 pc;
		u8 padding2[0x100];
	};
	union { /* 0x300 */
		u64 gprs[REC_CREATE_NR_GPRS];
		u8 padding3[0x500];
	};
	union { /* 0x800 */
		struct {
			u64 num_rec_aux;
			u64 aux[REC_PARAMS_AUX_GRANULES];
		};
		u8 padding4[0x800];
	};
};

#define REC_ENTER_FLAG_EMULATED_MMIO	BIT(0)
#define REC_ENTER_FLAG_INJECT_SEA	BIT(1)
#define REC_ENTER_FLAG_TRAP_WFI		BIT(2)
#define REC_ENTER_FLAG_TRAP_WFE		BIT(3)
#define REC_ENTER_FLAG_RIPAS_RESPONSE	BIT(4)
#define REC_ENTER_FLAG_DEV_MEM_RESPONSE	BIT(6)

#define REC_RUN_GPRS			31
#define REC_MAX_GIC_NUM_LRS		16

#define RMI_PERMITTED_GICV3_HCR_BITS	(ICH_HCR_EL2_UIE |		\
					 ICH_HCR_EL2_LRENPIE |		\
					 ICH_HCR_EL2_NPIE |		\
					 ICH_HCR_EL2_VGrp0EIE |		\
					 ICH_HCR_EL2_VGrp0DIE |		\
					 ICH_HCR_EL2_VGrp1EIE |		\
					 ICH_HCR_EL2_VGrp1DIE |		\
					 ICH_HCR_EL2_TDIR)

struct rec_enter {
	union { /* 0x000 */
		u64 flags;
		u8 padding0[0x200];
	};
	union { /* 0x200 */
		u64 gprs[REC_RUN_GPRS];
		u8 padding1[0x100];
	};
	union { /* 0x300 */
		struct {
			u64 gicv3_hcr;
			u64 gicv3_lrs[REC_MAX_GIC_NUM_LRS];
		};
		u8 padding2[0x100];
	};
	u8 padding3[0x400];
};

#define RMI_EXIT_SYNC			0x00
#define RMI_EXIT_IRQ			0x01
#define RMI_EXIT_FIQ			0x02
#define RMI_EXIT_PSCI			0x03
#define RMI_EXIT_RIPAS_CHANGE		0x04
#define RMI_EXIT_HOST_CALL		0x05
#define RMI_EXIT_SERROR			0x06
#define RMI_EXIT_VDEV_REQUEST		0x08
#define RMI_EXIT_DEV_COMM		0x09
#define RMI_EXIT_DEV_MEM_MAP		0x0a

#define RMI_VDEV_ACTION_GET_INTERFACE_REPORT	0
#define RMI_VDEV_ACTION_GET_MEASUREMENTS	1
#define RMI_VDEV_ACTION_LOCK			2
#define RMI_VDEV_ACTION_START			3
#define RMI_VDEV_ACTION_STOP			4

struct rec_exit {
	union { /* 0x000 */
		struct {
			u8 exit_reason;
		};
		u8 padding0[0x100];
	};
	union { /* 0x100 */
		struct {
			u64 esr;
			u64 far;
			u64 hpfar;
		};
		u8 padding1[0x100];
	};
	union { /* 0x200 */
		u64 gprs[REC_RUN_GPRS];
		u8 padding2[0x100];
	};
	union { /* 0x300 */
		struct {
			u64 gicv3_hcr;
			u64 gicv3_lrs[REC_MAX_GIC_NUM_LRS];
			u64 gicv3_misr;
			u64 gicv3_vmcr;
		};
		u8 padding3[0x100];
	};
	union { /* 0x400 */
		struct {
			u64 cntp_ctl;
			u64 cntp_cval;
			u64 cntv_ctl;
			u64 cntv_cval;
		};
		u8 padding4[0x100];
	};
	union { /* 0x500 */
		struct {
			u64 ripas_base;
			u64 ripas_top;
			u64 ripas_value;
			u64 ripas_dev_pa;
			u64 s2ap_base;
			u64 s2ap_top;
			u64 vdev_id;
		};
		u8 padding5[0x100];
	};
	union { /* 0x600 */
		struct {
			u16 imm;
			u8 padding9[6];
			u64 plane;
			u64 vdev;
			u8 vdev_action;
			u8 padding10[7];
			u64 dev_mem_base;
			u64 dev_mem_top;
			u64 dev_mem_pa;
		};
		u8 padding6[0x100];
	};
	union { /* 0x700 */
		struct {
			u8 pmu_ovf_status;
		};
		u8 padding7[0x100];
	};
};

struct rec_run {
	struct rec_enter enter;
	struct rec_exit exit;
};

#define MAX_PDEV_AUX_GRANULES		32
#define MAX_PDEV_IOCOH_ADDR		16
#define MAX_PDEV_FCOH_ADDR		4


#define RMI_PDEV_NEW		0	/* Initially when device is created */
#define RMI_PDEV_NEEDS_KEY	1	/* The device requires public key to proceed */
#define RMI_PDEV_HAS_KEY	2	/* The device has the public key */
#define RMI_PDEV_READY		3
#define RMI_PDEV_COMMUNICATING	4
#define RMI_PDEV_STOPPING	5
#define RMI_PDEV_STOPPED	6
#define RMI_PDEV_ERROR		7	/* The device has encountered an
					 * unrecoverable error and needs to
					 * be destroyed
					 */

#define MAX_VDEV_AUX_GRANULES	32
#define RMI_VDEV_READY		0
#define RMI_VDEV_COMMUNICATING	1
#define RMI_VDEV_STOPPING	2
#define RMI_VDEV_STOPPED	3
#define RMI_VDEV_ERROR		4

struct pdev_addr_range {
	unsigned long addr_start;
	unsigned long addr_end;
};

#define RMI_PDEV_PARAMS_USE_SPDM	(1 << 0)
#define RMI_PDEV_PARAMS_USE_IDE		(1 << 1)
#define RMI_PDEV_PARAMS_USE_COH		(1 << 2)
#define RMI_PDEV_PARAMS_USE_P2P		(1 << 3)

#define RMI_PDEV_PARAMS_DISABLE_SEL_IDE		(1UL << 62)
#define RMI_PDEV_PARAMS_DISABLE_LINK_IDE	(1UL << 63)

#define RMI_HASH_SHA_256		0
#define RMI_HASH_SHA_512		1

/*
 * The Device attribute parameters are shared by the Host via
 * RMI_PDEV_CREATE::params_ptr. The values can be observed or modified
 * either by the Host or by the Realm.
 */
struct rmi_pdev_params {
	union {
		struct {
			unsigned long flags;
			unsigned long pdev_id;
			unsigned long segment_id;
			unsigned long ecam_addr;
			unsigned long root_id;
			unsigned long cert_id;
			unsigned long rid_base;
			unsigned long rid_top;
			unsigned char hash_algo;
			unsigned long num_aux;
			unsigned long ide_sid;
			unsigned long ncoh_num_addr_range;
			unsigned long coh_num_addr_range;
			};
		u8 padding1[0x100];
	};

	union { /* 0x100 */
		struct {
			unsigned long aux[MAX_PDEV_AUX_GRANULES];
		};
		u8 padding2[0x100];
	};

	union { /* 0x200 */
		struct {
			struct pdev_addr_range ncoh_addr_range[MAX_PDEV_IOCOH_ADDR];
		};
		u8 padding3[0x100];
	};

	union { /* 0x300 */
		struct {
			struct pdev_addr_range coh_addr_range[MAX_PDEV_FCOH_ADDR];
		};
		u8 padding4[0x700];
	};

};

#define RMI_VDEV_PARAMS_USE_VSMMU	(1 << 0)

struct rmi_vdev_params {
	union {
		struct {
			u64 flags;
			u64 vdev_id;
			u64 tdi_id;
			u64 num_aux;
			u64 vsmmu_addr;
			u64 vsid;
		};
		u8 padding1[0x100];
	};
	union {	/* 0x100 */
		struct {
			unsigned long aux[MAX_VDEV_AUX_GRANULES];
		};
		u8 padding2[0x900];
	};
};

#define RMI_DEV_COMM_ENTER_SUCCESS	0
#define RMI_DEV_COMM_ENTER_RESPONSE	1
#define RMI_DEV_COMM_ENTER_ERROR	2

struct rmi_dev_comm_enter {
	unsigned long status;
	unsigned long req_buff;
	unsigned long resp_buff;
	unsigned long resp_size;
};

#define RMI_DEV_COMM_EXIT_CACHE_REQ	(1 << 0)
#define RMI_DEV_COMM_EXIT_CACHE_RSP	(1 << 1)
#define RMI_DEV_COMM_EXIT_SEND		(1 << 2)
#define RMI_DEV_COMM_EXIT_WAIT		(1 << 3)
#define RMI_DEV_COMM_EXIT_MULTI		(1 << 4)

#define RMI_DEV_COMM_PROTOCOL_SPDM		0
#define RMI_DEV_COMM_PROTOCOL_SECURE_SPDM	1

#define RMI_DEV_COMM_OBJECT_ID_VCA		0
#define RMI_DEV_COMM_OBJECT_ID_CERTIFICATE	1
#define RMI_DEV_COMM_OBJECT_ID_MEASUREMENTS	2
#define RMI_DEV_COMM_OBJECT_ID_INTERFACE_REPORT	3

struct rmi_dev_comm_exit {
	unsigned long flags;
	unsigned long cache_req_offset;
	unsigned long cache_req_len;
	unsigned long cache_rsp_offset;
	unsigned long cache_rsp_len;
	u8 cache_obj_id;
	u8 reserved0[7];
	u8 protocol;
	u8 reserved1[7];
	unsigned long req_len;
	unsigned long timeout;
};

struct rmi_dev_comm_data {
	union { /* 0x0 */
		struct rmi_dev_comm_enter enter;
		u8 padding_1[0x800];
	};
	union { /* 0x800 */
		struct rmi_dev_comm_exit exit;
		u8 padding_2[0x800];
	};
};

struct rmi_pubkey {
	unsigned char public_key[1024];
	unsigned char metadata[1024];
	unsigned long public_key_len;
	unsigned long metadata_len;
	unsigned char rmi_signature_algorithm;
	unsigned char reserved[2024];
};

#define RMI_SIG_RSASSA_3072	0
#define RMI_SIG_ECDSA_P256	1
#define RMI_SIG_ECDSA_P384	2

#define RMI_IDE_KEY_REFRESH	0

#endif /* __ASM_RMI_SMC_H */
