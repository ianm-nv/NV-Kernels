/* SPDX-License-Identifier: GPL-2.0 */
/*
 * SPDX-FileCopyrightText: Copyright (C) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#ifndef RMEDA_HOST_H
#define RMEDA_HOST_H

#include <linux/types.h>

struct pci_dev;
struct rmeda_host;
struct kvm;

#if IS_ENABLED(CONFIG_RMEDA_HOST)

void rmeda_host_unregister(struct rmeda_host *rmeda_host);
struct rmeda_host *rmeda_host_register(struct pci_dev *pdev,
				       bool platform_dev,
				       struct resource *ncoh_res,
				       size_t n_ncoh_res,
				       struct resource *coh_res,
				       size_t n_coh_res);
void rmeda_host_detach(struct rmeda_host *rmeda_host);
int rmeda_host_attach(struct rmeda_host *rmeda_host,
		      struct kvm *kvm,
		      u64 vdev_id);

#else

static inline void rmeda_host_unregister(struct rmeda_host *rmeda_host)
{
}

static inline struct rmeda_host *rmeda_host_register(struct pci_dev *pdev,
						     bool platform_dev,
						     struct resource *ncoh_res,
						     size_t n_ncoh_res,
						     struct resource *coh_res,
						     size_t n_coh_res)
{
	return NULL;
}

static inline void rmeda_host_detach(struct rmeda_host *rmeda_host)
{
}

static inline int rmeda_host_attach(struct rmeda_host *rmeda_host,
				    struct kvm *kvm,
				    u64 vdev_id)
{
	return -ENOTTY;
}

#endif /* defined(CONFIG_RMEDA_HOST) || defined(CONFIG_RMEDA_HOST_MODULE) */

#endif /* RMEDA_HOST_H */
