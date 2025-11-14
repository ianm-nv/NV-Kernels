// SPDX-License-Identifier: GPL-2.0-only
/*
 * SPDX-FileCopyrightText: Copyright (c) 2025, NVIDIA CORPORATION & AFFILIATES. All rights reserved
 */

#include <linux/module.h>
#include <linux/sizes.h>
#include <linux/pci.h>
#include <linux/io.h>

#include <linux/rmeda_host.h>

#define RMEDA_CMEM_DEV_NAME	"rmeda-cmem"

struct rmeda_cmem_priv {
	struct pci_dev *dev;

	struct rmeda_host *rmeda_host;
};

static int rmeda_cmem_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
	struct rmeda_cmem_priv *priv;
	int ret = 0;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;

	ret = pci_enable_device(dev);
	if (ret)
		goto err_enable_pci_device;

	priv->rmeda_host = rmeda_host_register(dev,
					       true, /* Enable SPDM */
					       true, /* Enable CXL_CMEM */
					       true, /* Enable SEL_IDE */
					       false, /* Disable link IDE */
					       dev->resource,
					       DEVICE_COUNT_RESOURCE,
					       NULL, 0);
	if (!priv->rmeda_host) {
		ret = -ENOMEM;
		goto err_register_rmeda;
	}

	pci_set_drvdata(dev, priv);

	return 0;

err_register_rmeda:
	pci_disable_device(dev);
err_enable_pci_device:
	kfree(priv);

	return ret;
}

static void rmeda_cmem_remove(struct pci_dev *dev)
{
	struct rmeda_cmem_priv *priv = pci_get_drvdata(dev);

	rmeda_host_unregister(priv->rmeda_host);

	pci_disable_device(dev);
	kfree(priv);
}

const struct pci_device_id rmeda_cmem_tbl[] = {
	{ .vendor = 0xdead, .device = 0xdead, .override_only = 1 },
	{0, }
};

static struct pci_driver rmeda_cmem_driver = {
	.name		= RMEDA_CMEM_DEV_NAME,
	.id_table	= rmeda_cmem_tbl,
	.probe		= rmeda_cmem_probe,
	.remove		= rmeda_cmem_remove,
	.driver_managed_dma = true,
};

static int __init rmeda_cmem_init(void)
{
	int ret;

	ret = pci_register_driver(&rmeda_cmem_driver);
	if (ret)
		return ret;

	return 0;
}
module_init(rmeda_cmem_init);

static void __exit rmeda_cmem_exit(void)
{
	pci_unregister_driver(&rmeda_cmem_driver);
}
module_exit(rmeda_cmem_exit);

MODULE_LICENSE("GPL");
