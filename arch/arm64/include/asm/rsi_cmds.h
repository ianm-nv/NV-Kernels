/* SPDX-License-Identifier: GPL-2.0-only
 * SPDX-FileCopyrightText: Copyright (C) 2023 ARM Ltd.
 * SPDX-FileCopyrightText: Copyright (C) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#ifndef __ASM_RSI_CMDS_H
#define __ASM_RSI_CMDS_H

#include <linux/arm-smccc.h>
#include <linux/string.h>
#include <asm/memory.h>

#include <asm/rsi_smc.h>

#define RSI_GRANULE_SHIFT		12
#define RSI_GRANULE_SIZE		(_AC(1, UL) << RSI_GRANULE_SHIFT)

enum ripas {
	RSI_RIPAS_EMPTY = 0,
	RSI_RIPAS_RAM = 1,
	RSI_RIPAS_DESTROYED = 2,
	RSI_RIPAS_DEV = 3,
};

static inline unsigned long rsi_request_version(unsigned long req,
						unsigned long *out_lower,
						unsigned long *out_higher)
{
	struct arm_smccc_res res;

	arm_smccc_smc(SMC_RSI_ABI_VERSION, req, 0, 0, 0, 0, 0, 0, &res);

	if (out_lower)
		*out_lower = res.a1;
	if (out_higher)
		*out_higher = res.a2;

	return res.a0;
}

static inline unsigned long rsi_get_realm_config(struct realm_config *cfg)
{
	struct arm_smccc_res res;

	arm_smccc_smc(SMC_RSI_REALM_CONFIG, virt_to_phys(cfg),
		      0, 0, 0, 0, 0, 0, &res);
	return res.a0;
}

static inline unsigned long rsi_ipa_state_get(phys_addr_t start,
					      phys_addr_t end,
					      enum ripas *state,
					      phys_addr_t *top)
{
	struct arm_smccc_res res;

	arm_smccc_smc(SMC_RSI_IPA_STATE_GET,
		      start, end, 0, 0, 0, 0, 0,
		      &res);

	if (res.a0 == RSI_SUCCESS) {
		if (top)
			*top = res.a1;
		if (state)
			*state = res.a2;
	}

	return res.a0;
}

static inline long rsi_set_addr_range_state(phys_addr_t start,
					    phys_addr_t end,
					    enum ripas state,
					    unsigned long flags,
					    phys_addr_t *top)
{
	struct arm_smccc_res res;

	arm_smccc_smc(SMC_RSI_IPA_STATE_SET, start, end, state,
		      flags, 0, 0, 0, &res);

	if (top)
		*top = res.a1;

	if (res.a2 != RSI_ACCEPT)
		return -EPERM;

	return res.a0;
}

/**
 * rsi_attestation_token_init - Initialise the operation to retrieve an
 * attestation token.
 *
 * @challenge:	The challenge data to be used in the attestation token
 *		generation.
 * @size:	Size of the challenge data in bytes.
 *
 * Initialises the attestation token generation and returns an upper bound
 * on the attestation token size that can be used to allocate an adequate
 * buffer. The caller is expected to subsequently call
 * rsi_attestation_token_continue() to retrieve the attestation token data on
 * the same CPU.
 *
 * Returns:
 *  On success, returns the upper limit of the attestation report size.
 *  Otherwise, -EINVAL
 */
static inline long
rsi_attestation_token_init(const u8 *challenge, unsigned long size)
{
	struct arm_smccc_1_2_regs regs = { 0 };

	/* The challenge must be at least 32bytes and at most 64bytes */
	if (!challenge || size < 32 || size > 64)
		return -EINVAL;

	regs.a0 = SMC_RSI_ATTESTATION_TOKEN_INIT;
	memcpy(&regs.a1, challenge, size);
	arm_smccc_1_2_smc(&regs, &regs);

	if (regs.a0 == RSI_SUCCESS)
		return regs.a1;

	return -EINVAL;
}

/**
 * rsi_attestation_token_continue - Continue the operation to retrieve an
 * attestation token.
 *
 * @granule: {I}PA of the Granule to which the token will be written.
 * @offset:  Offset within Granule to start of buffer in bytes.
 * @size:    The size of the buffer.
 * @len:     The number of bytes written to the buffer.
 *
 * Retrieves up to a RSI_GRANULE_SIZE worth of token data per call. The caller
 * is expected to call rsi_attestation_token_init() before calling this
 * function to retrieve the attestation token.
 *
 * Return:
 * * %RSI_SUCCESS     - Attestation token retrieved successfully.
 * * %RSI_INCOMPLETE  - Token generation is not complete.
 * * %RSI_ERROR_INPUT - A parameter was not valid.
 * * %RSI_ERROR_STATE - Attestation not in progress.
 */
static inline unsigned long rsi_attestation_token_continue(phys_addr_t granule,
							   unsigned long offset,
							   unsigned long size,
							   unsigned long *len)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_invoke(SMC_RSI_ATTESTATION_TOKEN_CONTINUE,
			     granule, offset, size, 0, &res);

	if (len)
		*len = res.a1;
	return res.a0;
}

static inline unsigned long rsi_rdev_continue(unsigned long dev_id,
					      unsigned long inst_id)
{
	struct arm_smccc_res res;

	arm_smccc_smc(SMC_RSI_RDEV_CONTINUE,
		      dev_id, inst_id, 0, 0, 0, 0, 0, &res);

	return res.a0;
}

static inline unsigned long rsi_rdev_get_info(unsigned long dev_id,
					      unsigned long inst_id,
					      unsigned long addr)
{
	struct arm_smccc_res res;

	arm_smccc_smc(SMC_RSI_RDEV_GET_INFO,
		      dev_id, inst_id, addr, 0, 0, 0, 0, &res);

	return res.a0;
}

static inline unsigned long rsi_rdev_get_instance_id(unsigned long dev_id,
						     unsigned long *inst_id)
{
	struct arm_smccc_res res;

	arm_smccc_smc(SMC_RSI_RDEV_GET_INSTANCE_ID,
		      dev_id, 0, 0, 0, 0, 0, 0, &res);

	*inst_id = res.a1;
	return res.a0;
}

static inline unsigned long rsi_rdev_get_interface_report(unsigned long dev_id,
							  unsigned long inst_id,
							  unsigned long version_max,
							  unsigned long *version)
{
	struct arm_smccc_res res;

	arm_smccc_smc(SMC_RSI_RDEV_GET_INTERFACE_REPORT,
		      dev_id, inst_id, version_max, 0, 0, 0, 0,
		      &res);

	*version = res.a1;
	return res.a0;
}

static inline unsigned long rsi_rdev_get_measurements(unsigned long dev_id,
						      unsigned long inst_id,
						      unsigned long params_addr)
{
	struct arm_smccc_res res;

	arm_smccc_smc(SMC_RSI_RDEV_GET_MEASUREMENTS,
		      dev_id, inst_id, params_addr, 0, 0, 0, 0, &res);

	return res.a0;
}

static inline unsigned long rsi_rdev_get_state(unsigned long dev_id,
					       unsigned long inst_id)
{
	struct arm_smccc_res res;

	arm_smccc_smc(SMC_RSI_RDEV_GET_STATE,
		      dev_id, inst_id, 0, 0, 0, 0, 0, &res);

	return res.a0;
}

static inline unsigned long rsi_rdev_lock(unsigned long dev_id,
					   unsigned long inst_id)
{
	struct arm_smccc_res res;

	arm_smccc_smc(SMC_RSI_RDEV_LOCK,
		      dev_id, inst_id, 0, 0, 0, 0, 0, &res);

	return res.a0;
}

static inline unsigned long rsi_rdev_start(unsigned long dev_id,
					   unsigned long inst_id)
{
	struct arm_smccc_res res;

	arm_smccc_smc(SMC_RSI_RDEV_START,
		      dev_id, inst_id, 0, 0, 0, 0, 0, &res);

	return res.a0;
}

static inline unsigned long rsi_rdev_stop(unsigned long dev_id,
					  unsigned long inst_id)
{
	struct arm_smccc_res res;

	arm_smccc_smc(SMC_RSI_RDEV_STOP,
		      dev_id, inst_id, 0, 0, 0, 0, 0, &res);

	return res.a0;
}

static inline unsigned long rsi_rdev_validate_mapping(unsigned long dev_id,
						      unsigned long inst_id,
						      phys_addr_t start,
						      phys_addr_t end,
						      phys_addr_t dev_pa,
						      unsigned long flags)
{
	struct arm_smccc_res res;

	arm_smccc_smc(SMC_RSI_RDEV_VALIDATE_MAPPING, dev_id, inst_id,
		      start, end, dev_pa, flags, 0, &res);

	return res.a0;
}

static inline unsigned long rsi_host_call(phys_addr_t addr)
{
	struct arm_smccc_res res;

	arm_smccc_smc(SMC_RSI_HOST_CALL, addr, 0, 0, 0, 0, 0, 0, &res);

	return res.a0;
}

#endif
