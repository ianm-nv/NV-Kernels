// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024 ARM Ltd.
 * SPDX-FileCopyrightText: Copyright (C) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#define dev_fmt(fmt) "IDE: " fmt

#include <linux/bitfield.h>
#include <linux/pci.h>
#include <linux/pci-doe.h>
#include <linux/pci-ide.h>
#include <linux/atomic.h>

#include "pci.h"


/* IDE config space layout:
 * IDE_HDR
 * IDE_CAP
 * IDE_CTRL
 * LINK_IDE ctrl---------------------------+
 * LINK_IDE status-------------------------+  x IDE_CAP.num_tc
 * SEL_CAP-----------------------------+
 * SEL_CTL                             |
 * SEL_STA                             | x IDE_CAP.num_sel
 * IDE_RID{1,2}                        |
 * IDE_SEL_ADDR{1,2,3} ----- x SEL_CAP.n---+
 *
 */

#define IDE_CAP_REG			0x4
#define IDE_CAP_LINK_BIT		BIT(0)
#define IDE_CAP_SEL_BIT			BIT(1)
#define IDE_CAP_FLOW_THRU_BIT		BIT(2)
#define IDE_CAP_KM_BIT			BIT(6)
#define IDE_CAP_LIMITED_STREAM_BIT	BIT(24)

#define IDE_CAP_ALGO_SHIFT		8
#define IDE_CAP_ALGO_MASK		GENMASK(12, 8)
#define IDE_CAP_NUM_LINK_SHIFT		13
#define IDE_CAP_NUM_LINK_MASK		GENMASK(15, 13)
#define IDE_CAP_NUM_SEL_SHIFT		16
#define IDE_CAP_NUM_SEL_MASK		GENMASK(23, 16)

#define IDE_CTRL_REG			0x8
#define IDE_DESC_SIZE			12

#define IDE_LINK_REG			0xc
#define IDE_LINK_DESC_SIZE		8

/* offset within the selective stream block starting from SEL CAP */
#define IDE_SEL_CAP_REG			0x0
#define IDE_SEL_CTRL_REG		0x4
#define IDE_SEL_CTRL_REG_STREAMID_MASK	GENMASK(31, 24)
#define IDE_SEL_CTRL_REG_ENABLE_BIT	BIT(0)

#define IDE_SEL_STATUS_REG		0x8

#define IDE_SEL_RID_REG1		0xc
#define IDE_SEL_RID_REG1_LIMIT_MASK	GENMASK(23, 8)

#define IDE_SEL_RID_REG2		0x10
#define IDE_SEL_RID_REG2_BASE_MASK	GENMASK(23, 8)
#define	IDE_SEL_RID_REG2_VALID_BIT	BIT(0)

#define IDE_SEL_DESC_SIZE		20

/* offset within a selective addr association block */
#define IDE_SEL_ADDR_REG1		0x0
#define ADDR_REG1_BASE_LOWER_MASK	GENMASK(19, 8)
#define ADDR_REG1_LIMIT_LOWER_MASK	GENMASK(31, 20)
#define	ADDR_REG1_VALID_BIT		BIT(0)

#define IDE_SEL_ADDR_REG2		0x4

#define IDE_SEL_ADDR_REG3		0x8

#define IDE_SEL_ADDR_DESC_SIZE		12

/* memory address masks */
#define ADDR_START_LOWER_MASK	GENMASK_ULL(31, 20)
#define ADDR_START_UPPER_MASK	GENMASK_ULL(63, 32)

#define ADDR_END_LOWER_MASK	GENMASK_ULL(31, 20)
#define ADDR_END_UPPER_MASK	GENMASK_ULL(63, 32)

struct pci_ide_sel_info {
	unsigned int sel_start;
	unsigned int num_addr;
	atomic_t alloc;
};

struct pci_ide {
	struct pci_ide_info info;
	unsigned int num_sel;
	struct pci_ide_sel_info *sel;
};

static void write_ide_rid_register(struct pci_dev *pdev,
				   struct pci_ide_sel_info *sel,
				   unsigned int rid_start,
				   unsigned int rid_end)
{
	unsigned int val;

	pci_info(pdev, "%s %x-%x\n", __func__, rid_start, rid_end);

	val = FIELD_PREP(IDE_SEL_RID_REG1_LIMIT_MASK, rid_end);
	pci_write_config_dword(pdev, sel->sel_start + IDE_SEL_RID_REG1, val);

	val = FIELD_PREP(IDE_SEL_RID_REG2_BASE_MASK, rid_start);
	val |= IDE_SEL_RID_REG2_VALID_BIT;
	pci_write_config_dword(pdev, sel->sel_start + IDE_SEL_RID_REG2, val);
}

static void write_ide_addr_register(struct pci_dev *pdev,
				    struct pci_ide_sel_info *sel,
				    unsigned int idx,
				    unsigned long addr_start,
				    unsigned long addr_end)
{
	unsigned int val;
	unsigned int off;

	pci_info(pdev, "%s %d:%lx-%lx\n", __func__, idx, addr_start, addr_end);
	off = sel->sel_start + IDE_SEL_DESC_SIZE + idx*IDE_SEL_ADDR_DESC_SIZE;

	val = FIELD_GET(ADDR_START_UPPER_MASK, addr_start);
	pci_write_config_dword(pdev, off + IDE_SEL_ADDR_REG3, val);

	val = FIELD_GET(ADDR_END_UPPER_MASK, addr_end);
	pci_write_config_dword(pdev, off + IDE_SEL_ADDR_REG2, val);

	val = FIELD_PREP(ADDR_REG1_LIMIT_LOWER_MASK,
			FIELD_GET(ADDR_END_LOWER_MASK, addr_end));
	val |= FIELD_PREP(ADDR_REG1_BASE_LOWER_MASK,
			FIELD_GET(ADDR_START_LOWER_MASK, addr_start));
	val |= ADDR_REG1_VALID_BIT;
	pci_write_config_dword(pdev, off + IDE_SEL_ADDR_REG1, val);
}

static void configure_ide_sel_stream(struct pci_dev *pdev,
				  struct pci_ide_sel_info *sel,
				  unsigned int streamid,
				  unsigned int flags)
{
	unsigned int val;

	/* FIXME: add from flags IDE_SEL_CTRL_TEE_LIMIT_STREAM_BIT and others */
	val = FIELD_PREP(IDE_SEL_CTRL_REG_STREAMID_MASK, streamid);
	val |= flags & PCI_IDE_ENABLE_FLAGS_DEFAULT_STREAM;

	pci_write_config_dword(pdev, sel->sel_start + IDE_SEL_CTRL_REG, val);
}

/*
 * Allocate streamids which can range from 0 to 255
 * we are using the idx of the selective stream as streamid
 */
int pcie_ide_sel_streamid_alloc(struct pci_dev *pdev)
{
	struct pci_ide *ide = pdev->ide;
	struct pci_ide_sel_info *sel = NULL;
	int i, old;

	if (ide == NULL)
		return -ENOENT;

	/*
	 * Stream allocation must be unique to the root port, so limit
	 * the allocation for root-port and allow end points to use this
	 * number directly.
	 */
	if (pci_pcie_type(pdev) != PCI_EXP_TYPE_ROOT_PORT)
		return -EINVAL;

	if (!ide->info.sel_ide)
		return -EINVAL;

	for (i = 0; i < ide->num_sel; i++) {
		sel = &ide->sel[i];
		old = atomic_cmpxchg(&sel->alloc, 0, 1);
		if (old == 0)
			break;
	}

	if (i == ide->num_sel)
		return -ENOMEM;

	return i;
}
EXPORT_SYMBOL(pcie_ide_sel_streamid_alloc);

void pcie_ide_sel_streamid_free(struct pci_dev *pdev, unsigned int streamid)
{
	struct pci_ide *ide = pdev->ide;
	struct pci_ide_sel_info *sel = NULL;

	if (!ide)
		return;

	if (pci_pcie_type(pdev) != PCI_EXP_TYPE_ROOT_PORT)
		return;

	if (!ide->info.sel_ide)
		return;

	if (streamid >= ide->num_sel)
		return;

	sel = &ide->sel[streamid];
	atomic_set(&sel->alloc, 0);
}
EXPORT_SYMBOL(pcie_ide_sel_streamid_free);

int pcie_ide_program_rp_stream(struct pci_dev *pdev,
			       u8 streamid,
			       unsigned long rid_start,
			       unsigned long rid_end,
			       struct ide_addr_range *addr,
			       unsigned int naddr,
			       unsigned int flags)
{
	struct pci_ide *ide = pdev->ide;
	struct pci_ide_sel_info *sel = NULL;
	int i;

	pci_info(pdev, "%s sid:%d\n", __func__, streamid);

	ide = pdev->ide;

	if (!ide)
		return -EINVAL;

	if (!ide->info.sel_ide)
		return -EINVAL;

	if (streamid >= ide->num_sel)
		return -EINVAL;

	sel = &ide->sel[streamid];

	if (naddr > sel->num_addr)
		return -EINVAL;

	write_ide_rid_register(pdev, sel, rid_start, rid_end);
	for (i = 0; i < naddr; i++)
		write_ide_addr_register(pdev,
					sel,
					i,
					addr[i].start,
					addr[i].end);

	configure_ide_sel_stream(pdev, sel, streamid, flags);

	return 0;
}
EXPORT_SYMBOL(pcie_ide_program_rp_stream);

int pcie_ide_enable(struct pci_dev *pdev, u8 streamid)
{
	struct pci_ide *ide = pdev->ide;
	struct pci_ide_sel_info *sel = NULL;
	unsigned int val;

	sel = &ide->sel[streamid];

	pci_read_config_dword(pdev, sel->sel_start + IDE_SEL_CTRL_REG, &val);
	val |= IDE_SEL_CTRL_REG_ENABLE_BIT;
	pci_write_config_dword(pdev, sel->sel_start + IDE_SEL_CTRL_REG, val);

	pci_info(pdev, "%s sid:%d\n", __func__, streamid);

	return 0;
}
EXPORT_SYMBOL(pcie_ide_enable);

int pcie_ide_program_ep_stream(struct pci_dev *pdev,
			       u8 streamid,
			       unsigned int flags)
{
	struct pci_ide *ide = pdev->ide;
	struct pci_ide_sel_info *sel = NULL;

	if (streamid >= ide->num_sel)
		return -EINVAL;

	sel = &ide->sel[streamid];

	write_ide_rid_register(pdev, sel, 0, 0xFFFF);
	write_ide_addr_register(pdev, sel, 0, 0, 0xFFFFFFFFFFF00000ULL);

	configure_ide_sel_stream(pdev, sel, streamid, flags);

	pci_info(pdev, "%s sid:%d\n", __func__, streamid);

	return 0;
}
EXPORT_SYMBOL(pcie_ide_program_ep_stream);

void pcie_ide_disable(struct pci_dev *pdev, u8 streamid)
{
	struct pci_ide *ide = pdev->ide;
	struct pci_ide_sel_info *sel = NULL;

	if (streamid >= ide->num_sel)
		return;

	sel = &ide->sel[streamid];

	/* Clearing enable bit transitions stream state to insecure */
	pci_write_config_dword(pdev, sel->sel_start + IDE_SEL_CTRL_REG, 0);

	pci_info(pdev, "%s sid:%d\n", __func__, streamid);
}
EXPORT_SYMBOL(pcie_ide_disable);

struct pci_ide_info *pci_get_ide(struct pci_dev *pdev)
{
	/* It is assumed that a reference of pdev is already taken */
	return &pdev->ide->info;
}
EXPORT_SYMBOL(pci_get_ide);

static int ide_dev_create(struct pci_dev *pdev, u16 ide_start)
{
	struct pci_ide *ide;
	struct pci_ide_info *info;
	u16 num_link = 0;
	u16 sel_next = 0;
	u32 val;
	unsigned int i;
	unsigned int next_cap;

	ide = kzalloc(sizeof(struct pci_ide), GFP_KERNEL);
	if (!ide)
		return -ENOMEM;

	/* Determine the maximum size of the capability */
	pci_read_config_dword(pdev, ide_start, &val);
	next_cap = PCI_EXT_CAP_NEXT(val);
	next_cap = next_cap ? next_cap : 0xfff;

	info = &ide->info;
	pci_read_config_dword(pdev, ide_start + IDE_CAP_REG, &val);
	info->sel_ide = !!(val & IDE_CAP_SEL_BIT);
	info->flow_thru = !!(val & IDE_CAP_FLOW_THRU_BIT);
	info->km_support = !!(val & IDE_CAP_KM_BIT);
	info->limited_stream = !!(val & IDE_CAP_LIMITED_STREAM_BIT);

	if (info->sel_ide)
		ide->num_sel = ((val & IDE_CAP_NUM_SEL_MASK) >>
				IDE_CAP_NUM_SEL_SHIFT) + 1;

	if (!!(val & IDE_CAP_LINK_BIT))
		num_link = ((val & IDE_CAP_NUM_LINK_MASK) >>
			    IDE_CAP_NUM_LINK_SHIFT) + 1;

	if (ide->num_sel)
		ide->sel = kzalloc(ide->num_sel *
				   sizeof(struct pci_ide_sel_info),
				   GFP_KERNEL);

	if (!ide->sel)
		goto err_sel_info;

	pci_info(pdev, "Info: sel_ide:%d flow_thru:%d, km_support: %d num_sel:%d\n",
			info->sel_ide,
			info->flow_thru,
			info->km_support,
			ide->num_sel);

	sel_next = ide_start + IDE_DESC_SIZE + num_link * IDE_LINK_DESC_SIZE;
	for (i = 0; i < ide->num_sel; i++) {
		u16 sel_cur = sel_next;
		unsigned int num_addr;

		pci_read_config_dword(pdev, sel_cur, &val);
		num_addr = val & 0xf;

		sel_next += IDE_SEL_DESC_SIZE + num_addr * IDE_SEL_ADDR_DESC_SIZE;

		/*
		 * The IDE extcap may have reported the number of IDEs incorrectly
		 * (i.e. there is an off-by-one bug). Take this into account by
		 * ensuring that the current field won't overlap with the next
		 * capability.
		 */
		if ((sel_next - 1) >= next_cap) {
			pci_warn(pdev, "IDE implementation defective. Updating sel_ide to %d",
				 i);
			break;
		}

		ide->sel[i].sel_start = sel_cur;
		ide->sel[i].num_addr = val & 0xf;

		pci_info(pdev, " SEL: start:%d num_addr:%d\n",
			 ide->sel[i].sel_start,
			 ide->sel[i].num_addr);

	}

	/*
	 * Update the number of selective IDEs based on the number of
	 * initialized fields
	 */
	ide->num_sel = i;

	pdev->ide = ide;

	return 0;

err_sel_info:
	kfree(ide);
	return -ENOMEM;
}

void pci_ide_init(struct pci_dev *pdev)
{
	u16 offset = 0;

	while ((offset = pci_find_next_ext_capability(pdev, offset,
						      PCI_EXT_CAP_ID_IDE))) {
		pci_info(pdev, "Found IDE cap. @%d\n", offset);
		/* start ide parsing */
		ide_dev_create(pdev, offset);
	}
}

void pci_ide_destroy(struct pci_dev *pdev)
{
	struct pci_ide *ide = pdev->ide;

	if (!ide)
		return;

	if (ide->num_sel)
		kfree(ide->sel);

	kfree(ide);
}
