// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024 ARM Ltd.
 * SPDX-FileCopyrightText: Copyright (C) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include <linux/sizes.h>
#include <linux/vfio_pci_core.h>
#include <linux/rmeda_host.h>
#include <asm/virt.h>

#define RME_DA_NAME    "rmeda"

/*
 * If non-zero, RMI_PDEV_CREATE is called without setting
 * the RMI_PDEV_PARAMS_USE_SPDM flag, and DOE is left
 * uninitialized. This mode is intended to be used in cases
 * where the device is treated as trusted based on platform
 * construction and it does not support DOE.
 *
 * If the flag is 0, the driver sets the RMI_PDEV_PARAMS_USE_SPDM
 * flag and allows RMM to request DOE with the device.
 */
static int platform_dev;
module_param(platform_dev, int, 0660);

/* If non-zero, link IDE is enabled. */
static int enable_link_ide = 1;
module_param(enable_link_ide, int, 0660);

/* If non-zero, selective IDE is enabled. */
static int enable_sel_ide = 1;
module_param(enable_sel_ide, int, 0660);

struct kvm_rme_device {
	struct vfio_pci_core_device core_device;
	struct rmeda_host *rmeda_host;
};

static int rmeda_open_device(struct vfio_device *core_vdev)
{
	struct vfio_pci_core_device *vdev =
		container_of(core_vdev, struct vfio_pci_core_device, vdev);
	int ret;

	if (!core_vdev->kvm) {
		pci_warn(vdev->pdev, "kvm is not set for the device\n");
		return -EINVAL;
	}

	ret = vfio_pci_core_enable(vdev);
	if (ret)
		return ret;

	vfio_pci_core_finish_enable(vdev);

	return 0;
}

static long rmeda_set_dev_info(struct vfio_device *core_vdev, unsigned long arg)
{
	struct vfio_pci_core_device *vdev =
		container_of(core_vdev, struct vfio_pci_core_device, vdev);
	struct kvm_rme_device *rme_dev = dev_get_drvdata(core_vdev->dev);
	struct vfio_dev_info info;

	if (copy_from_user(&info, (void __user *)arg, sizeof(info)))
		return -EFAULT;

	if (core_vdev->kvm == NULL) {
		pci_warn(vdev->pdev, "kvm is not set for the device\n");
		return -EINVAL;
	}

	return rmeda_host_attach(rme_dev->rmeda_host,
				 core_vdev->kvm,
				 info.dev_num);
}

static long rmeda_ioctl(struct vfio_device *core_vdev,
			unsigned int cmd, unsigned long arg)
{
	if (cmd == VFIO_DEVICE_SET_DEV_INFO)
		return rmeda_set_dev_info(core_vdev, arg);

	return vfio_pci_core_ioctl(core_vdev, cmd, arg);
}

static void rmeda_close_device(struct vfio_device *core_vdev)
{
	struct kvm_rme_device *rme_dev = dev_get_drvdata(core_vdev->dev);

	rmeda_host_detach(rme_dev->rmeda_host);
	vfio_pci_core_close_device(core_vdev);
}

static const struct vfio_device_ops rmeda_ops = {
	.name		= "rmeda-vfio-pci",
	.init		= vfio_pci_core_init_dev,
	.release	= vfio_pci_core_release_dev,
	.open_device	= rmeda_open_device,
	.close_device	= rmeda_close_device,
	.ioctl		= rmeda_ioctl,
	.device_feature = vfio_pci_core_ioctl_feature,
	.read		= vfio_pci_core_read,
	.write		= vfio_pci_core_write,
	.mmap		= vfio_pci_core_mmap,
	.request	= vfio_pci_core_request,
	.match		= vfio_pci_core_match,
	.match_token_uuid = vfio_pci_core_match_token_uuid,
	.bind_iommufd	= vfio_iommufd_physical_bind,
	.unbind_iommufd	= vfio_iommufd_physical_unbind,
	.attach_ioas	= vfio_iommufd_physical_attach_ioas,
	.detach_ioas	= vfio_iommufd_physical_detach_ioas,
};

static int rme_da_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	struct kvm_rme_device *rme_dev;
	int ret;

	/* Assume that RME is available */
	if (!static_branch_unlikely(&kvm_rme_is_available))
		return -ENOTTY;

	rme_dev = vfio_alloc_device(kvm_rme_device, core_device.vdev, &pdev->dev,
				 &rmeda_ops);
	if (IS_ERR(rme_dev))
		return PTR_ERR(rme_dev);

	dev_set_drvdata(&pdev->dev, &rme_dev->core_device);

	rme_dev->rmeda_host = rmeda_host_register(pdev, platform_dev,
						  enable_sel_ide,
						  enable_link_ide,
						  pdev->resource,
						  DEVICE_COUNT_RESOURCE,
						  NULL, 0);
	if (!rme_dev->rmeda_host) {
		ret = -ENOMEM;
		goto err_register_rmeda;
	}

	ret = vfio_pci_core_register_device(&rme_dev->core_device);
	if (ret)
		goto err_register_vfio;

	return 0;

err_register_vfio:
	rmeda_host_unregister(rme_dev->rmeda_host);
err_register_rmeda:
	vfio_put_device(&rme_dev->core_device.vdev);
	pci_set_drvdata(pdev, NULL);
	return ret;
}

static void rme_da_remove(struct pci_dev *pdev)
{
	struct kvm_rme_device *rme_dev = pci_get_drvdata(pdev);

	vfio_pci_core_unregister_device(&rme_dev->core_device);
	rmeda_host_unregister(rme_dev->rmeda_host);
	pci_set_drvdata(pdev, NULL);
	vfio_put_device(&rme_dev->core_device.vdev);
}

const struct pci_device_id rme_da_pci_tbl[] = {
	/* Don't match to any particular pci device id and allow override */
	{ .vendor = 0xdead, .device = 0xdead, .override_only = 1 },
	{ 0, }
};

static struct pci_driver rme_da_driver = {
	.name = RME_DA_NAME,
	.id_table = rme_da_pci_tbl,
	.probe = rme_da_probe,
	.remove = rme_da_remove,
	.err_handler =  &vfio_pci_core_err_handlers,
	.driver_managed_dma = true,
};

static int __init rme_da_module_init(void)
{
	return pci_register_driver(&rme_da_driver);
}
module_init(rme_da_module_init);

static void __exit rme_da_module_exit(void)
{
	pci_unregister_driver(&rme_da_driver);
}
module_exit(rme_da_module_exit);

MODULE_LICENSE("GPL");
