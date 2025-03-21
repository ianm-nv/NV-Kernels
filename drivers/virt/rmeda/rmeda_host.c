// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 ARM Ltd.
 * SPDX-FileCopyrightText: Copyright (c) 2025, NVIDIA CORPORATION & AFFILIATES. All rights reserved
 */

#include <linux/delay.h>
#include <linux/kvm_host.h>
#include <linux/pci.h>
#include <linux/pci-doe.h>
#include <linux/pci-ide.h>
#include <linux/pci-ecam.h>
#include <linux/highmem.h>

#include <asm/kvm_emulate.h>
#include <asm/kvm_mmu.h>
#include <asm/kvm_rme.h>
#include <asm/ptrace.h>
#include <asm/rhi.h>
#include <asm/rmi_cmds.h>

#include <asm/virt.h>
#include <asm/kvm_emulate.h>

#include <crypto/internal/rsa.h>

#include <keys/x509-parser.h>
#include <keys/asymmetric-type.h>

#include <linux/rmeda_host.h>

#include "rmeda_common.h"

#define PCI_DOE_PROTOCOL_CMA_SPDM		1
#define PCI_DOE_PROTOCOL_SEC_CMA_SPDM		2

struct rmeda_host {
	bool platform_dev;
	bool ide_enabled;
	int sid;

	struct pci_dev *epdev;
	struct pci_dev *rpdev;
	struct pci_doe_mb *doe_cma;
	struct pci_doe_mb *doe_scma;

	void *dev_resp_buff;	/* response buffer */
	size_t dev_resp_max_sz; /* max response buffer size that can be rcvd */
	size_t dev_resp_sz;	/* actual response buffer size */
	size_t dev_resp_recv_sz; /* response buffer received so far */
	uint16_t dev_resp_vendor;
	uint8_t dev_resp_type;

	void *pdev;
	int num_aux;
	void *aux[MAX_PDEV_AUX_GRANULES];
	struct rmi_dev_comm_data *dev_comm_params;
	void *resp_buff;
	void *req_buff;

	struct kvm *kvm;
	bool vdev_active;
	void *vdev;
	u64 vdev_id;

	uint8_t rmi_signature_algorithm;

	struct {
		unsigned char buf[4096];
		size_t size;

		void *public_key;
		size_t public_key_size;

		bool valid;
	} cert_chain;

	struct {
		unsigned char buf[4096];
		size_t size;
		size_t remaining;
	} interface_report;

	struct {
		unsigned char buf[4096];
		size_t size;
		size_t remaining;
	} measurements;
};

static int probe_doe_mb(struct rmeda_host *rmeda_host)
{
	struct pci_doe_mb *doe_cma;
	struct pci_dev *pdev;

	pdev = rmeda_host->epdev;
	doe_cma = pci_find_doe_mailbox(pdev, PCI_VENDOR_ID_PCI_SIG,
					PCI_DOE_PROTOCOL_CMA_SPDM);
	if (!doe_cma) {
		pci_err(pdev, "EP DOE does not support SPDM\n");
		return -ENOENT;
	}
	rmeda_host->doe_cma = doe_cma;

	doe_cma = pci_find_doe_mailbox(pdev, PCI_VENDOR_ID_PCI_SIG,
					PCI_DOE_PROTOCOL_SEC_CMA_SPDM);
	if (!doe_cma) {
		pci_err(pdev, "EP DOE does not support secure SPDM\n");
		return -ENOENT;
	}
	rmeda_host->doe_scma = doe_cma;
	pci_dbg(pdev, "Found DOE mailboxes for SPDM\n");
	return 0;
}

static bool pcie_has_tee_io(struct pci_dev *dev)
{
	u32 cap;

	/* FIXME: FLR_RESET ? */
	if (dev->dev_flags & PCI_DEV_FLAGS_NO_FLR_RESET)
		return false;

	pcie_capability_read_dword(dev, PCI_EXP_DEVCAP, &cap);
	return cap & PCI_EXP_DEVCAP_TEE_IO;
}

static int check_pcie_downstream(struct pci_dev *pdev)
{
	struct pci_ide_info *ide;

	ide = pci_get_ide(pdev);
	if (!ide) {
		pci_warn(pdev, "Dev does not support IDE. Ignoring.\n");
		return 0;
	}

	if (!pci_ide_has_flow_thru(ide)) {
		pci_warn(pdev, "Dev does not support flow-through. Ignoring.\n");
		return 0;
	}

	pci_dbg(pdev, "%s OK\n", __func__);
	return 0;
}

static int check_pcie_upstream(struct pci_dev *pdev)
{
	int ret;

	ret = check_pcie_downstream(pdev);
	if (ret)
		return ret;

	pci_dbg(pdev, "%s OK\n", __func__);
	return ret;
}

static int check_ide_capabilities(struct pci_dev *pdev)
{
	bool ide_km_required =
		(pci_pcie_type(pdev) == PCI_EXP_TYPE_ROOT_PORT) ?
		false : true;
	struct pci_ide_info *ide;

	ide = pci_get_ide(pdev);
	if (!ide) {
		pci_err(pdev, "Device does not support IDE\n");
		return -ENOENT;
	}

	if (!pci_ide_has_sel_ide(ide) ||
	    (ide_km_required && !pci_ide_has_km(ide))) {
		pci_err(pdev, "Device does not support IDE_KM\n");
		return -ENOENT;
	}

	if (!pcie_has_tee_io(pdev))
		pci_err(pdev, "Device does not support TEE I/O, ignoring errors\n");

	pci_dbg(pdev, "%s OK\n", __func__);
	return 0;
}

static int rdev_walk_get_pcie(struct rmeda_host *rmeda_host)
{
	struct pci_dev *pdev = rmeda_host->epdev;
	int ret, type;

	while ((pdev = pci_upstream_bridge(pdev))) {
		if (!pci_is_pcie(pdev)) {
			ret = -EINVAL;
			goto err_pdev_put;
		}
		type = pci_pcie_type(pdev);
		if (type == PCI_EXP_TYPE_DOWNSTREAM) {
			ret = check_pcie_downstream(pdev);
		} else if (type == PCI_EXP_TYPE_UPSTREAM) {
			ret = check_pcie_upstream(pdev);
		} else if (type == PCI_EXP_TYPE_ROOT_PORT) {
			rmeda_host->rpdev = pdev;
		} else {
			pci_err(pdev, "Unknown device type:%d\n", type);
			ret = -EINVAL;
			goto err_pdev_put;
		}
		if (ret)
			goto err_pdev_put;
	}

	return ret;

err_pdev_put:
	return ret;
}

static int pci_res_to_ide_addr(struct pci_dev *pdev,
			       struct ide_addr_range *ide_addr)
{
	struct pci_dev *bridge = pci_upstream_bridge(pdev);
	struct pci_bus_region region;
	struct resource *res;
	int naddr = 0;

	res = &bridge->resource[PCI_BRIDGE_IO_WINDOW];
	if (res->flags & IORESOURCE_IO) {
		pcibios_resource_to_bus(bridge->bus, &region, res);
		ide_addr[naddr].start = region.start;
		ide_addr[naddr].end = region.end;
		naddr++;
	}

	res = &bridge->resource[PCI_BRIDGE_MEM_WINDOW];
	if (res->flags & IORESOURCE_MEM) {
		pcibios_resource_to_bus(bridge->bus, &region, res);
		ide_addr[naddr].start = region.start;
		ide_addr[naddr].end = region.end;
		naddr++;
	}

	res = &bridge->resource[PCI_BRIDGE_PREF_MEM_WINDOW];
	if (res->flags & IORESOURCE_PREFETCH) {
		pcibios_resource_to_bus(bridge->bus, &region, res);
		ide_addr[naddr].start = region.start;
		ide_addr[naddr].end = region.end;
		naddr++;
	}

	return naddr;
}

static int pci_res_to_pdev_addr(struct pdev_addr_range *pdev_addr,
				unsigned int naddr,
				struct resource *res,
				unsigned int nres)
{
	int i, j;

	if (!res)
		return 0;

	for (i = 0, j = 0; i < naddr && j < nres; j++) {
		if (res[j].flags & IORESOURCE_MEM) {
			pdev_addr[i].addr_start = res[j].start;
			/* NOTE: the address range end is exclusive */
			pdev_addr[i].addr_end = res[j].end + 1;
			i++;
		}
	}
	return i;
}

static void free_aux_pages(int cnt, void *aux[])
{
	int ret;

	while (cnt--) {
		ret = rmi_granule_undelegate(virt_to_phys(aux[cnt]));
		if (!ret)
			free_page((unsigned long)aux[cnt]);
	}

}

static phys_addr_t get_ecam_base(struct pci_dev *pdev)
{
	struct pci_config_window *cfg = pdev->bus->sysdata;

	return cfg->res.start;
}

static int rmeda_host_init_ide(struct rmeda_host *rmeda_host)
{
	struct ide_addr_range ide_addr[3];
	int ret;
	int sid;
	int rid;
	int naddr;

	ret = check_ide_capabilities(rmeda_host->epdev);
	if (ret)
		return ret;

	ret = check_ide_capabilities(rmeda_host->rpdev);
	if (ret)
		return ret;

	sid = pcie_ide_sel_streamid_alloc(rmeda_host->rpdev);
	if (sid < 0) {
		pci_err(rmeda_host->rpdev, "Error allocating IDE stream\n");
		ret = -ENOENT;
		goto err_exit;
	}

	rmeda_host->sid = sid;
	pci_dbg(rmeda_host->rpdev, "Allocated sid:%d\n", sid);

	/* Program RID/Memory ranges for EP device */
	rid = pci_dev_id(rmeda_host->epdev);
	naddr = pci_res_to_ide_addr(rmeda_host->epdev, ide_addr);
	ret = pcie_ide_program_rp_stream(rmeda_host->rpdev, sid, rid, rid, ide_addr, naddr, 0);
	if (ret) {
		pci_err(rmeda_host->rpdev, "Error programming IDE stream\n");
		goto err_release_ide;
	}

	ret = pcie_ide_program_ep_stream(rmeda_host->epdev, sid, PCI_IDE_ENABLE_FLAGS_DEFAULT_STREAM);
	if (ret) {
		pci_err(rmeda_host->rpdev, "Error programming IDE stream\n");
		goto err_disable_rp_stream;
	}

	rmeda_host->ide_enabled = true;

	return 0;

err_disable_rp_stream:
	pcie_ide_disable(rmeda_host->rpdev, rmeda_host->sid);
err_release_ide:
	pcie_ide_sel_streamid_free(rmeda_host->rpdev, rmeda_host->sid);
err_exit:
	return ret;
}

static int rmeda_host_deinit_ide(struct rmeda_host *rmeda_host)
{
	pcie_ide_disable(rmeda_host->rpdev, rmeda_host->sid);
	pcie_ide_disable(rmeda_host->epdev, rmeda_host->sid);
	pcie_ide_sel_streamid_free(rmeda_host->rpdev, rmeda_host->sid);

	return 0;
}

static size_t pdev_get_num_aux(bool platform_dev)
{
	unsigned long num_aux;
	unsigned long flags;
	int ret;

	flags = platform_dev ?
		RMI_PDEV_PARAMS_USE_IDE :
		(RMI_PDEV_PARAMS_USE_IDE |
		 RMI_PDEV_PARAMS_USE_SPDM);
	ret = rmi_pdev_aux_count(flags, &num_aux);
	if (ret)
		return 0;

	return num_aux;
}

static int rmeda_host_init_pdev_params(struct rmeda_host *rmeda_host,
				       struct resource *ncoh_res,
				       size_t n_ncoh_res,
				       struct resource *coh_res,
				       size_t n_coh_res,
				       struct rmi_pdev_params *params)
{
	struct pci_dev *epdev = rmeda_host->epdev;
	void *aux;
	phys_addr_t aux_phys;
	int rid, ret, i;

	/* assign the ep device with RMM */
	rid = pci_dev_id(epdev);
	params->pdev_id = rid;
	params->root_id = pci_dev_id(rmeda_host->rpdev);
	params->segment_id = pci_domain_nr(epdev->bus);
	params->cert_id = 0;
	params->flags = rmeda_host->ide_enabled ?
			RMI_PDEV_PARAMS_USE_IDE : 0;
	params->flags |= !rmeda_host->platform_dev ?
			 RMI_PDEV_PARAMS_USE_SPDM : 0;
	params->ecam_addr = get_ecam_base(rmeda_host->epdev);
	params->ide_sid = rmeda_host->sid;
	params->hash_algo = RMI_HASH_SHA_256;

	/*
	 * Use the RID and MMIO resources of the epdev. Note that rid_top
	 * is exclusive as defined in the RMM specification (alpha-9,
	 * section B4.4.24, RmiPdevParams type)
	 */
	params->rid_base = rid;
	params->rid_top = rid + 1;
	params->ncoh_num_addr_range =
		pci_res_to_pdev_addr(params->ncoh_addr_range,
				     ARRAY_SIZE(params->ncoh_addr_range),
				     ncoh_res,
				     n_ncoh_res);
	params->coh_num_addr_range =
		pci_res_to_pdev_addr(params->coh_addr_range,
				     ARRAY_SIZE(params->coh_addr_range),
				     coh_res,
				     n_coh_res);

	params->num_aux = pdev_get_num_aux(rmeda_host->platform_dev);
	pr_debug("%s using %ld pdev aux granules\n", __func__, params->num_aux);
	rmeda_host->num_aux = params->num_aux;
	for (i = 0; i < params->num_aux; i++) {
		aux = (void *)__get_free_page(GFP_KERNEL);
		if (!aux) {
			ret = -ENOMEM;
			goto err_free_aux;
		}

		aux_phys = virt_to_phys(aux);
		if (rmi_granule_delegate(aux_phys)) {
			ret = -ENXIO;
			free_page((unsigned long)aux);
			goto err_free_aux;
		}
		params->aux[i] = aux_phys;
		rmeda_host->aux[i] = aux;
	}

	return 0;

err_free_aux:
	free_aux_pages(i, rmeda_host->aux[i]);
	return -ENOMEM;
}

static int rmeda_host_vdev_communicate(struct rmeda_host *rmeda_host)
{
	int ret;

	print_hex_dump_debug("RESP:", DUMP_PREFIX_ADDRESS, 16, 1,
			rmeda_host->resp_buff, rmeda_host->dev_comm_params->enter.resp_size, true);

	pr_debug("vdev_communicate: enter status: %ld size:%ld\n",
			rmeda_host->dev_comm_params->enter.status,
			rmeda_host->dev_comm_params->enter.resp_size);
	ret = rmi_vdev_communicate(virt_to_phys(rmeda_host->pdev),
				   virt_to_phys(rmeda_host->vdev),
				   virt_to_phys(rmeda_host->dev_comm_params));

	pr_debug("vdev_communicate exit: ret:%d flags:%ld size:%ld\n",
			ret,
			rmeda_host->dev_comm_params->exit.flags,
			rmeda_host->dev_comm_params->exit.req_len);

	if (rmeda_host->dev_comm_params->exit.flags & RMI_DEV_COMM_EXIT_SEND) {
		print_hex_dump_debug("REQ:", DUMP_PREFIX_ADDRESS,
					16, 1, rmeda_host->req_buff,
					rmeda_host->dev_comm_params->exit.req_len, true);
	}

	return ret;
}

static int rmeda_host_pdev_communicate(struct rmeda_host *rmeda_host)
{
	int ret;

	print_hex_dump_debug("RESP:", DUMP_PREFIX_ADDRESS, 16, 1,
			rmeda_host->resp_buff, rmeda_host->dev_comm_params->enter.resp_size, true);

	pr_debug("pdev_communicate: enter status: %ld size:%ld\n",
			rmeda_host->dev_comm_params->enter.status,
			rmeda_host->dev_comm_params->enter.resp_size);
	ret = rmi_pdev_communicate(virt_to_phys(rmeda_host->pdev),
					virt_to_phys(rmeda_host->dev_comm_params));

	pr_debug("pdev_communicate exit: ret:%d flags:%ld size:%ld\n",
			ret,
			rmeda_host->dev_comm_params->exit.flags,
			rmeda_host->dev_comm_params->exit.req_len);

	if (rmeda_host->dev_comm_params->exit.flags & RMI_DEV_COMM_EXIT_SEND) {
		print_hex_dump_debug("REQ:", DUMP_PREFIX_ADDRESS,
					16, 1, rmeda_host->req_buff,
					rmeda_host->dev_comm_params->exit.req_len, true);
	}

	return ret;
}

static int doe_req_resp(struct rmeda_host *rmeda_host)
{
	struct rmi_dev_comm_exit *io_exit;
	struct pci_doe_mb *doe_mb;
	u8 protocol;
	int ret;

	/*
	 * DOE is unsupported for platform attested devices, and
	 * therefore the realm should never request DOE.
	 */
	if (rmeda_host->platform_dev)
		return -EINVAL;

	io_exit = &rmeda_host->dev_comm_params->exit;

	pr_debug("doe_req size:%ld doe_protocol=%d\n",
		 io_exit->req_len,
		 (int)io_exit->protocol);

	if (io_exit->protocol == RMI_DEV_COMM_PROTOCOL_SPDM) {
		protocol = PCI_DOE_PROTOCOL_CMA_SPDM;
		doe_mb = rmeda_host->doe_cma;
	} else if (io_exit->protocol == RMI_DEV_COMM_PROTOCOL_SECURE_SPDM) {
		protocol = PCI_DOE_PROTOCOL_SEC_CMA_SPDM;
		doe_mb = rmeda_host->doe_scma;
	} else
		return -EINVAL;

	ret = pci_doe(doe_mb,
		      PCI_VENDOR_ID_PCI_SIG,
		      protocol,
		      rmeda_host->req_buff,
		      io_exit->req_len,
		      rmeda_host->resp_buff,
		      PAGE_SIZE);

	pr_debug("doe returned:%d\n", ret);

	return ret;

}

static void init_pdev_io(struct rmeda_host *rmeda_host)
{
	struct rmi_dev_comm_enter *io_enter;

	io_enter = &rmeda_host->dev_comm_params->enter;
	io_enter->resp_size = 0;
	io_enter->status = RMI_DEV_COMM_ENTER_SUCCESS;
}

static int do_pdev_io_single(struct rmeda_host *rmeda_host,
			     int (*communicate_fn)(struct rmeda_host *rmeda_host),
			     void *cache_buff,
			     size_t *cache_offset,
			     size_t *cache_remaining)
{
	int ret;
	int nbytes;
	struct rmi_dev_comm_enter *io_enter;
	struct rmi_dev_comm_exit *io_exit;

	io_enter = &rmeda_host->dev_comm_params->enter;
	io_exit = &rmeda_host->dev_comm_params->exit;

	ret = communicate_fn(rmeda_host);
	if (ret != RMI_SUCCESS) {
		pr_err("pdev communicate error\n");
		return ret;
	}

	/* caching request from RMM */
	if (io_exit->flags & RMI_DEV_COMM_EXIT_CACHE_RSP) {
		size_t copy_sz;

		if (cache_buff == NULL ||
		    cache_offset == NULL)
			return -EINVAL;

		copy_sz = MIN(io_exit->cache_rsp_len, *cache_remaining);
		pr_debug("Cache req= len:%ld, copy_sz:%ld, rem_sz:%ld, off:%ld\n",
				io_exit->cache_rsp_len, copy_sz,
				*cache_remaining, io_exit->cache_rsp_offset);
		memcpy(cache_buff + *cache_offset,
			(rmeda_host->resp_buff + io_exit->cache_rsp_offset),
			copy_sz);
		*cache_remaining -= copy_sz;
		*cache_offset += copy_sz;
	}

	/* wait for last packet request from RMM */
	if (io_exit->flags & RMI_DEV_COMM_EXIT_WAIT) {
		unsigned long time_left;

		time_left = msleep_interruptible(io_exit->timeout);
		if (time_left)
			return -ENXIO;
	}

	/* no support for caching the request buffer */
	if (io_exit->flags & RMI_DEV_COMM_EXIT_CACHE_REQ)
		return -ENXIO;

	/* next packet to send */
	if (io_exit->flags & RMI_DEV_COMM_EXIT_SEND) {
		nbytes = doe_req_resp(rmeda_host);
		if (nbytes < 0) {
			/* report error back to RMM */
			io_enter->status = RMI_DEV_COMM_ENTER_ERROR;
		} else {
			/* send response back to RMM */
			io_enter->resp_size = nbytes;
			io_enter->status = RMI_DEV_COMM_ENTER_RESPONSE;
		}
	} else {
		/* no data transmitted => no data received */
		io_enter->status = RMI_DEV_COMM_ENTER_SUCCESS;
		io_enter->resp_size = 0;
	}

	return 0;
}

/* Do pdev_communicate until it reaches the target state or encounters an error */
static int do_pdev_io(struct rmeda_host *rmeda_host,
		      int target_state0,
		      int target_state1,
		      void *cache_buff,
		      size_t *cache_sz)
{
	int ret;
	unsigned long state;
	size_t cache_remaining;

	init_pdev_io(rmeda_host);

	if (cache_buff && cache_sz) {
		cache_remaining = *cache_sz;
		*cache_sz = 0;
	}

	state = -1;
	do {
		ret = do_pdev_io_single(rmeda_host, rmeda_host_pdev_communicate,
					cache_buff, cache_sz,
					&cache_remaining);
		if (ret != 0) {
			pr_err("pdev communication error\n");
			break;
		}

		ret = rmi_pdev_get_state(virt_to_phys(rmeda_host->pdev), &state);
		if (ret != 0) {
			pr_err("Get pdev state error\n");
			break;
		}
	} while (state != target_state0 &&
		 state != target_state1 &&
		 state != RMI_PDEV_ERROR);

	pr_info("pdev_io_complete: status: %d state:%ld\n", ret, state);

	return state;
}

static int rmeda_host_init_io(struct rmeda_host *rmeda_host)
{
	rmeda_host->dev_comm_params = (struct rmi_dev_comm_data *)get_zeroed_page(GFP_KERNEL);
	rmeda_host->resp_buff = (void *)__get_free_page(GFP_KERNEL);
	rmeda_host->req_buff = (void *)__get_free_page(GFP_KERNEL);

	if (!rmeda_host->dev_comm_params ||
	    !rmeda_host->req_buff ||
	    !rmeda_host->resp_buff)
		return -ENOMEM;

	rmeda_host->dev_comm_params->enter.status = RMI_DEV_COMM_ENTER_SUCCESS;
	rmeda_host->dev_comm_params->enter.resp_size = 0;
	rmeda_host->dev_comm_params->enter.resp_buff = virt_to_phys(rmeda_host->resp_buff);
	rmeda_host->dev_comm_params->enter.req_buff = virt_to_phys((void *)rmeda_host->req_buff);

	return 0;
}

static void rmeda_host_deinit_io(struct rmeda_host *rmeda_host)
{
	free_page((unsigned long) rmeda_host->dev_comm_params);
	free_page((unsigned long) rmeda_host->resp_buff);
	free_page((unsigned long) rmeda_host->req_buff);
}

static int vdev_communicate(struct kvm *kvm,
			    unsigned long io_action,
			    void *priv)
{
	struct rmeda_host *rmeda_host = priv;
	void *cache_buf = NULL;
	size_t *cache_remaining = NULL;
	size_t *cache_offset = NULL;
	unsigned long state = -1;
	int ret;

	switch (io_action) {
	case RMI_VDEV_ACTION_GET_INTERFACE_REPORT:
		cache_buf = rmeda_host->interface_report.buf;
		cache_remaining = &rmeda_host->interface_report.remaining;
		cache_offset = &rmeda_host->interface_report.size;

		if (!rmeda_host->vdev_active) {
			*cache_remaining = sizeof(rmeda_host->interface_report.buf);
			*cache_offset = 0;
		}
		break;
	case RMI_VDEV_ACTION_GET_MEASUREMENTS:
		cache_buf = rmeda_host->measurements.buf;
		cache_remaining = &rmeda_host->measurements.remaining;
		cache_offset = &rmeda_host->measurements.size;

		if (!rmeda_host->vdev_active) {
			*cache_remaining = sizeof(rmeda_host->measurements.buf);
			*cache_offset = 0;
		}
		break;
	}

	if (!rmeda_host->vdev_active) {
		init_pdev_io(rmeda_host);
		rmeda_host->vdev_active = true;
	}

	ret = do_pdev_io_single(rmeda_host, rmeda_host_vdev_communicate,
				cache_buf, cache_offset,
				cache_remaining);
	if (ret != 0) {
		pr_err("pdev communication error\n");
		return 1;
	}

	ret = rmi_vdev_get_state(virt_to_phys(rmeda_host->vdev), &state);
	if (ret != 0) {
		pr_err("Get vdev state error\n");
		return 1;
	}

	/*
	 * If vdev is done communicating, the next update should
	 * reinitialize the cache
	 */
	if (state != RMI_VDEV_COMMUNICATING)
		rmeda_host->vdev_active = false;

	/* Ignore the result. RMM will report the error to the realm. */
	return 1;
}

static int control_ide_call(struct kvm_vcpu *vcpu, unsigned long hsi, void *priv)
{
	struct rmeda_host *rmeda_host = priv;
	bool enable = vcpu_get_reg(vcpu, 2);

	if (enable)
		pcie_ide_enable(rmeda_host->epdev, rmeda_host->sid);
	else
		pcie_ide_disable(rmeda_host->epdev, rmeda_host->sid);

	/* Operation complete */
	vcpu_set_reg(vcpu, 0, 0);

	/* Return to realm */
	return 1;
}

static int read_cache_call(struct kvm_vcpu *vcpu, unsigned long hsi, void *priv)
{
	struct rmeda_host *rmeda_host = priv;
	unsigned long vdev_id = vcpu_get_reg(vcpu, 1);
	unsigned long type = vcpu_get_reg(vcpu, 2);
	size_t src_offset = vcpu_get_reg(vcpu, 3);
	size_t max_len = vcpu_get_reg(vcpu, 4);
	unsigned long addr = vcpu_get_reg(vcpu, 5);
	size_t len;
	void *buf;

	struct kvm_memory_slot *memslot;
	struct vm_area_struct *vma;
	unsigned long hva;
	bool writable;
	gpa_t gpa;
	gfn_t gfn;
	int ret = 0;

	/* Determine the buffer that should be used */
	if (type == 0) {
		len = rmeda_host->cert_chain.size;
		buf = rmeda_host->cert_chain.buf;
	} else if (type == 1) {
		len = rmeda_host->measurements.size;
		buf = rmeda_host->measurements.buf;
	} else if (type == 2) {
		len = rmeda_host->interface_report.size;
		buf = rmeda_host->interface_report.buf;
	} else {
		ret = -EINVAL;
		goto err_out;
	}

	if (src_offset >= len) {
		ret = -EINVAL;
		goto err_out;
	}

	/* Update the parameters based on the offset */
	buf = ((unsigned char *)buf) + src_offset;
	len -= src_offset;

	/* Take into account the maximum length */
	len = max_len < len ? max_len : len;

	/* Current implementation supports only one vdev-per-pdev */
	if (vdev_id != rmeda_host->vdev_id) {
		ret = -ENOMEM;
		goto err_out;
	}

	/* Search and validate the user memory address */

	gpa = kvm_gpa_from_fault(vcpu->kvm, addr);
	gfn = gpa >> PAGE_SHIFT;
	memslot = gfn_to_memslot(vcpu->kvm, gfn);
	hva = gfn_to_hva_memslot_prot(memslot, gfn, &writable);
	if (kvm_is_error_hva(hva)) {
		ret = -EINVAL;
		goto err_out;
	}

	vma = vma_lookup(current->mm, hva);
	if (!vma) {
		ret = -EINVAL;
		goto err_out;
	}

	/* Take the offset within the page into account */
	hva = hva + (addr & (~PAGE_MASK));

	/* Copy the report */
	ret = copy_to_user((void __user *)hva, buf, len);
	if (ret) {
		ret = -EINVAL;
		goto err_out;
	}

	vcpu_set_reg(vcpu, 0, 0);
	vcpu_set_reg(vcpu, 1, len);
	vcpu_set_reg(vcpu, 2, 0);

	return 1;

err_out:
	vcpu_set_reg(vcpu, 0, ret);
	vcpu_set_reg(vcpu, 1, 0);
	vcpu_set_reg(vcpu, 2, 0);

	return 1;
}

int rmeda_host_attach(struct rmeda_host *rmeda_host,
		      struct kvm *kvm,
		      u64 vdev_id)
{
	phys_addr_t pdev_phys, rd_phys, vdev_phys;
	struct rmi_vdev_params *params;
	struct pci_dev *epdev;
	unsigned long state;
	void *vdev;
	int ret;

	if (!rmeda_host || !kvm || !kvm_is_realm(kvm))
		return -EINVAL;

	if (rmeda_host->kvm)
		return -EBUSY;

	epdev = rmeda_host->epdev;
	pdev_phys = virt_to_phys(rmeda_host->pdev);
	rd_phys = virt_to_phys(kvm_get_rd(kvm));

	ret = rmi_pdev_get_state(virt_to_phys(rmeda_host->pdev), &state);
	if (ret != 0) {
		pci_warn(rmeda_host->epdev, "failed to get device state\n");
		return -EBADFD;
	}

	if (state != RMI_PDEV_READY) {
		pci_warn(rmeda_host->epdev, "invalid device state\n");
		return -EBADFD;
	}

	vdev = (void *)get_zeroed_page(GFP_KERNEL);
	if (!vdev)
		return -ENOMEM;

	vdev_phys = virt_to_phys(vdev);
	if (rmi_granule_delegate(vdev_phys)) {
		ret = -ENXIO;
		goto err_free_pdev;
	}

	params = (struct rmi_vdev_params *)get_zeroed_page(GFP_KERNEL);
	if (!params) {
		ret = -ENOMEM;
		goto err_granule_undelegate;
	}

	/* The parameters must be setup before registering callbacks */
	rmeda_host->vdev_id = vdev_id;
	rmeda_host->vdev_active = false;
	rmeda_host->kvm = kvm;
	rmeda_host->vdev = vdev;

	ret = kvm_realm_register_vdev(kvm, vdev_id, vdev_phys);
	if (ret)
		goto err_clear_props;

	ret = kvm_realm_register_io_callback(kvm, vdev_phys,
					     vdev_communicate, rmeda_host);
	if (ret)
		goto err_unregister_vdev;

	ret = kvm_realm_register_hsi_callback(kvm, RHI_DA_OBJECT_READ, vdev_id,
					      read_cache_call, rmeda_host);
	if (ret)
		goto err_unregister_io_callback;

	if (rmeda_host->ide_enabled && rmeda_host->platform_dev) {
		ret = kvm_realm_register_hsi_callback(kvm, RHI_DA_CONTROL_IDE, vdev_id,
						      control_ide_call, rmeda_host);
		if (ret)
			goto err_unregister_hsi_da_callback;
	}

	params->flags = 0;
	params->vdev_id = vdev_id;
	params->tdi_id = pci_dev_id(epdev);
	params->num_aux = 0;

	ret = rmi_vdev_create(rd_phys, pdev_phys, vdev_phys, virt_to_phys(params));
	if (ret)
		goto err_unregister_hsi_ide_callback;

	goto done;

err_unregister_hsi_ide_callback:
	if (rmeda_host->ide_enabled && rmeda_host->platform_dev)
		kvm_realm_unregister_hsi_callback(kvm, RHI_DA_CONTROL_IDE, vdev_id);
err_unregister_hsi_da_callback:
	kvm_realm_unregister_hsi_callback(kvm, RHI_DA_OBJECT_READ, vdev_id);
err_unregister_io_callback:
	kvm_realm_unregister_io_callback(kvm, vdev_phys);
err_unregister_vdev:
	kvm_realm_unregister_vdev(kvm, vdev_id);
err_clear_props:
	rmeda_host->vdev_id = 0;
	rmeda_host->vdev_active = false;
	rmeda_host->kvm = NULL;
	rmeda_host->vdev = NULL;
err_granule_undelegate:
	rmi_granule_undelegate(vdev_phys);
err_free_pdev:
	free_page((unsigned long)vdev);
done:
	free_page((unsigned long)params);

	return ret;
}
EXPORT_SYMBOL_GPL(rmeda_host_attach);

void rmeda_host_detach(struct rmeda_host *rmeda_host)
{
	phys_addr_t vdev_phys, rd_phys;
	unsigned long state;
	int ret;

	if (!rmeda_host || !rmeda_host->kvm)
		return;

	vdev_phys = virt_to_phys(rmeda_host->vdev);
	rd_phys = virt_to_phys(kvm_get_rd(rmeda_host->kvm));

	/* Request stopping the VDEV */
	ret = rmi_vdev_stop(vdev_phys);
	if (ret) {
		pr_err("failed to stop vdev (%d)\n", ret);
		return;
	}

	do {
		ret = do_pdev_io_single(rmeda_host, rmeda_host_vdev_communicate,
					NULL, NULL, NULL);
		if (ret != 0) {
			pr_err("failed to stop vdev: communication error (%d)\n", ret);
			return;
		}

		ret = rmi_vdev_get_state(vdev_phys, &state);
		if (ret != 0) {
			pr_err("failed to stop vdev: get vdev state error (%d)\n", ret);
			return;
		}
	} while (state != RMI_VDEV_STOPPED && state != RMI_VDEV_ERROR);

	ret = rmi_vdev_destroy(rd_phys, virt_to_phys(rmeda_host->pdev), vdev_phys);
	if (ret) {
		pr_err("failed to destroy vdev (%d)\n", ret);
		return;
	}

	kvm_realm_unregister_vdev(rmeda_host->kvm, rmeda_host->vdev_id);
	if (rmeda_host->ide_enabled && rmeda_host->platform_dev) {
		kvm_realm_unregister_hsi_callback(rmeda_host->kvm,
						  RHI_DA_CONTROL_IDE,
						  rmeda_host->vdev_id);
		/*
		 * Ensure that IDE gets disabled in EP side
		 * if it is controlled by the realm
		 */
		pcie_ide_disable(rmeda_host->epdev, rmeda_host->sid);
	}
	kvm_realm_unregister_hsi_callback(rmeda_host->kvm, RHI_DA_OBJECT_READ,
					  rmeda_host->vdev_id);
	kvm_realm_unregister_io_callback(rmeda_host->kvm,
					 virt_to_phys(rmeda_host->vdev));

	ret = rmi_granule_undelegate(vdev_phys);
	if (ret) {
		pr_err("failed to undelegate vdev page (%d)\n", ret);
		return;
	}

	free_page((unsigned long)rmeda_host->vdev);

	/* Clear tracking information */
	rmeda_host->vdev_id = 0;
	rmeda_host->vdev_active = false;
	rmeda_host->kvm = NULL;
	rmeda_host->vdev = NULL;

}
EXPORT_SYMBOL_GPL(rmeda_host_detach);

static int rmeda_host_set_key(struct rmeda_host *rmeda_host)
{
	struct rmi_pubkey *key_shared;
	int ret;

	/* Check that all the necessary information was captured from communication */
	if (!rmeda_host->cert_chain.valid)
		return -EINVAL;

	key_shared = (struct rmi_pubkey *)get_zeroed_page(GFP_KERNEL);
	if (!key_shared)
		return -ENOMEM;

	key_shared->metadata_len = 0;
	key_shared->rmi_signature_algorithm = rmeda_host->rmi_signature_algorithm;

	if (key_shared->rmi_signature_algorithm != RMI_SIG_RSASSA_3072) {
		key_shared->public_key_len = rmeda_host->cert_chain.public_key_size;
		memcpy(key_shared->public_key,
		       rmeda_host->cert_chain.public_key,
		       rmeda_host->cert_chain.public_key_size);
	} else {
		struct rsa_key rsa_key = {0};

		ret = rsa_parse_pub_key(&rsa_key,
					rmeda_host->cert_chain.public_key,
					rmeda_host->cert_chain.public_key_size);
		if (ret) {
			free_page((unsigned long)key_shared);
			return ret;
		}

		key_shared->public_key_len = rsa_key.n_sz;
		key_shared->metadata_len = rsa_key.e_sz;
		memcpy(key_shared->public_key, rsa_key.n, rsa_key.n_sz);
		memcpy(key_shared->metadata, rsa_key.e, rsa_key.e_sz);
	}

	ret = rmi_pdev_set_pubkey(virt_to_phys(rmeda_host->pdev),
				  virt_to_phys(key_shared));

	free_page((unsigned long)key_shared);

	return ret;
}

static int process_certificate_chain(struct rmeda_host *rmeda_host)
{
	u8 *chain_data = rmeda_host->cert_chain.buf;
	unsigned int chain_size = rmeda_host->cert_chain.size;
	unsigned int offset = 0;
	int ret = 0;

	while (offset < chain_size) {
		unsigned int cert_len =
			x509_get_certificate_length(chain_data + offset,
						    chain_size - offset);
		struct x509_certificate *cert =
			x509_cert_parse(chain_data + offset, cert_len);

		if (IS_ERR(cert)) {
			pr_warn("%s(): parsing of certificate chain not successful\n", __func__);
			ret = PTR_ERR(cert);
			break;
		}

		if (offset + cert_len == chain_size) {
			rmeda_host->cert_chain.public_key = kzalloc(cert->pub->keylen, GFP_KERNEL);
			if (!rmeda_host->cert_chain.public_key) {
				ret = -ENOMEM;
				x509_free_certificate(cert);
				break;
			}

			if (!strcmp("ecdsa-nist-p256", cert->pub->pkey_algo)) {
				rmeda_host->rmi_signature_algorithm = RMI_SIG_ECDSA_P256;
			} else if (!strcmp("ecdsa-nist-p384", cert->pub->pkey_algo)) {
				rmeda_host->rmi_signature_algorithm = RMI_SIG_ECDSA_P384;
			} else if (!strcmp("rsa", cert->pub->pkey_algo)) {
				rmeda_host->rmi_signature_algorithm = RMI_SIG_RSASSA_3072;
			} else {
				ret = -ENXIO;
				x509_free_certificate(cert);
				break;
			}
			memcpy(rmeda_host->cert_chain.public_key, cert->pub->key, cert->pub->keylen);
			rmeda_host->cert_chain.public_key_size = cert->pub->keylen;
		}

		x509_free_certificate(cert);

		offset += cert_len;
	}

	if (ret == 0)
		rmeda_host->cert_chain.valid = true;

	return ret;
}

static int rmeda_host_stop_pdev(struct rmeda_host *rmeda_host)
{
	phys_addr_t pdev_phys = virt_to_phys(rmeda_host->pdev);
	unsigned long state;
	unsigned long ret;

	ret = rmi_pdev_stop(pdev_phys);
	if (WARN_ON(ret != RMI_SUCCESS))
		return -EIO;

	state = do_pdev_io(rmeda_host, RMI_PDEV_STOPPED, -1, NULL, NULL);
	if (WARN_ON(state != RMI_PDEV_STOPPED))
		return -EIO;

	ret = rmi_pdev_destroy(pdev_phys);
	if (WARN_ON(ret != RMI_SUCCESS))
		return -EIO;

	return 0;
}

static int rmeda_host_init_pdev(struct rmeda_host *rmeda_host,
				struct resource *ncoh_res,
				size_t n_ncoh_res,
				struct resource *coh_res,
				size_t n_coh_res)
{
	struct rmi_pdev_params *params;
	phys_addr_t pdev_phys;
	unsigned long state;
	void *pdev;
	int ret;

	pdev = (void *)get_zeroed_page(GFP_KERNEL);
	if (!pdev) {
		ret = -ENOMEM;
		goto err_out;
	}

	rmeda_host->pdev = pdev;

	pdev_phys = virt_to_phys(pdev);
	if (rmi_granule_delegate(pdev_phys)) {
		ret = -ENXIO;
		goto err_free_pdev;
	}

	params = (struct rmi_pdev_params *)get_zeroed_page(GFP_KERNEL);
	if (!params) {
		ret = -ENOMEM;
		goto err_granule_undelegate;
	}

	ret = rmeda_host_init_pdev_params(rmeda_host,
					  ncoh_res,
					  n_ncoh_res,
					  coh_res,
					  n_coh_res,
					  params);
	if (ret)
		goto err_free_params;

	ret = rmi_pdev_create(pdev_phys, virt_to_phys(params));
	pr_info("rmi_pdev_create(0x%llx, 0x%llx): %d\n", pdev_phys, virt_to_phys(params), ret);
	if (ret)
		goto err_free_aux;

	rmi_pdev_get_state(virt_to_phys(rmeda_host->pdev), &state);
	if (state != RMI_PDEV_NEW) {
		ret = -EIO;
		goto err_free_aux;
	}

	rmeda_host->cert_chain.size = sizeof(rmeda_host->cert_chain.buf);
	state = do_pdev_io(rmeda_host, RMI_PDEV_NEEDS_KEY, RMI_PDEV_READY,
			   rmeda_host->cert_chain.buf, &rmeda_host->cert_chain.size);

	/* Special case: Platform devices enter PDEV_READY immediately */
	if (rmeda_host->platform_dev) {
		if (state != RMI_PDEV_READY) {
			ret = -EIO;
			goto err_stop_pdev;
		}

		ret = 0;
		goto done;
	}

	/* Otherwise, RMM should be waiting for a key */
	if (state != RMI_PDEV_NEEDS_KEY) {
		ret = -EIO;
		goto err_stop_pdev;
	}

	ret = process_certificate_chain(rmeda_host);
	if (ret)
		goto err_stop_pdev;

	ret = rmeda_host_set_key(rmeda_host);
	if (ret)
		goto err_stop_pdev;

	state = do_pdev_io(rmeda_host, RMI_PDEV_READY, -1, NULL, NULL);
	if (state != RMI_PDEV_READY) {
		ret = -EIO;
		goto err_stop_pdev;
	}

	/* Enable the IDE in EP. RMM has enabled IDE in RP. */
	pcie_ide_enable(rmeda_host->epdev, rmeda_host->sid);

done:
	free_page((unsigned long)params);
	pci_info(rmeda_host->epdev, "DA dev initialization done\n");
	return 0;

err_stop_pdev:
	/*
	 * If stopping of PDEV fails, return without further clean-up.
	 * Pages are lost, and accessing them would cause a fault.
	 */
	if (WARN_ON(rmeda_host_stop_pdev(rmeda_host)))
		return ret;
err_free_aux:
	free_aux_pages(rmeda_host->num_aux, rmeda_host->aux);
err_free_params:
	free_page((unsigned long)params);
err_granule_undelegate:
	rmi_granule_undelegate(pdev_phys);
err_free_pdev:
	free_page((unsigned long)pdev);
err_out:
	return ret;
}

static void rmeda_host_deinit_pdev(struct rmeda_host *rmeda_host)
{
	/*
	 * If stopping of PDEV fails, return without further clean-up.
	 * Pages are lost, and accessing them would cause a fault.
	 */
	if (WARN_ON(rmeda_host_stop_pdev(rmeda_host)))
		return;

	free_aux_pages(rmeda_host->num_aux, rmeda_host->aux);
	if (!rmi_granule_undelegate(virt_to_phys(rmeda_host->pdev)))
		free_page((unsigned long)rmeda_host->pdev);
}

struct rmeda_host *rmeda_host_register(struct pci_dev *pdev,
				       bool platform_dev,
				       struct resource *ncoh_res,
				       size_t n_ncoh_res,
				       struct resource *coh_res,
				       size_t n_coh_res)
{
	struct rmeda_host *rmeda_host;
	int ret;

	if (!pdev)
		return NULL;

	if (!pci_is_pcie(pdev))
		return NULL;

	if (pci_pcie_type(pdev) != PCI_EXP_TYPE_ENDPOINT)
		return NULL;

	rmeda_host = kzalloc(sizeof(*rmeda_host), GFP_KERNEL);
	if (!rmeda_host)
		return NULL;

	rmeda_host->platform_dev = platform_dev;
	rmeda_host->epdev = pdev;

	ret = rdev_walk_get_pcie(rmeda_host);
	if (ret) {
		pr_err("Failed to identify the rootport\n");
		goto err_config;
	}

	ret = rmeda_host_init_ide(rmeda_host);
	if (ret)
		pci_warn(rmeda_host->epdev, "IDE unsupported. Attempting registration w/o IDE\n");

	if (!rmeda_host->platform_dev) {
		ret = probe_doe_mb(rmeda_host);
		if (ret)
			goto err_get_mbs;
	}

	/* initialize communication buffers with RMM */
	ret = rmeda_host_init_io(rmeda_host);
	if (ret) {
		pci_info(rmeda_host->epdev, "Failed to register communication buffers\n");
		goto err_init_io;
	}

	ret = rmeda_host_init_pdev(rmeda_host,
				   ncoh_res,
				   n_ncoh_res,
				   coh_res,
				   n_coh_res);
	if (ret)
		goto err_assign;

	return rmeda_host;

err_assign:
	rmeda_host_deinit_io(rmeda_host);
err_init_io:
err_get_mbs:
	if (rmeda_host->ide_enabled)
		rmeda_host_deinit_ide(rmeda_host);
err_config:
	kfree(rmeda_host);
	return NULL;
}
EXPORT_SYMBOL_GPL(rmeda_host_register);

void rmeda_host_unregister(struct rmeda_host *rmeda_host)
{
	if (!rmeda_host)
		return;

	rmeda_host_deinit_pdev(rmeda_host);
	if (rmeda_host->ide_enabled)
		rmeda_host_deinit_ide(rmeda_host);
	rmeda_host_deinit_io(rmeda_host);
	kfree(rmeda_host);
}
EXPORT_SYMBOL_GPL(rmeda_host_unregister);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Arto Merilainen <amerilainen@nvidia.com>");
MODULE_DESCRIPTION("Routines to allow registering devices with RMM");
