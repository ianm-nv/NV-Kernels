/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2023 ARM Ltd.
 */

#ifndef __ASM_RMI_CMDS_H
#define __ASM_RMI_CMDS_H

#include <linux/arm-smccc.h>

#include <asm/rmi_smc.h>

struct rtt_entry {
	unsigned long walk_level;
	unsigned long desc;
	int state;
	int ripas;
};

#define RMI_MAX_ADDR_LIST 256

struct rmi_sro_state {
	struct arm_smccc_1_2_regs regs;

	unsigned long addr_count;
	unsigned long addr_list[RMI_MAX_ADDR_LIST];
};

#define rmi_init_sro(...) ({						\
	struct rmi_sro_state *sro = kmalloc_obj(*sro);			\
	if (sro)							\
		*sro = (struct rmi_sro_state){.regs = {__VA_ARGS__}};	\
	sro;								\
})

#define rmi_smccc(...) do { \
	arm_smccc_1_1_invoke(__VA_ARGS__); \
} while (RMI_RETURN_STATUS(res.a0) == RMI_BUSY || \
	 RMI_RETURN_STATUS(res.a0) == RMI_BLOCKED)

/**
 * rmi_rmm_config_get() - Get the system configuration
 * @cfg_ptr: PA of a struct rmm_config
 *
 * Gets the system configuration
 *
 * Return: RMI return code
 */
static inline int rmi_rmm_config_get(unsigned long cfg_ptr)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_invoke(SMC_RMI_RMM_CONFIG_GET, cfg_ptr, &res);

	return res.a0;
}

/**
 * rmi_rmm_config_set() - Configure the RMM
 * @cfg_ptr: PA of a struct rmm_config
 *
 * Sets configuration options on the RMM.
 *
 * Return: RMI return code
 */
static inline int rmi_rmm_config_set(unsigned long cfg_ptr)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_invoke(SMC_RMI_RMM_CONFIG_SET, cfg_ptr, &res);

	return res.a0;
}

/**
 * rmi_rmm_activate() - Activate the RMM
 *
 * Return: RMI return code
 */
static inline int rmi_rmm_activate(void)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_invoke(SMC_RMI_RMM_ACTIVATE, &res);

	return res.a0;
}

/**
 * rmi_granule_tracking_get() - Get configuration of a Granule tracking region
 * @addr: PA of the tracking region
 * @out_category: Memory category
 * @out_state: Tracking region state
 *
 * Return: RMI return code
 */
static inline int rmi_granule_tracking_get(unsigned long addr,
					   unsigned long *out_category,
					   unsigned long *out_state)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_invoke(SMC_RMI_GRANULE_TRACKING_GET, addr, &res);

	if (out_category)
		*out_category = res.a1;
	if (out_state)
		*out_state = res.a2;

	return res.a0;
}

/**
 * rmi_granule_tracking_set() - Set configuration of a Granule tracking region
 * @addr: PA of the tracking region
 * @category: Memory category
 * @state: Tracking region state
 *
 * Return: RMI return code
 */
static inline int rmi_granule_tracking_set(unsigned long addr,
					   unsigned long category,
					   unsigned long state)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_invoke(SMC_RMI_GRANULE_TRACKING_SET, addr, category,
			     state, &res);

	if (RMI_RETURN_STATUS(res.a0) == RMI_INCOMPLETE) {
		/* FIXME */
		return WARN_ON(res.a0);
	}

	return res.a0;
}

/**
 * rmi_gpt_l1_create() - Create a Level 1 GPT
 * @addr: Base of physical address region described by the L1GPT
 *
 * Return: RMI return code
 */
static inline int rmi_gpt_l1_create(unsigned long addr)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_invoke(SMC_RMI_GPT_L1_CREATE, addr, &res);

	if (RMI_RETURN_STATUS(res.a0) == RMI_INCOMPLETE) {
		/* FIXME */
		return WARN_ON(res.a0);
	}

	return res.a0;
}

/**
 * rmi_gpt_l1_destroy() - Destroy a Level 1 GPT
 * @addr: Base of physical address region descripted by the L1GPT
 *
 * Return: RMI return code
 */
static inline int rmi_gpt_l1_destroy(unsigned long addr)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_invoke(SMC_RMI_GPT_L1_DESTROY, addr, &res);

	if (RMI_RETURN_STATUS(res.a0) == RMI_INCOMPLETE) {
		/* FIXME */
		return WARN_ON(res.a0);
	}

	return res.a0;
}

/**
 * rmi_rtt_data_map_init() - Create a protected mapping with data contents
 * @rd: PA of the RD
 * @data: PA of the target granule
 * @ipa: IPA at which the granule will be mapped in the guest
 * @src: PA of the source granule
 * @flags: RMI_MEASURE_CONTENT if the contents should be measured
 *
 * Create a mapping from Protected IPA space to conventional memory, copying
 * contents from a Non-secure Granule provided by the caller.
 *
 * Return: RMI return code
 */
static inline int rmi_rtt_data_map_init(unsigned long rd, unsigned long data,
					unsigned long ipa, unsigned long src,
					unsigned long flags)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_invoke(SMC_RMI_RTT_DATA_MAP_INIT, rd, data, ipa, src,
			     flags, &res);

	return res.a0;
}

/**
 * rmi_rtt_data_map() - Create mappings in protected IPA with unknown contents
 * @rd: PA of the RD
 * @base: Base of the target IPA range
 * @top: Top of the target IPA range
 * @flags: Flags
 * @oaddr: Output address set descriptor
 * @out_top: Top address of range which was processed.
 *
 * Return RMI return code
 */
static inline int rmi_rtt_data_map(unsigned long rd,
				   unsigned long base,
				   unsigned long top,
				   unsigned long flags,
				   unsigned long oaddr,
				   unsigned long *out_top)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_invoke(SMC_RMI_RTT_DATA_MAP, rd, base, top, flags, oaddr,
			     &res);

	if (RMI_RETURN_STATUS(res.a0) == RMI_INCOMPLETE) {
		/* FIXME */
		return WARN_ON(res.a0);
	}

	if (out_top)
		*out_top = res.a1;

	return res.a0;
}

/**
 * rmi_rtt_data_unmap() - Remove mappings to conventional memory
 * @rd: PA of the RD for the target Realm
 * @base: Base of the target IPA range
 * @top: Top of the target IPA range
 * @flags: Flags
 * @oaddr: Output address set descriptor
 * @out_top: Returns top IPA of range which has been unmapped
 * @out_range: Output address range
 * @out_count: Number of entries in output address list
 *
 * Removes mappings to convention memory with a target Protected IPA range.
 *
 * Return: RMI return code
 */
static inline int rmi_rtt_data_unmap(unsigned long rd,
				     unsigned long base,
				     unsigned long top,
				     unsigned long flags,
				     unsigned long oaddr,
				     unsigned long *out_top,
				     unsigned long *out_range,
				     unsigned long *out_count)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_invoke(SMC_RMI_RTT_DATA_UNMAP, rd, base, top, flags,
			     oaddr, &res);

	/* FIXME: Handle SRO */

	if (out_top)
		*out_top = res.a1;
	if (out_range)
		*out_range = res.a2;
	if (out_count)
		*out_count = res.a3;

	return res.a0;
}

/**
 * rmi_features() - Read feature register
 * @index: Feature register index
 * @out: Feature register value is written to this pointer
 *
 * Return: RMI return code
 */
static inline int rmi_features(unsigned long index, unsigned long *out)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_invoke(SMC_RMI_FEATURES, index, &res);

	if (out)
		*out = res.a1;
	return res.a0;
}

/**
 * rmi_granule_range_delegate() - Delegate granules
 * @base: PA of the first granule of the range
 * @top: PA of the first granule after the range
 * @out_top: PA of the first granule not delegated
 *
 * Delegate a range of granule for use by the realm world. If the entire range
 * was delegated then @out_top == @top, otherwise the function should be called
 * again with @base == @out_top.
 *
 * Return: RMI return code
 */
static inline int rmi_granule_range_delegate(unsigned long base,
					     unsigned long top,
					     unsigned long *out_top)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_invoke(SMC_RMI_GRANULE_RANGE_DELEGATE, base, top, &res);

	if (RMI_RETURN_STATUS(res.a0) == RMI_INCOMPLETE) {
		/* FIXME - Handle SRO */
		return WARN_ON(res.a0);
	}

	if (out_top)
		*out_top = res.a1;

	return res.a0;
}

/**
 * rmi_granule_range_undelegate() - Undelegate a range of granules
 * @base: Base PA of the target range
 * @top: Top PA of the target range
 * @out_top: Returns the top PA of range whose state is undelegated
 *
 * Undelegate a range of granules to allow use by the normal world. Will fail if
 * the granules are in use.
 *
 * Return: RMI return code
 */
static inline int rmi_granule_range_undelegate(unsigned long base,
					       unsigned long top,
					       unsigned long *out_top)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_invoke(SMC_RMI_GRANULE_RANGE_UNDELEGATE, base, top, &res);

	if (RMI_RETURN_STATUS(res.a0) == RMI_INCOMPLETE) {
		/* FIXME - Handle SRO */
		return WARN_ON(res.a0);
	}

	if (out_top)
		*out_top = res.a1;

	return res.a0;
}

/**
 * rmi_psci_complete() - Complete pending PSCI command
 * @calling_rec: PA of the calling REC
 * @target_rec: PA of the target REC
 * @status: Status of the PSCI request
 *
 * Completes a pending PSCI command which was called with an MPIDR argument, by
 * providing the corresponding REC.
 *
 * Return: RMI return code
 */
static inline int rmi_psci_complete(unsigned long calling_rec,
				    unsigned long target_rec,
				    unsigned long status)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_invoke(SMC_RMI_PSCI_COMPLETE, calling_rec, target_rec,
			     status, &res);

	return res.a0;
}

/**
 * rmi_realm_activate() - Active a realm
 * @rd: PA of the RD
 *
 * Mark a realm as Active signalling that creation is complete and allowing
 * execution of the realm.
 *
 * Return: RMI return code
 */
static inline int rmi_realm_activate(unsigned long rd)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_invoke(SMC_RMI_REALM_ACTIVATE, rd, &res);

	return res.a0;
}

/**
 * rmi_realm_create() - Create a realm
 * @rd: PA of the RD
 * @params: PA of realm parameters
 *
 * Create a new realm using the given parameters.
 *
 * Return: RMI return code
 */
static inline int rmi_realm_create(unsigned long rd, unsigned long params)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_invoke(SMC_RMI_REALM_CREATE, rd, params, &res);

	return res.a0;
}

/**
 * rmi_realm_destroy() - Destroy a realm
 * @rd: PA of the RD
 *
 * Destroys a realm, all objects belonging to the realm must be destroyed first.
 *
 * Return: RMI return code
 */
static inline int rmi_realm_destroy(unsigned long rd)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_invoke(SMC_RMI_REALM_DESTROY, rd, &res);

	return res.a0;
}

/**
 * rmi_rec_create_sro_init() - Init an SRO to create a REC
 * @rd: PA of the RD
 * @rec: PA of the target REC
 * @params: PA of REC parameters
 *
 * Create a REC using the parameters specified in the struct rec_params pointed
 * to by @params.
 *
 * Returns: Allocated SRO object
 */
static inline struct rmi_sro_state *
rmi_rec_create_sro_init(unsigned long rd,
			unsigned long rec,
			unsigned long params)
{
	return rmi_init_sro(SMC_RMI_REC_CREATE, rd, rec, params);
}

/**
 * rmi_rec_destroy_sro_init() - Init an SRO to destroy a REC
 * @rec: PA of the target REC
 *
 * Destroys a REC. The REC must not be running.
 *
 * Return: Allocated SRO object
 */
static inline struct rmi_sro_state *rmi_rec_destroy_sro_init(unsigned long rec)
{
	return rmi_init_sro(SMC_RMI_REC_DESTROY, rec);
}

/**
 * rmi_rec_enter() - Enter a REC
 * @rec: PA of the target REC
 * @run_ptr: PA of RecRun structure
 *
 * Starts (or continues) execution within a REC.
 *
 * Return: RMI return code
 */
static inline int rmi_rec_enter(unsigned long rec, unsigned long run_ptr)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_invoke(SMC_RMI_REC_ENTER, rec, run_ptr, &res);

	return res.a0;
}

/**
 * rmi_rtt_create() - Creates an RTT
 * @rd: PA of the RD
 * @rtt: PA of the target RTT
 * @ipa: Base of the IPA range described by the RTT
 * @level: Depth of the RTT within the tree
 *
 * Creates an RTT (Realm Translation Table) at the specified level for the
 * translation of the specified address within the realm.
 *
 * Return: RMI return code
 */
static inline int rmi_rtt_create(unsigned long rd, unsigned long rtt,
				 unsigned long ipa, long level)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_invoke(SMC_RMI_RTT_CREATE, rd, rtt, ipa, level, &res);

	return res.a0;
}

/**
 * rmi_rtt_destroy() - Destroy an RTT
 * @rd: PA of the RD
 * @ipa: Base of the IPA range described by the RTT
 * @level: Depth of the RTT within the tree
 * @out_rtt: Pointer to write the PA of the RTT which was destroyed
 * @out_top: Pointer to write the top IPA of non-live RTT entries
 *
 * Destroys an RTT. The RTT must be non-live, i.e. none of the entries in the
 * table are in ASSIGNED or TABLE state.
 *
 * Return: RMI return code.
 */
static inline int rmi_rtt_destroy(unsigned long rd,
				  unsigned long ipa,
				  long level,
				  unsigned long *out_rtt,
				  unsigned long *out_top)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_invoke(SMC_RMI_RTT_DESTROY, rd, ipa, level, &res);

	if (out_rtt)
		*out_rtt = res.a1;
	if (out_top)
		*out_top = res.a2;

	return res.a0;
}

/**
 * rmi_rtt_fold() - Fold an RTT
 * @rd: PA of the RD
 * @ipa: Base of the IPA range described by the RTT
 * @level: Depth of the RTT within the tree
 * @out_rtt: Pointer to write the PA of the RTT which was destroyed
 *
 * Folds an RTT. If all entries with the RTT are 'homogeneous' the RTT can be
 * folded into the parent and the RTT destroyed.
 *
 * Return: RMI return code
 */
static inline int rmi_rtt_fold(unsigned long rd, unsigned long ipa,
			       long level, unsigned long *out_rtt)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_invoke(SMC_RMI_RTT_FOLD, rd, ipa, level, &res);

	if (out_rtt)
		*out_rtt = res.a1;

	return res.a0;
}

/**
 * rmi_rtt_init_ripas() - Set RIPAS for new realm
 * @rd: PA of the RD
 * @base: Base of target IPA region
 * @top: Top of target IPA region
 * @out_top: Top IPA of range whose RIPAS was modified
 *
 * Sets the RIPAS of a target IPA range to RAM, for a realm in the NEW state.
 *
 * Return: RMI return code
 */
static inline int rmi_rtt_init_ripas(unsigned long rd, unsigned long base,
				     unsigned long top, unsigned long *out_top)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_invoke(SMC_RMI_RTT_INIT_RIPAS, rd, base, top, &res);

	if (out_top)
		*out_top = res.a1;

	return res.a0;
}

/**
 * rmi_rtt_map_unprotected() - Map NS granules into a realm
 * @rd: PA of the RD
 * @ipa: Base IPA of the mapping
 * @level: Depth within the RTT tree
 * @desc: RTTE descriptor
 *
 * Create a mapping from an Unprotected IPA to a Non-secure PA.
 *
 * Return: RMI return code
 */
static inline int rmi_rtt_map_unprotected(unsigned long rd,
					  unsigned long ipa,
					  long level,
					  unsigned long desc)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_invoke(SMC_RMI_RTT_MAP_UNPROTECTED, rd, ipa, level,
			     desc, &res);

	return res.a0;
}

/**
 * rmi_rtt_read_entry() - Read an RTTE
 * @rd: PA of the RD
 * @ipa: IPA for which to read the RTTE
 * @level: RTT level at which to read the RTTE
 * @rtt: Output structure describing the RTTE
 *
 * Reads a RTTE (Realm Translation Table Entry).
 *
 * Return: RMI return code
 */
static inline int rmi_rtt_read_entry(unsigned long rd, unsigned long ipa,
				     long level, struct rtt_entry *rtt)
{
	struct arm_smccc_1_2_regs regs = {
		SMC_RMI_RTT_READ_ENTRY,
		rd, ipa, level
	};

	arm_smccc_1_2_invoke(&regs, &regs);

	rtt->walk_level = regs.a1;
	rtt->state = regs.a2 & 0xFF;
	rtt->desc = regs.a3;
	rtt->ripas = regs.a4 & 0xFF;

	return regs.a0;
}

/**
 * rmi_rtt_set_ripas() - Set RIPAS for an running realm
 * @rd: PA of the RD
 * @rec: PA of the REC making the request
 * @base: Base of target IPA region
 * @top: Top of target IPA region
 * @out_top: Pointer to write top IPA of range whose RIPAS was modified
 *
 * Completes a request made by the realm to change the RIPAS of a target IPA
 * range.
 *
 * Return: RMI return code
 */
static inline int rmi_rtt_set_ripas(unsigned long rd, unsigned long rec,
				    unsigned long base, unsigned long top,
				    unsigned long *out_top)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_invoke(SMC_RMI_RTT_SET_RIPAS, rd, rec, base, top, &res);

	if (out_top)
		*out_top = res.a1;

	return res.a0;
}

/**
 * rmi_rtt_unmap_unprotected() - Remove a NS mapping
 * @rd: PA of the RD
 * @ipa: Base IPA of the mapping
 * @level: Depth within the RTT tree
 * @out_top: Pointer to write top IPA of non-live RTT entries
 *
 * Removes a mapping at an Unprotected IPA.
 *
 * Return: RMI return code
 */
static inline int rmi_rtt_unmap_unprotected(unsigned long rd,
					    unsigned long ipa,
					    long level,
					    unsigned long *out_top)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_invoke(SMC_RMI_RTT_UNMAP_UNPROTECTED, rd, ipa,
			     level, &res);

	if (out_top)
		*out_top = res.a1;

	return res.a0;
}

#endif /* __ASM_RMI_CMDS_H */
