/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2021 ARM Ltd, All Rights Reserved.
 */

#ifndef LINUX_PCI_IDE_H
#define LINUX_PCI_IDE_H

struct pci_ide_info {
	unsigned int sel_ide:1;
	unsigned int flow_thru:1;
	unsigned int km_support:1;
	unsigned int limited_stream:1;
	unsigned int num_sel;
};

struct ide_addr_range {
	unsigned long start;
	unsigned long end;
};

struct pci_ide_info *pci_get_ide(struct pci_dev *pdev);

static inline bool pci_ide_has_sel_ide(struct pci_ide_info *ide)
{
	return ide->sel_ide;
}

static inline bool pci_ide_has_km(struct pci_ide_info *ide)
{
	return ide->km_support;
}

static inline bool pci_ide_has_flow_thru(struct pci_ide_info *ide)
{
	return ide->flow_thru;
}

int pcie_ide_sel_streamid_alloc(struct pci_dev *pdev);

void pcie_ide_sel_streamid_free(struct pci_dev *pdev, unsigned int streamid);

int pcie_ide_program_rp_stream(struct pci_dev *dev, u8 streamid,
			unsigned long rid_start, unsigned long rid_end,
			struct ide_addr_range *addr, unsigned int naddr,
			unsigned int flags);

int pcie_ide_program_ep_stream(struct pci_dev *dev, u8 streamid, unsigned int flags);

#endif
