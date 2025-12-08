// SPDX-License-Identifier: GPL-2.0-only
/*
 * SPDX-FileCopyrightText: Copyright (c) 2025, NVIDIA CORPORATION & AFFILIATES. All rights reserved
 */

#include <linux/module.h>
#include <linux/sizes.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/mutex.h>

#include <linux/mem_encrypt.h>

#include <crypto/hash.h>

#include <asm/rhi.h>
#include <asm/rsi_cmds.h>

#include <linux/rmeda_guest.h>

#include "rmeda_common.h"

#define PCI_TDISP_MESSAGE_VERSION_10	0x10

struct rmeda_guest {
	unsigned long vdev_id;
	unsigned long inst_id;

	struct pci_dev *dev;

	void *interface_report;
	size_t interface_report_size;
	void *measurements;
	size_t measurements_size;
	void *certificate_chain;
	size_t certificate_chain_size;
	struct rsi_device_info *device_info;

	bool report_offset_in_pages;

	struct list_head mappings_list;
	struct mutex mappings_mutex;
};

struct rmeda_guest_mapping {
	struct rmeda_guest *rmeda_guest;
	struct list_head list;

	resource_size_t start;
	size_t size;
};

static int control_ide(struct pci_dev *dev,
		       unsigned long vdev_id,
		       bool state)
{
	struct rsi_host_call *rsi_host_call_params;
	unsigned long a0;
	int ret = 0;

	rsi_host_call_params = (void *)__get_free_page(GFP_KERNEL);
	if (!rsi_host_call_params) {
		pci_err(dev, "failed to allocate host call buffer\n");
		ret = -ENOMEM;
		goto err_out;
	}

	rsi_host_call_params->imm = 0;
	rsi_host_call_params->gprs[0] = RHI_DA_CONTROL_IDE;
	rsi_host_call_params->gprs[1] = vdev_id;
	rsi_host_call_params->gprs[2] = state;

	a0 = rsi_host_call(virt_to_phys(rsi_host_call_params));
	if (a0 != 0) {
		pci_err(dev, "failed to issue host call (%lu)\n", a0);
		ret = -EIO;
		goto err_release_rsi_host_call;
	}

	if (rsi_host_call_params->gprs[0] != 0) {
		pci_err(dev, "host call failed (%llu)\n", rsi_host_call_params->gprs[0]);
		ret = -EIO;
		goto err_release_rsi_host_call;
	}

	/* Not needed anymore */
	free_page((unsigned long)rsi_host_call_params);

	return 0;

err_release_rsi_host_call:
	free_page((unsigned long)rsi_host_call_params);
err_out:
	return ret;
}

static void *get_report(struct pci_dev *dev,
			unsigned long vdev_id,
			unsigned long report_id,
			size_t *size)
{
	struct rsi_host_call *rsi_host_call_params;
	void *report_shared, *report;
	unsigned long a0;
	int ret;

	rsi_host_call_params = (void *)__get_free_page(GFP_KERNEL);
	if (!rsi_host_call_params) {
		pci_err(dev, "failed to allocate host call buffer\n");
		ret = -ENOMEM;
		goto err_out;
	}

	report = (void *)__get_free_page(GFP_KERNEL);
	if (!report) {
		pci_err(dev, "failed to allocate interface report buffer\n");
		ret = -ENOMEM;
		goto err_release_rsi_host_call;
	}

	report_shared = (void *)__get_free_page(GFP_KERNEL);
	if (!report_shared) {
		pci_err(dev, "failed to allocate interface report buffer\n");
		ret = -ENOMEM;
		goto err_release_report_buffer;
	}

	ret = set_memory_decrypted((unsigned long)report_shared, 1);
	if (ret) {
		pci_err(dev, "failed to set memory decrypted (%d)\n", ret);
		goto err_release_report_shared_buffer;
	}

	rsi_host_call_params->imm = 0;
	rsi_host_call_params->gprs[0] = RHI_DA_OBJECT_READ;
	rsi_host_call_params->gprs[1] = vdev_id;
	rsi_host_call_params->gprs[2] = report_id;
	rsi_host_call_params->gprs[3] = 0;
	rsi_host_call_params->gprs[4] = 0x1000;
	rsi_host_call_params->gprs[5] = virt_to_phys(report_shared);

	a0 = rsi_host_call(virt_to_phys(rsi_host_call_params));
	if (a0 != 0) {
		pci_err(dev, "failed to issue host call (%lu)\n", a0);
		goto err_decrypt_memory;
	}

	if (rsi_host_call_params->gprs[0] != 0) {
		pci_err(dev, "host call failed (%llu)\n", rsi_host_call_params->gprs[0]);
		goto err_decrypt_memory;
	}

	/* Copy data to encyrpted memory before setting the page encrypted. */
	memcpy(report, report_shared, rsi_host_call_params->gprs[1]);
	*size = rsi_host_call_params->gprs[1];

	/* Revoke host access to page. This also wiped the memory content. */
	ret = set_memory_encrypted((unsigned long)report_shared, 1);
	if (ret) {
		pci_warn(dev, "failed to set memory encrypted (%d)\n", ret);
		/* NOTE: Leak the decrypted page instead of releasing it */
		goto err_release_report_buffer;
	}

	/* Not needed anymore */
	free_page((unsigned long)rsi_host_call_params);
	free_page((unsigned long)report_shared);
	report_shared = NULL;

	return report;

err_decrypt_memory:
	set_memory_encrypted((unsigned long)report_shared, 1);
err_release_report_shared_buffer:
	free_page((unsigned long)report_shared);
err_release_report_buffer:
	free_page((unsigned long)report);
err_release_rsi_host_call:
	free_page((unsigned long)rsi_host_call_params);
err_out:
	return NULL;
}

void rmeda_guest_stop_tdisp(struct rmeda_guest *priv)
{
	struct rmeda_guest_mapping *mapping, *next_mapping;
	struct pci_dev *dev;
	unsigned long a0;
	int ret = 0;

	if (!priv)
		return;

	dev = priv->dev;

	/* Remove mappings established earlier */
	list_for_each_entry_safe(mapping,
				 next_mapping,
				 &priv->mappings_list,
				 list)
		rmeda_guest_release_mapping(mapping);

	ret = control_ide(dev, priv->vdev_id, false);
	if (ret)
		pci_info(dev, "Lazy disablement of EP IDE skipped.\n");

	a0 = rsi_rdev_stop(priv->vdev_id, priv->inst_id);
	if (a0 != RSI_SUCCESS) {
		pci_err(dev, "failed to stop the device (%lu)\n", a0);
		return;
	}

	do {
		a0 = rsi_rdev_continue(priv->vdev_id, priv->inst_id);
	} while (a0 == RSI_INCOMPLETE);
	if (a0 != RSI_SUCCESS)
		pci_err(dev, "failed to communicate with the device (%lu)\n", a0);

	dev->dev.tdi_enabled = false;

	if (priv->measurements)
		free_page((unsigned long)priv->measurements);
	if (priv->certificate_chain)
		free_page((unsigned long)priv->certificate_chain);
	if (priv->interface_report)
		free_page((unsigned long)priv->interface_report);
	if (priv->device_info)
		free_page((unsigned long)priv->device_info);

	kfree(priv);
}
EXPORT_SYMBOL_GPL(rmeda_guest_stop_tdisp);

struct pci_tdisp_device_interface_report {
	u16 interface_info;
	u16 reserved;
	u16 msi_x_message_control;
	u16 lnr_control;
	u32 tph_control;
	u32 mmio_range_count;
	/*
	 * struct pci_tdisp_mmio_range mmio_range[mmio_range_count];
	 * uint32_t device_specific_info_len;
	 * uint8_t device_specific_info[device_specific_info_len];
	 */
};

struct pci_tdisp_mmio_range {
	u64 first_page;
	u32 number_of_pages;
	u16 range_attributes;
	u16 range_id;
};

static void dump_interface_report(struct pci_tdisp_device_interface_report
				  *interface_report)
{
	struct pci_tdisp_mmio_range *mmio_range;
	u32 *device_specific_info_len;
	u8 *device_specific_info;
	unsigned int i;

	pr_info("interface_report:\n");
	pr_info("  interface_info  - 0x%04x\n", interface_report->interface_info);
	pr_info("  msi_x_message_control - 0x%04x\n", interface_report->msi_x_message_control);
	pr_info("  lnr_control	- 0x%04x\n", interface_report->lnr_control);
	pr_info("  tph_control	- 0x%08x\n", interface_report->tph_control);
	pr_info("  mmio_range_count	  - 0x%08x\n", interface_report->mmio_range_count);

	mmio_range = (struct pci_tdisp_mmio_range *)(interface_report + 1);
	for (i = 0; i < interface_report->mmio_range_count; i++) {
		pr_info("  mmio_range(%u):\n", i);
		pr_info("  first_page      - 0x%016llx\n", mmio_range[i].first_page);
		pr_info("  number_of_pages  - 0x%08x\n", mmio_range[i].number_of_pages);
		pr_info("  range_attributes	- 0x%04x\n", mmio_range[i].range_attributes);
		pr_info("  range_id	- 0x%04x\n", mmio_range[i].range_id);
	}

	device_specific_info_len = (u32 *)&mmio_range[i];
	pr_info("  device_info_len    - 0x%08x\n", *device_specific_info_len);
	device_specific_info = (u8 *)(device_specific_info_len + 1);
	pr_info("  device_info		- ");
	for (i = 0; i < *device_specific_info_len && i < 10; i++)
		pr_info("%02x ", device_specific_info[i]);
}

struct sdesc {
	struct shash_desc shash;
	char ctx[];
};

static int verify_digests(struct pci_dev *dev, struct rmeda_guest *priv)
{
	struct {
		uint8_t *report;
		size_t size;
		uint8_t *digest;
	} reports[] = {
		{
			priv->measurements,
			priv->measurements_size,
			priv->device_info->meas_digest
		},
		{
			priv->interface_report,
			priv->interface_report_size,
			priv->device_info->report_digest
		},
		{
			priv->certificate_chain,
			priv->certificate_chain_size,
			priv->device_info->cert_digest
		}
	};

	struct sdesc *sdesc;
	struct crypto_shash *alg;
	char *hash_alg_name;
	int hash_algo;
	u8 digest[64];
	size_t digest_size;
	int sdesc_size;
	int ret;
	int i;

	hash_algo = priv->device_info->hash_algo;
	if (hash_algo == RSI_HASH_SHA_256) {
		hash_alg_name = "sha256";
		digest_size = 32;
	} else if (hash_algo == RSI_HASH_SHA_512) {
		hash_alg_name = "sha512";
		digest_size = 64;
	} else {
		pci_err(dev, "unknown realm hash algorithm!\n");
		ret = -ENXIO;
		goto err_out;
	}

	alg = crypto_alloc_shash(hash_alg_name, 0, 0);
	if (IS_ERR(alg)) {
		pci_err(dev, "cannot allocate %s\n", hash_alg_name);
		return PTR_ERR(alg);
	}

	sdesc_size = sizeof(struct shash_desc) + crypto_shash_descsize(alg);
	sdesc = kmalloc(sdesc_size, GFP_KERNEL);
	if (!sdesc) {
		pci_err(dev, "cannot allocate sdesc\n");
		goto err_free_shash;
	}
	sdesc->shash.tfm = alg;

	for (i = 0; i < ARRAY_SIZE(reports); i++) {
		/*
		 * Allow reports to be missing - but warn the user if that
		 * happens.
		 */
		if (reports[i].report == NULL || reports[i].size == 0) {
			pci_warn(dev, "report #%d missing. cannot verify digest\n",
				 i);
			continue;
		}

		ret = crypto_shash_digest(&sdesc->shash, reports[i].report,
					  reports[i].size, digest);
		if (ret) {
			pci_err(dev, "failed to compute digest, %d\n", ret);
			goto err_free_sdesc;
		}

		if (memcmp(reports[i].digest, digest, digest_size)) {
			pci_err(dev, "invalid digest at #%d\n", i);
			goto err_free_sdesc;
		}
	}

	kfree(sdesc);
	crypto_free_shash(alg);

	return 0;

err_free_sdesc:
	kfree(sdesc);
err_free_shash:
	crypto_free_shash(alg);
err_out:
	return ret;
}

struct rmeda_guest *rmeda_guest_start_tdisp(struct pci_dev *dev)
{
	struct pci_tdisp_device_interface_report *interface_report;
	struct pci_tdisp_mmio_range *mmio_range;
	struct rsi_device_measurements_params *device_measurements_params;
	struct rmeda_guest *priv;
	unsigned long a0, a1;
	int ret;

	if (!dev)
		return NULL;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return NULL;

	priv->dev = dev;
	priv->vdev_id =
		(unsigned long)pci_dev_id(dev) << 8 |
		(unsigned long)pci_domain_nr(dev->bus) << 32;
	priv->report_offset_in_pages = true;
	INIT_LIST_HEAD(&priv->mappings_list);
	mutex_init(&priv->mappings_mutex);

	/* Workaround to a known defect in ARM FVP */
	if (dev->vendor == 0x13b5)
		priv->report_offset_in_pages = false;

	a0 = rsi_rdev_get_instance_id(priv->vdev_id, &priv->inst_id);
	if (a0 != RSI_SUCCESS) {
		pci_err(dev, "failed to get device instance (%lu)\n", a0);
		goto err_free_buffers;
	}

	a0 = rsi_rdev_lock(priv->vdev_id, priv->inst_id);
	if (a0 != RSI_SUCCESS) {
		pci_err(dev, "failed to lock the device (%lu)\n", a0);
		goto err_free_buffers;
	}

	do {
		a0 = rsi_rdev_continue(priv->vdev_id, priv->inst_id);
	} while (a0 == RSI_INCOMPLETE);
	if (a0 != RSI_SUCCESS) {
		pci_err(dev, "failed to communicate with the device (%lu)\n", a0);
		goto err_free_buffers;
	}

	a0 = rsi_rdev_get_interface_report(priv->vdev_id, priv->inst_id,
					   PCI_TDISP_MESSAGE_VERSION_10,
					   &a1);
	if (a0 != RSI_SUCCESS) {
		pci_err(dev, "failed to get interface report (%lu)\n", a0);
		goto err_unlock;
	}

	if (a1 != PCI_TDISP_MESSAGE_VERSION_10) {
		pci_err(dev, "unknown TDISP version (%lu)\n", a1);
		goto err_unlock;
	}

	do {
		a0 = rsi_rdev_continue(priv->vdev_id, priv->inst_id);
	} while (a0 == RSI_INCOMPLETE);
	if (a0 != RSI_SUCCESS) {
		pci_err(dev, "failed to communicate with the device (%lu)\n", a0);
		goto err_unlock;
	}

	device_measurements_params = (void *)__get_free_page(GFP_KERNEL);
	if (!device_measurements_params) {
		pci_err(dev, "failed to allocate measurement parameters\n");
		goto err_unlock;
	}

	device_measurements_params->flags = RSI_RDEV_MEAS_FLAGS_ALL |
					    RSI_RDEV_MEAS_FLAGS_RAW |
					    RSI_RDEV_MEAS_FLAGS_SIGNED;
	memset(device_measurements_params->nonce, 0,
	       sizeof(device_measurements_params->nonce));
	memset(device_measurements_params->meas_ids, 0,
	       sizeof(device_measurements_params->meas_ids));

	a0 = rsi_rdev_get_measurements(priv->vdev_id, priv->inst_id,
				       virt_to_phys(device_measurements_params));
	free_page((unsigned long)device_measurements_params);

	if (a0 != RSI_SUCCESS) {
		pci_err(dev, "failed to get measurements (%lu)\n", a0);
		goto err_unlock;
	}

	do {
		a0 = rsi_rdev_continue(priv->vdev_id, priv->inst_id);
	} while (a0 == RSI_INCOMPLETE);
	if (a0 != RSI_SUCCESS) {
		pci_err(dev, "failed to communicate with the device (%lu)\n", a0);
		goto err_unlock;
	}

	/*
	 * Read interface report. The report is mandatory for
	 * RMI_RDEV_VALIDATE_MAPPING.
	 */
	priv->interface_report = get_report(dev, priv->vdev_id, 2,
					    &priv->interface_report_size);
	if (priv->interface_report == NULL) {
		pci_err(dev, "failed to get the interface report\n");
		goto err_unlock;
	}

	/*
	 * Measurements may not be available if the device is platform
	 * attested. Just warn the user.
	 */
	priv->measurements = get_report(dev, priv->vdev_id, 1,
					&priv->measurements_size);
	if (priv->measurements == NULL)
		pci_err(dev, "failed to get the measurements\n\n");

	/*
	 * Certificate chain is unavailable when the device is platform
	 * attested. Warn the user.
	 */
	priv->certificate_chain = get_report(dev, priv->vdev_id, 0,
					     &priv->certificate_chain_size);
	if (priv->certificate_chain == NULL)
		pci_err(dev, "failed to get the certificates report\n");

	/* Read the device information from RMM */
	priv->device_info = (void *)__get_free_page(GFP_KERNEL);
	if (priv->device_info == NULL) {
		pci_err(dev, "failed to allocate a page for the device_info\n");
		goto err_unlock;
	}
	memset(priv->device_info, 0, PAGE_SIZE);
	a0 = rsi_rdev_get_info(priv->vdev_id,
			       priv->inst_id,
			       virt_to_phys(priv->device_info));
	if (a0 != RSI_SUCCESS) {
		pci_err(dev, "failed to get device device_info (%lu)\n", a0);
		goto err_unlock;
	}

	/*
	 * Verify that the device_info of the provided reports match with the
	 * device_info from RMM
	 */

	ret = verify_digests(dev, priv);
	if (ret) {
		pci_err(dev, "device digest validation failed (%d)\n", ret);
		goto err_unlock;
	}

	/* Digests match. The data can be used */

	interface_report = (struct pci_tdisp_device_interface_report *)
		priv->interface_report;
	mmio_range = (struct pci_tdisp_mmio_range *)(interface_report + 1);
	dump_interface_report(interface_report);

	if (interface_report->mmio_range_count == 0) {
		pci_err(dev, "interface report does not include ranges\n");
		goto err_unlock;
	}

	a0 = rsi_rdev_start(priv->vdev_id, priv->inst_id);
	if (a0 != RSI_SUCCESS) {
		pci_err(dev, "failed to start the device (%lu)\n", a0);
		goto err_unlock;
	}

	do {
		a0 = rsi_rdev_continue(priv->vdev_id, priv->inst_id);
	} while (a0 == RSI_INCOMPLETE);
	if (a0 != RSI_SUCCESS) {
		pci_err(dev, "failed to communicate with the device (%lu)\n", a0);
		goto err_unlock;
	}

	ret = control_ide(dev, priv->vdev_id, true);
	if (ret)
		pci_info(dev, "Lazy enablement of EP IDE skipped.\n");

	dev->dev.tdi_enabled = true;

	pci_info(dev, "TDISP enabled\n");

	return priv;

err_unlock:
	rmeda_guest_stop_tdisp(priv);
	return NULL;

err_free_buffers:
	if (priv->measurements)
		free_page((unsigned long)priv->measurements);
	if (priv->certificate_chain)
		free_page((unsigned long)priv->certificate_chain);
	if (priv->interface_report)
		free_page((unsigned long)priv->interface_report);
	if (priv->device_info)
		free_page((unsigned long)priv->device_info);

	kfree(priv);
	return NULL;
}
EXPORT_SYMBOL_GPL(rmeda_guest_start_tdisp);

struct rmeda_guest_mapping *rmeda_guest_validate_mapping(struct rmeda_guest *priv,
							 resource_size_t start,
							 size_t size,
							 bool coherent)
{
	struct pci_tdisp_device_interface_report *interface_report;
	struct pci_tdisp_mmio_range *mmio_range;
	unsigned long flags = coherent ? RSI_DEV_MEM_COHERENT : 0;
	struct rmeda_guest_mapping *mapping;
	unsigned int resource_id;
	unsigned long start_phys;
	unsigned long range_size;
	size_t i, report_id;
	struct pci_dev *dev;
	unsigned long a0;
	bool fake_report;

	if (!priv)
		return NULL;

	interface_report = priv->interface_report;
	dev = priv->dev;

	/* Validations cannot be performed without valid ranges */
	if (interface_report->mmio_range_count == 0)
		return NULL;

	/* Default to the non-PCI range. */
	resource_id = 0xffff;

	/*
	 * Determine if this is a PCI range, and update resource_id
	 * if necessary
	 */
	for (i = 0; i < PCI_NUM_RESOURCES; i++) {
		/* Does the range start from this region? */
		if ((start < pci_resource_start(dev, i)) ||
		    (start > pci_resource_end(dev, i)))
			continue;

		/* Does the range end in this region? */
		if (((start + size - 1) > pci_resource_end(dev, i)))
			continue;

		/* Found the region from the PCI ranges */
		resource_id = i;

		break;
	}

	/*
	 * Determine if the report is generated by RMM. The first
	 * range is 0xff00 if RMM has generated the report.
	 */
	mmio_range = (struct pci_tdisp_mmio_range *)(interface_report + 1);
	if (mmio_range[0].range_id == 0xff00)
		fake_report = true;
	else
		fake_report = false;

	/*
	 * Use heuristics to support different configurations:
	 *
	 * - Fake report, range is not in the PCI resources => use the last
	 *   MMIO range since RMM places coherent ranges after non-coherent
	 *   ranges. This configuration applies if the device is mapped
	 *   as a CXL device to the realm.
	 *
	 * - Fake report, range is in the PCI resources => assume 1:1 mapping.
	 *   This applies when the device is mapped to the realm as a PCIe
	 *   device, or validation targets a BAR.
	 *
	 * - Real report, range is not in the PCI resources => find the MMIO
	 *   range with ID 0xffff. This applies when the device is mapped as a
	 *   CXL device to the realm.
	 *
	 * - Real report, range is in the PCI resources => find the
	 *   corresponding MMIO range. This applies when the device is mapped
	 *   as a PCIe device to the realm, or validation targets a PCIe range
	 *   of a CXL device.
	 *
	 * - Real report, range is in the PCI resources, mapping of a coherent
	 *   range is requested, the report defines a range as a coherent
	 *   range => use the coherent range fom the report. This applies if
	 *   the device is mapped as a PCIe device to the realm, but the
	 *   interface report contains a specific coherent range.
	 */
	if (fake_report) {
		if (resource_id == 0xffff) {
			/*
			 * This is a CXL range but a fake report is used.
			 *
			 * The coherent range reside after non-coherent ranges.
			 */
			report_id = interface_report->mmio_range_count - 1;
		} else {
			/*
			 * This is a PCI range and a fake report is used.
			 *
			 * BAR mapping is sparse (e.g., BAR1 may not be mapped) whereas
			 * MMIO ranges are not. Map the BAR ID to the MMIO range ID with
			 * assumption that both increase in-step.
			 */
			for (i = 0, report_id = 0; i < resource_id; i++) {
				if (!pci_resource_start(dev, i))
					continue;
				report_id++;
			}
		}
	} else {
		int coherent_report_id = -1;

		/*
		 * This is a real report. Assume that the PCI ranges map
		 * to resource IDs (as per the TDISP specification).
		 * The CXL range is denoted with the range id 0xffff
		 * (as per the ARM RME specification).
		 */
		for (report_id = 0;
		     report_id < interface_report->mmio_range_count;
		     report_id++) {
			if (mmio_range[report_id].range_id == resource_id)
				break;
			if (mmio_range[report_id].range_id == 0xffff)
				coherent_report_id = report_id;
		}

		/*
		 * Special case: Real report with a valid coherent range,
		 * but the coherent range is also defined in the PCI
		 * ranges.
		 */
		if ((report_id >= interface_report->mmio_range_count) &&
		    resource_id != 0xffff &&
		    coherent && (coherent_report_id >= 0)) {
			pci_warn(dev, "The requested coherent PCIe range missing from the report. "
				      "Using the coherent CXL range.\n");
			report_id = coherent_report_id;
		}
	}

	if (report_id >= interface_report->mmio_range_count)
		return NULL;

	/* Determine the physical base address of the range */
	start_phys = mmio_range[report_id].first_page;
	start_phys *= priv->report_offset_in_pages ? SZ_4K : 1;

	/*
	 * The range base must be aligned with its size. Use this information
	 * to determine the offset within the range
	 */
	range_size = ((u64)mmio_range[report_id].number_of_pages) * SZ_4K;
	start_phys += (start & (range_size - 1));

	mapping = kzalloc(sizeof(*mapping), GFP_KERNEL);
	if (!mapping)
		return NULL;

	a0 = rsi_rdev_validate_mapping(priv->vdev_id, priv->inst_id,
				       start, start + size, start_phys, flags);
	if (a0 != RSI_SUCCESS) {
		kfree(mapping);
		pci_err(dev, "failed to set protection attributes for the address range\n");
		return NULL;
	}

	mapping->rmeda_guest = priv;
	mapping->start = start;
	mapping->size = size;
	INIT_LIST_HEAD(&mapping->list);

	mutex_lock(&priv->mappings_mutex);
	list_add(&mapping->list, &priv->mappings_list);
	mutex_unlock(&priv->mappings_mutex);

	return mapping;
}
EXPORT_SYMBOL_GPL(rmeda_guest_validate_mapping);

void rmeda_guest_release_mapping(struct rmeda_guest_mapping *mapping)
{
	struct rmeda_guest *rmeda_guest;
	unsigned long a0;
	phys_addr_t top;

	if (!mapping)
		return;

	rmeda_guest = mapping->rmeda_guest;

	mutex_lock(&rmeda_guest->mappings_mutex);
	list_del(&mapping->list);
	mutex_unlock(&rmeda_guest->mappings_mutex);

	a0 = rsi_set_addr_range_state(mapping->start,
				      mapping->start + mapping->size,
				      RSI_RIPAS_EMPTY,
				      RSI_CHANGE_DESTROYED,
				      &top);
	if (a0 != RSI_SUCCESS)
		pci_err(rmeda_guest->dev,
			"failed to set ripas (%lu)\n",
			a0);

	kfree(mapping);
}
EXPORT_SYMBOL_GPL(rmeda_guest_release_mapping);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Arto Merilainen <amerilainen@nvidia.com>");
MODULE_DESCRIPTION("Routines to authenticate the device from realm");
