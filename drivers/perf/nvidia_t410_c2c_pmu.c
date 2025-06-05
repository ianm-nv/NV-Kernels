// SPDX-License-Identifier: GPL-2.0
/*
 * NVIDIA Tegra410 C2C PMU driver.
 *
 * Copyright (c) 2025, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include <linux/acpi.h>
#include <linux/bitops.h>
#include <linux/cpumask.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/perf_event.h>
#include <linux/platform_device.h>
#include <linux/property.h>

/* The C2C interface types in Tegra410. */
#define C2C_GRS          0x0
#define C2C_UPHY         0x1
#define C2C_LPI          0x2
#define C2C_NUM_TYPES    0x3

#define C2C_GRS_CPU            0x0
#define C2C_GRS_GPU            0x1
#define C2C_GRS_MAX_DEVICES    0x2

#define C2C_GRS_HID     "NVDA2023"
#define C2C_UPHY_HID    "NVDA2022"
#define C2C_LPI_HID     "NVDA2020"

/* Number of instances and counters on each interface. */
#define C2C_GRS_NUM_INSTANCES     14
#define C2C_UPHY_NUM_INSTANCES    12
#define C2C_LPI_NUM_INSTANCES     16
#define C2C_MAX_NUM_INSTANCES     16

/* Register offsets. */
#define C2C_CTRL                    0x864
#define C2C_IN_STATUS               0x868
#define C2C_CYCLE_CNTR              0x86c
#define C2C_IN_RD_CUM_OUTS_CNTR     0x874
#define C2C_IN_RD_REQ_CNTR          0x87c
#define C2C_IN_WR_CUM_OUTS_CNTR     0x884
#define C2C_IN_WR_REQ_CNTR          0x88c
#define C2C_OUT_STATUS              0x890
#define C2C_OUT_RD_CUM_OUTS_CNTR    0x898
#define C2C_OUT_RD_REQ_CNTR         0x8a0
#define C2C_OUT_WR_CUM_OUTS_CNTR    0x8a8
#define C2C_OUT_WR_REQ_CNTR         0x8b0

/* C2C_IN_STATUS register field. */
#define C2C_IN_STATUS_CYCLE_OVF             BIT(0)
#define C2C_IN_STATUS_IN_RD_CUM_OUTS_OVF    BIT(1)
#define C2C_IN_STATUS_IN_RD_REQ_OVF         BIT(2)
#define C2C_IN_STATUS_IN_WR_CUM_OUTS_OVF    BIT(3)
#define C2C_IN_STATUS_IN_WR_REQ_OVF         BIT(4)

/* C2C_OUT_STATUS register field. */
#define C2C_OUT_STATUS_OUT_RD_CUM_OUTS_OVF    BIT(0)
#define C2C_OUT_STATUS_OUT_RD_REQ_OVF         BIT(1)
#define C2C_OUT_STATUS_OUT_WR_CUM_OUTS_OVF    BIT(2)
#define C2C_OUT_STATUS_OUT_WR_REQ_OVF         BIT(3)

/* Events. */
#define C2C_EVENT_CYCLES                0x0
#define C2C_EVENT_IN_RD_CUM_OUTS        0x1
#define C2C_EVENT_IN_RD_REQ             0x2
#define C2C_EVENT_IN_WR_CUM_OUTS        0x3
#define C2C_EVENT_IN_WR_REQ             0x4
#define C2C_EVENT_OUT_RD_CUM_OUTS       0x5
#define C2C_EVENT_OUT_RD_REQ            0x6
#define C2C_EVENT_OUT_WR_CUM_OUTS       0x7
#define C2C_EVENT_OUT_WR_REQ            0x8

#define C2C_NUM_EVENTS           0x9
#define C2C_MASK_EVENT           0xFF
#define C2C_MAX_ACTIVE_EVENTS    32

#define C2C_ACTIVE_CPU_MASK        0x0
#define C2C_ASSOCIATED_CPU_MASK    0x1

/*
 * Maximum poll count for reading counter value using high-low-high sequence.
 */
#define HILOHI_MAX_POLL    1000

static unsigned long nv_c2c_pmu_cpuhp_state;

/* PMU descriptor. */

/* Tracks the events assigned to the PMU for a given logical index. */
struct nv_c2c_pmu_hw_events {
	/* The events that are active. */
	struct perf_event *events[C2C_MAX_ACTIVE_EVENTS];

	/*
	 * Each bit indicates a logical counter is being used (or not) for an
	 * event.
	 */
	DECLARE_BITMAP(used_ctrs, C2C_MAX_ACTIVE_EVENTS);
};

/*
 * The C2C counters can be filtered by instance or group of instances.
 * When filtering by instance, the user can specify filter mask containing the
 * instances to be enabled.
 * When filtering by group, the user can specify filter mask containing the
 * groups to be enabled. Each group is a set of instances.
 */
#define C2C_FILTER_MODE_INSTANCE	0
#define C2C_FILTER_MODE_GROUP		1

struct nv_c2c_pmu {
	struct pmu pmu;
	struct device *dev;
	struct acpi_device *acpi_dev;
	const char *name;
	const char *identifier;

	unsigned int c2c_type;
	unsigned int c2c_subtype;
	unsigned int socket;

	struct nv_c2c_pmu_hw_events hw_events;

	u64 (*read_counter)(struct perf_event *event,
		unsigned long *filter_bitmap);

	/* Number of instances on this C2C interface type. */
	unsigned int num_inst;

	/* Filter mode: C2C_FILTER_MODE_INSTANCE or C2C_FILTER_MODE_GROUP. */
	unsigned int filter_mode;

	/*
	 * The filter size.
	 * Equals to group size if mode is C2C_FILTER_MODE_GROUP.
	 * Equals to instance count if mode is C2C_FILTER_MODE_INSTANCE.
	 */
	unsigned int filter_size;

	/* Default mask for the filter. */
	u32 filter_mask_all;

	/*
	 * Bitmap containing the list of instances for each group.
	 * This is set to NULL if mode is C2C_FILTER_MODE_INSTANCE.
	 */
	unsigned long **filter_inst_mask;

	cpumask_t associated_cpus;
	cpumask_t active_cpu;

	struct hlist_node cpuhp_node;

	struct attribute **formats;
	const struct attribute_group *attr_groups[5];

	void __iomem *base_broadcast;
	void __iomem *base[C2C_MAX_NUM_INSTANCES];
};

#define to_c2c_pmu(p) (container_of(p, struct nv_c2c_pmu, pmu))

/* Get event type from perf_event. */
static inline u32 get_event_type(struct perf_event *event)
{
	return (event->attr.config) & C2C_MASK_EVENT;
}

static inline u32 get_filter_mask(struct perf_event *event)
{
	u32 filter;
	struct nv_c2c_pmu *c2c_pmu = to_c2c_pmu(event->pmu);

	filter = ((u32)event->attr.config1) & c2c_pmu->filter_mask_all;
	if (filter == 0)
		filter = c2c_pmu->filter_mask_all;

	return filter;
}

/* PMU operations. */

static int nv_c2c_pmu_get_event_idx(struct nv_c2c_pmu_hw_events *hw_events,
				    struct perf_event *event)
{
	u32 idx = get_event_type(event);

	idx = find_first_zero_bit(hw_events->used_ctrs, C2C_MAX_ACTIVE_EVENTS);
	if (idx >= C2C_MAX_ACTIVE_EVENTS)
		return -EAGAIN;

	set_bit(idx, hw_events->used_ctrs);

	return idx;
}

static bool
nv_c2c_pmu_validate_event(struct pmu *pmu,
			  struct nv_c2c_pmu_hw_events *hw_events,
			  struct perf_event *event)
{
	if (is_software_event(event))
		return true;

	/* Reject groups spanning multiple HW PMUs. */
	if (event->pmu != pmu)
		return false;

	return nv_c2c_pmu_get_event_idx(hw_events, event) >= 0;
}

/*
 * Make sure the group of events can be scheduled at once
 * on the PMU.
 */
static bool nv_c2c_pmu_validate_group(struct perf_event *event)
{
	struct perf_event *sibling, *leader = event->group_leader;
	struct nv_c2c_pmu_hw_events fake_hw_events;

	if (event->group_leader == event)
		return true;

	memset(&fake_hw_events, 0, sizeof(fake_hw_events));

	if (!nv_c2c_pmu_validate_event(event->pmu, &fake_hw_events, leader))
		return false;

	for_each_sibling_event(sibling, leader) {
		if (!nv_c2c_pmu_validate_event(event->pmu, &fake_hw_events,
					       sibling))
			return false;
	}

	return nv_c2c_pmu_validate_event(event->pmu, &fake_hw_events, event);
}

static int nv_c2c_pmu_event_init(struct perf_event *event)
{
	struct nv_c2c_pmu *c2c_pmu = to_c2c_pmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	u32 event_type = get_event_type(event);

	if (event->attr.type != event->pmu->type ||
	    event_type >= C2C_NUM_EVENTS)
		return -ENOENT;

	/*
	 * Following other "uncore" PMUs, we do not support sampling mode or
	 * attach to a task (per-process mode).
	 */
	if (is_sampling_event(event)) {
		dev_dbg(c2c_pmu->pmu.dev, "Can't support sampling events\n");
		return -EOPNOTSUPP;
	}

	if (event->cpu < 0 || event->attach_state & PERF_ATTACH_TASK) {
		dev_dbg(c2c_pmu->pmu.dev, "Can't support per-task counters\n");
		return -EINVAL;
	}

	/*
	 * Make sure the CPU assignment is on one of the CPUs associated with
	 * this PMU.
	 */
	if (!cpumask_test_cpu(event->cpu, &c2c_pmu->associated_cpus)) {
		dev_dbg(c2c_pmu->pmu.dev,
			"Requested cpu is not associated with the PMU\n");
		return -EINVAL;
	}

	/* Enforce the current active CPU to handle the events in this PMU. */
	event->cpu = cpumask_first(&c2c_pmu->active_cpu);
	if (event->cpu >= nr_cpu_ids)
		return -EINVAL;

	if (!nv_c2c_pmu_validate_group(event))
		return -EINVAL;

	hwc->idx = -1;
	hwc->config = event_type;

	return 0;
}

/*
 * Read 64-bit register as a pair of 32-bit registers using hi-lo-hi sequence.
 */
static u64 read_reg64_hilohi(const void __iomem *addr, u32 max_poll_count)
{
	u32 val_lo, val_hi;
	u64 val;

	/* Use high-low-high sequence to avoid tearing */
	do {
		if (max_poll_count-- == 0) {
			pr_err("NV C2C PMU: timeout hi-low-high sequence\n");
			return 0;
		}

		val_hi = readl(addr + 4);
		val_lo = readl(addr);
	} while (val_hi != readl(addr + 4));

	val = (((u64)val_hi << 32) | val_lo);

	return val;
}

static void nv_c2c_pmu_check_status(struct nv_c2c_pmu *c2c_pmu, u32 instance)
{
	u32 in_status, out_status;

	in_status = readl(c2c_pmu->base[instance] + C2C_IN_STATUS);
	out_status = readl(c2c_pmu->base[instance] + C2C_OUT_STATUS);

	if (in_status || out_status)
		dev_warn(c2c_pmu->dev,
			"C2C PMU overflow in: 0x%x, out: 0x%x\n",
			in_status, out_status);
}

static u32 nv_c2c_ctr_offset[C2C_NUM_EVENTS] = {
	[C2C_EVENT_CYCLES] = C2C_CYCLE_CNTR,
	[C2C_EVENT_IN_RD_CUM_OUTS] = C2C_IN_RD_CUM_OUTS_CNTR,
	[C2C_EVENT_IN_RD_REQ] = C2C_IN_RD_REQ_CNTR,
	[C2C_EVENT_IN_WR_CUM_OUTS] = C2C_IN_WR_CUM_OUTS_CNTR,
	[C2C_EVENT_IN_WR_REQ] = C2C_IN_WR_REQ_CNTR,
	[C2C_EVENT_OUT_RD_CUM_OUTS] = C2C_OUT_RD_CUM_OUTS_CNTR,
	[C2C_EVENT_OUT_RD_REQ] = C2C_OUT_RD_REQ_CNTR,
	[C2C_EVENT_OUT_WR_CUM_OUTS] = C2C_OUT_WR_CUM_OUTS_CNTR,
	[C2C_EVENT_OUT_WR_REQ] = C2C_OUT_WR_REQ_CNTR,
};

static u64 nv_c2c_pmu_read_counter_mode_group(struct perf_event *event,
					      unsigned long *filter_bitmap)
{
	unsigned int filter_idx, inst_idx;
	u32 ctr_id, ctr_offset;
	u64 val = 0;
	unsigned long *inst_mask;
	struct nv_c2c_pmu *c2c_pmu = to_c2c_pmu(event->pmu);

	ctr_id = event->hw.config;
	ctr_offset = nv_c2c_ctr_offset[ctr_id];

	for_each_set_bit(filter_idx, filter_bitmap, c2c_pmu->filter_size) {
		inst_mask = c2c_pmu->filter_inst_mask[filter_idx];
		for_each_set_bit(inst_idx, inst_mask, c2c_pmu->num_inst) {
			nv_c2c_pmu_check_status(c2c_pmu, inst_idx);

			/*
			 * Each instance share same clock and the driver always
			 * enables all instances. So we can use the counts from
			 * one instance for cycle counter.
			 */
			if (ctr_id == C2C_EVENT_CYCLES)
				return read_reg64_hilohi(
					c2c_pmu->base[inst_idx] + ctr_offset,
					HILOHI_MAX_POLL);

			/*
			 * For other events, sum up the counts from all instances.
			 */
			val += read_reg64_hilohi(
				c2c_pmu->base[inst_idx] + ctr_offset,
				HILOHI_MAX_POLL);
		}
	}

	return val;
}

static u64 nv_c2c_pmu_read_counter_mode_instance(struct perf_event *event,
						 unsigned long *filter_bitmap)
{
	unsigned int filter_idx;
	u32 ctr_id, ctr_offset;
	u64 val = 0;
	struct nv_c2c_pmu *c2c_pmu = to_c2c_pmu(event->pmu);

	ctr_id = event->hw.config;
	ctr_offset = nv_c2c_ctr_offset[ctr_id];

	for_each_set_bit(filter_idx, filter_bitmap, c2c_pmu->filter_size) {
		nv_c2c_pmu_check_status(c2c_pmu, filter_idx);

		/*
		 * Each instance share same clock and the driver always
		 * enables all instances. So we can use the counts from
		 * one instance for cycle counter.
		 */
		if (ctr_id == C2C_EVENT_CYCLES)
			return read_reg64_hilohi(
				c2c_pmu->base[filter_idx] + ctr_offset,
				HILOHI_MAX_POLL);

		/* For other events, sum up the counts from all instances. */
		val += read_reg64_hilohi(
			c2c_pmu->base[filter_idx] + ctr_offset,
			HILOHI_MAX_POLL);
	}

	return val;
}

static u64 nv_c2c_pmu_read_counter(struct perf_event *event)
{
	u32 filter_mask;
	DECLARE_BITMAP(filter_bitmap, C2C_MAX_NUM_INSTANCES);
	struct nv_c2c_pmu *c2c_pmu = to_c2c_pmu(event->pmu);

	filter_mask = get_filter_mask(event);
	bitmap_from_arr32(filter_bitmap, &filter_mask, c2c_pmu->filter_size);

	return c2c_pmu->read_counter(event, filter_bitmap);
}

static void nv_c2c_pmu_event_update(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;
	u64 prev, now;

	do {
		prev = local64_read(&hwc->prev_count);
		now = nv_c2c_pmu_read_counter(event);
	} while (local64_cmpxchg(&hwc->prev_count, prev, now) != prev);

	local64_add(now - prev, &event->count);
}

static void nv_c2c_pmu_start(struct perf_event *event, int pmu_flags)
{
	event->hw.state = 0;
}

static void nv_c2c_pmu_stop(struct perf_event *event, int pmu_flags)
{
	event->hw.state |= PERF_HES_STOPPED;
}

static int nv_c2c_pmu_add(struct perf_event *event, int flags)
{
	struct nv_c2c_pmu *c2c_pmu = to_c2c_pmu(event->pmu);
	struct nv_c2c_pmu_hw_events *hw_events = &c2c_pmu->hw_events;
	struct hw_perf_event *hwc = &event->hw;
	int idx;

	if (WARN_ON_ONCE(!cpumask_test_cpu(smp_processor_id(),
					   &c2c_pmu->associated_cpus)))
		return -ENOENT;

	idx = nv_c2c_pmu_get_event_idx(hw_events, event);
	if (idx < 0)
		return idx;

	hw_events->events[idx] = event;
	hwc->idx = idx;
	hwc->state = PERF_HES_STOPPED | PERF_HES_UPTODATE;

	if (flags & PERF_EF_START)
		nv_c2c_pmu_start(event, PERF_EF_RELOAD);

	/* Propagate changes to the userspace mapping. */
	perf_event_update_userpage(event);

	return 0;
}

static void nv_c2c_pmu_del(struct perf_event *event, int flags)
{
	struct nv_c2c_pmu *c2c_pmu = to_c2c_pmu(event->pmu);
	struct nv_c2c_pmu_hw_events *hw_events = &c2c_pmu->hw_events;
	struct hw_perf_event *hwc = &event->hw;
	int idx = hwc->idx;

	nv_c2c_pmu_stop(event, PERF_EF_UPDATE);

	hw_events->events[idx] = NULL;

	clear_bit(idx, hw_events->used_ctrs);

	perf_event_update_userpage(event);
}

static void nv_c2c_pmu_read(struct perf_event *event)
{
	nv_c2c_pmu_event_update(event);
}

static void nv_c2c_pmu_enable(struct pmu *pmu)
{
	void __iomem *bcast;
	struct nv_c2c_pmu *c2c_pmu = to_c2c_pmu(pmu);

	/* Check if any filter is enabled. */
	if (bitmap_empty(c2c_pmu->hw_events.used_ctrs, C2C_MAX_ACTIVE_EVENTS))
		return;

	/* Enable all the counters. */
	bcast = c2c_pmu->base_broadcast;
	writel(0x1UL, bcast + C2C_CTRL);
}

static void nv_c2c_pmu_disable(struct pmu *pmu)
{
	unsigned int idx;
	void __iomem *bcast;
	struct perf_event *event;
	struct nv_c2c_pmu *c2c_pmu = to_c2c_pmu(pmu);

	/* Disable all the counters. */
	bcast = c2c_pmu->base_broadcast;
	writel(0x0UL, bcast + C2C_CTRL);

	/*
	 * The counters will start from 0 again on restart.
	 * Update the events immediately to avoid losing the counts.
	 */
	for_each_set_bit(idx, c2c_pmu->hw_events.used_ctrs,
			 C2C_MAX_ACTIVE_EVENTS) {
		event = c2c_pmu->hw_events.events[idx];

		if (!event)
			continue;

		nv_c2c_pmu_event_update(event);

		local64_set(&event->hw.prev_count, 0ULL);
	}
}

/* PMU identifier attribute. */

static ssize_t nv_c2c_pmu_identifier_show(struct device *dev,
					  struct device_attribute *attr,
					  char *page)
{
	struct nv_c2c_pmu *nv_c2c_pmu = to_c2c_pmu(dev_get_drvdata(dev));

	return sysfs_emit(page, "%s\n", nv_c2c_pmu->identifier);
}

static struct device_attribute nv_c2c_pmu_identifier_attr =
	__ATTR(identifier, 0444, nv_c2c_pmu_identifier_show, NULL);

static struct attribute *nv_c2c_pmu_identifier_attrs[] = {
	&nv_c2c_pmu_identifier_attr.attr,
	NULL,
};

static struct attribute_group nv_c2c_pmu_identifier_attr_group = {
	.attrs = nv_c2c_pmu_identifier_attrs,
};

/* Format attributes. */

#define NV_C2C_PMU_EXT_ATTR(_name, _func, _config)			\
	(&((struct dev_ext_attribute[]){				\
		{							\
			.attr = __ATTR(_name, 0444, _func, NULL),	\
			.var = (void *)_config				\
		}							\
	})[0].attr.attr)

#define NV_C2C_PMU_FORMAT_ATTR(_name, _config) \
	NV_C2C_PMU_EXT_ATTR(_name, device_show_string, _config)

#define NV_C2C_PMU_FORMAT_EVENT_ATTR \
	NV_C2C_PMU_FORMAT_ATTR(event, "config:0-3")

static struct attribute *nv_c2c_grs_cg2_pmu_formats[] = {
	NV_C2C_PMU_FORMAT_EVENT_ATTR,
	NV_C2C_PMU_FORMAT_ATTR(gpu, "config1:0-1"),
	NULL,
};

static struct attribute *nv_c2c_uphy_pmu_formats[] = {
	NV_C2C_PMU_FORMAT_EVENT_ATTR,
	NV_C2C_PMU_FORMAT_ATTR(instance, "config1:0-11"),
	NULL,
};

static struct attribute *nv_c2c_generic_pmu_formats[] = {
	NV_C2C_PMU_FORMAT_EVENT_ATTR,
	NULL,
};

static struct attribute_group *
nv_c2c_pmu_alloc_format_attr_group(struct nv_c2c_pmu *c2c_pmu)
{
	struct attribute_group *format_group;
	struct device *dev = c2c_pmu->dev;

	format_group =
		devm_kzalloc(dev, sizeof(struct attribute_group), GFP_KERNEL);
	if (!format_group)
		return NULL;

	format_group->name = "format";
	format_group->attrs = c2c_pmu->formats;

	return format_group;
}

/* Event attributes. */

static ssize_t nv_c2c_pmu_sysfs_event_show(struct device *dev,
					   struct device_attribute *attr,
					   char *buf)
{
	struct perf_pmu_events_attr *pmu_attr;

	pmu_attr = container_of(attr, typeof(*pmu_attr), attr);
	return sysfs_emit(buf, "event=0x%llx\n", pmu_attr->id);
}

#define NV_C2C_PMU_EVENT_ATTR(_name, _config)	\
	PMU_EVENT_ATTR_ID(_name, nv_c2c_pmu_sysfs_event_show, _config)

static struct attribute *nv_c2c_pmu_events[] = {
	NV_C2C_PMU_EVENT_ATTR(cycles, C2C_EVENT_CYCLES),
	NV_C2C_PMU_EVENT_ATTR(in_rd_cum_outs, C2C_EVENT_IN_RD_CUM_OUTS),
	NV_C2C_PMU_EVENT_ATTR(in_rd_req, C2C_EVENT_IN_RD_REQ),
	NV_C2C_PMU_EVENT_ATTR(in_wr_cum_outs, C2C_EVENT_IN_WR_CUM_OUTS),
	NV_C2C_PMU_EVENT_ATTR(in_wr_req, C2C_EVENT_IN_WR_REQ),
	NV_C2C_PMU_EVENT_ATTR(out_rd_cum_outs, C2C_EVENT_OUT_RD_CUM_OUTS),
	NV_C2C_PMU_EVENT_ATTR(out_rd_req, C2C_EVENT_OUT_RD_REQ),
	NV_C2C_PMU_EVENT_ATTR(out_wr_cum_outs, C2C_EVENT_OUT_WR_CUM_OUTS),
	NV_C2C_PMU_EVENT_ATTR(out_wr_req, C2C_EVENT_OUT_WR_REQ),
	NULL
};

static umode_t
nv_c2c_pmu_event_attr_is_visible(struct kobject *kobj, struct attribute *attr,
				 int unused)
{
	struct device *dev = kobj_to_dev(kobj);
	struct nv_c2c_pmu *c2c_pmu = to_c2c_pmu(dev_get_drvdata(dev));
	struct perf_pmu_events_attr *eattr;

	eattr = container_of(attr, typeof(*eattr), attr.attr);

	if (c2c_pmu->c2c_type == C2C_GRS) {
		/* Hide the write events if GRS connected to another SoC. */
		if (c2c_pmu->c2c_subtype == C2C_GRS_CPU) {
			switch (eattr->id) {
			case C2C_EVENT_IN_WR_CUM_OUTS:
			case C2C_EVENT_IN_WR_REQ:
			case C2C_EVENT_OUT_WR_CUM_OUTS:
			case C2C_EVENT_OUT_WR_REQ:
				return 0;
			default:
				return attr->mode;
			}
		}
	} else if (c2c_pmu->c2c_type == C2C_LPI) {
		/* Only incoming reads are available. */
		switch (eattr->id) {
		case C2C_EVENT_IN_WR_CUM_OUTS:
		case C2C_EVENT_IN_WR_REQ:
		case C2C_EVENT_OUT_RD_CUM_OUTS:
		case C2C_EVENT_OUT_RD_REQ:
		case C2C_EVENT_OUT_WR_CUM_OUTS:
		case C2C_EVENT_OUT_WR_REQ:
			return 0;
		default:
			return attr->mode;
		}
	}

	return attr->mode;
}

static const struct attribute_group nv_c2c_pmu_events_group = {
	.name = "events",
	.attrs = nv_c2c_pmu_events,
	.is_visible = nv_c2c_pmu_event_attr_is_visible,
};

/* Cpumask attributes. */

static ssize_t nv_c2c_pmu_cpumask_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct pmu *pmu = dev_get_drvdata(dev);
	struct nv_c2c_pmu *c2c_pmu = to_c2c_pmu(pmu);
	struct dev_ext_attribute *eattr =
		container_of(attr, struct dev_ext_attribute, attr);
	unsigned long mask_id = (unsigned long)eattr->var;
	const cpumask_t *cpumask;

	switch (mask_id) {
	case C2C_ACTIVE_CPU_MASK:
		cpumask = &c2c_pmu->active_cpu;
		break;
	case C2C_ASSOCIATED_CPU_MASK:
		cpumask = &c2c_pmu->associated_cpus;
		break;
	default:
		return 0;
	}
	return cpumap_print_to_pagebuf(true, buf, cpumask);
}

#define NV_C2C_PMU_CPUMASK_ATTR(_name, _config)			\
	NV_C2C_PMU_EXT_ATTR(_name, nv_c2c_pmu_cpumask_show,	\
				(unsigned long)_config)

static struct attribute *nv_c2c_pmu_cpumask_attrs[] = {
	NV_C2C_PMU_CPUMASK_ATTR(cpumask, C2C_ACTIVE_CPU_MASK),
	NV_C2C_PMU_CPUMASK_ATTR(associated_cpus, C2C_ASSOCIATED_CPU_MASK),
	NULL,
};

static const struct attribute_group nv_c2c_pmu_cpumask_attr_group = {
	.attrs = nv_c2c_pmu_cpumask_attrs,
};

/* Per PMU device attribute groups. */

static int nv_c2c_pmu_alloc_attr_groups(struct nv_c2c_pmu *c2c_pmu)
{
	const struct attribute_group **attr_groups = c2c_pmu->attr_groups;

	attr_groups[0] = nv_c2c_pmu_alloc_format_attr_group(c2c_pmu);
	attr_groups[1] = &nv_c2c_pmu_events_group;
	attr_groups[2] = &nv_c2c_pmu_cpumask_attr_group;
	attr_groups[3] = &nv_c2c_pmu_identifier_attr_group;

	if (!attr_groups[0])
		return -ENOMEM;

	return 0;
}

static int nv_c2c_pmu_online_cpu(unsigned int cpu, struct hlist_node *node)
{
	struct nv_c2c_pmu *c2c_pmu =
		hlist_entry_safe(node, struct nv_c2c_pmu, cpuhp_node);

	if (!cpumask_test_cpu(cpu, &c2c_pmu->associated_cpus))
		return 0;

	/* If the PMU is already managed, there is nothing to do */
	if (!cpumask_empty(&c2c_pmu->active_cpu))
		return 0;

	/* Use this CPU for event counting */
	cpumask_set_cpu(cpu, &c2c_pmu->active_cpu);

	return 0;
}

static int nv_c2c_pmu_cpu_teardown(unsigned int cpu, struct hlist_node *node)
{
	unsigned int dst;

	struct nv_c2c_pmu *c2c_pmu =
		hlist_entry_safe(node, struct nv_c2c_pmu, cpuhp_node);

	/* Nothing to do if this CPU doesn't own the PMU */
	if (!cpumask_test_and_clear_cpu(cpu, &c2c_pmu->active_cpu))
		return 0;

	/* Choose a new CPU to migrate ownership of the PMU to */
	dst = cpumask_any_and_but(&c2c_pmu->associated_cpus,
				  cpu_online_mask, cpu);
	if (dst >= nr_cpu_ids)
		return 0;

	/* Use this CPU for event counting */
	perf_pmu_migrate_context(&c2c_pmu->pmu, cpu, dst);
	cpumask_set_cpu(dst, &c2c_pmu->active_cpu);

	return 0;
}

static int nv_c2c_pmu_get_cpus(struct nv_c2c_pmu *c2c_pmu)
{
	int ret = 0, socket = c2c_pmu->socket, cpu;

	for_each_possible_cpu(cpu) {
		if (cpu_to_node(cpu) == socket)
			cpumask_set_cpu(cpu, &c2c_pmu->associated_cpus);
	}

	if (cpumask_empty(&c2c_pmu->associated_cpus)) {
		dev_dbg(c2c_pmu->dev,
			"No cpu associated with C2C PMU socket-%u\n", socket);
		ret = -ENODEV;
	}

	return ret;
}

static int nv_c2c_pmu_init_socket(struct nv_c2c_pmu *c2c_pmu)
{
	const char *uid_str;
	int ret, socket;

	uid_str = acpi_device_uid(c2c_pmu->acpi_dev);
	if (!uid_str) {
		ret = -ENODEV;
		goto fail;
	}

	ret = kstrtou32(uid_str, 0, &socket);
	if (ret)
		goto fail;

	c2c_pmu->socket = socket;
	return 0;

fail:
	dev_err(c2c_pmu->dev, "Failed to initialize socket\n");
	return ret;
}

static const char *nv_c2c_pmu_name_pattern[C2C_NUM_TYPES] = {
	[C2C_GRS] = "nvidia_nvlink_c2c_pmu_%u",
	[C2C_UPHY] = "nvidia_nvclink_pmu_%u",
	[C2C_LPI] = "nvidia_nvdlink_pmu_%u",
};

static int nv_c2c_pmu_init_name(struct nv_c2c_pmu *c2c_pmu)
{
	char *name;
	int ret;

	name = devm_kasprintf(c2c_pmu->dev, GFP_KERNEL,
		nv_c2c_pmu_name_pattern[c2c_pmu->c2c_type], c2c_pmu->socket);
	if (!name) {
		ret = -ENOMEM;
		goto fail;
	}

	c2c_pmu->name = name;
	return 0;

fail:
	dev_err(c2c_pmu->dev, "Failed to initialize name\n");
	return ret;
}

static int nv_c2c_pmu_alloc_filter_inst_mask(struct nv_c2c_pmu *c2c_pmu,
					     unsigned int filter_size)
{
	int i, ret;
	unsigned long **filter_inst_mask;

	if (c2c_pmu->filter_mode != C2C_FILTER_MODE_GROUP)
		return 0;

	filter_inst_mask = devm_kcalloc(c2c_pmu->dev, filter_size,
				sizeof(*filter_inst_mask), GFP_KERNEL);
	if (!filter_inst_mask) {
		ret = -ENOMEM;
		goto fail;
	}

	for (i = 0; i < filter_size; i++) {
		filter_inst_mask[i] = devm_kcalloc(c2c_pmu->dev,
					BITS_TO_LONGS(c2c_pmu->num_inst),
					sizeof(*filter_inst_mask[i]),
					GFP_KERNEL);
		if (!filter_inst_mask[i]) {
			ret = -ENOMEM;
			goto fail;
		}
	}

	c2c_pmu->filter_inst_mask = filter_inst_mask;

	return 0;

fail:
	dev_err(c2c_pmu->dev, "Failed to initialize filter\n");
	return ret;
}

static int nv_c2c_pmu_init_grs(struct nv_c2c_pmu *c2c_pmu)
{
	int ret;
	u32 cpu_en;
	struct device *dev;
	const char *id_str, *subtype_str;

	dev = c2c_pmu->dev;

	c2c_pmu->num_inst = C2C_GRS_NUM_INSTANCES;
	c2c_pmu->filter_mode = C2C_FILTER_MODE_GROUP;
	c2c_pmu->read_counter = nv_c2c_pmu_read_counter_mode_group;

	/* Allocate instance bitmaps for maximum number of connected devices. */
	ret = nv_c2c_pmu_alloc_filter_inst_mask(c2c_pmu, C2C_GRS_MAX_DEVICES);
	if (ret)
		return ret;

	/*
	 * Allow user to filter per instance group if connected to GPU,
	 * otherwise configure this PMU with one filter group containing all
	 * instances.
	 */

	ret = device_property_read_u32(dev, "cpu_en_mask", &cpu_en);
	if (ret) {
		dev_err(dev, "Failed to read cpu_en_mask\n");
		return ret;
	}

	if (cpu_en) {
		c2c_pmu->c2c_subtype = C2C_GRS_CPU;
		c2c_pmu->filter_size = 1;

		bitmap_from_arr32(c2c_pmu->filter_inst_mask[0], &cpu_en,
				  c2c_pmu->num_inst);
	} else {
		u32 i, gpu_en;
		const char *props[C2C_GRS_MAX_DEVICES] = {
			"gpu0_en_mask", "gpu1_en_mask"
		};

		c2c_pmu->c2c_subtype = C2C_GRS_GPU;

		for (i = 0; i < C2C_GRS_MAX_DEVICES; i++) {
			ret = device_property_read_u32(dev, props[i], &gpu_en);
			if (ret) {
				dev_err(dev, "Failed to read %s\n", props[i]);
				return ret;
			}

			if (gpu_en) {
				c2c_pmu->filter_size++;
				bitmap_from_arr32(c2c_pmu->filter_inst_mask[i],
						  &gpu_en, c2c_pmu->num_inst);
			}
		}

		if (c2c_pmu->filter_size == 0) {
			dev_err(dev, "No GPU is enabled\n");
			return -EINVAL;
		}
	}

	c2c_pmu->filter_mask_all = (1 << c2c_pmu->filter_size) - 1;

	if (c2c_pmu->filter_size == 1)
		c2c_pmu->formats = nv_c2c_generic_pmu_formats;
	else
		c2c_pmu->formats = nv_c2c_grs_cg2_pmu_formats;

	subtype_str = (c2c_pmu->c2c_subtype == C2C_GRS_CPU) ? "cpu" : "gpu";
	id_str = devm_kasprintf(dev, GFP_KERNEL, "%s.%s",
				acpi_device_hid(c2c_pmu->acpi_dev), subtype_str);
	if (!id_str) {
		dev_err(dev, "Failed to initialize identifier\n");
		return -ENOMEM;
	}

	c2c_pmu->identifier = id_str;

	return 0;
}

static int nv_c2c_pmu_init_uphy(struct nv_c2c_pmu *c2c_pmu)
{
	c2c_pmu->identifier = acpi_device_hid(c2c_pmu->acpi_dev);

	/* Allow user to filter per instance.*/

	c2c_pmu->num_inst = C2C_UPHY_NUM_INSTANCES;
	c2c_pmu->filter_mode = C2C_FILTER_MODE_INSTANCE;
	c2c_pmu->filter_size = c2c_pmu->num_inst;
	c2c_pmu->filter_mask_all = (1 << c2c_pmu->filter_size) - 1;
	c2c_pmu->read_counter = nv_c2c_pmu_read_counter_mode_instance;

	c2c_pmu->formats = nv_c2c_uphy_pmu_formats;

	return 0;
}

static int nv_c2c_pmu_init_lpi(struct nv_c2c_pmu *c2c_pmu)
{
	int ret;

	c2c_pmu->identifier = acpi_device_hid(c2c_pmu->acpi_dev);

	/*
	 * Per instance filtering has no meaning to user. Hence configure this
	 * PMU with one filter group containing all instances.
	 */

	c2c_pmu->num_inst = C2C_LPI_NUM_INSTANCES;
	c2c_pmu->filter_mode = C2C_FILTER_MODE_GROUP;
	c2c_pmu->filter_size = 1;
	c2c_pmu->filter_mask_all = (1 << c2c_pmu->filter_size) - 1;
	c2c_pmu->read_counter = nv_c2c_pmu_read_counter_mode_group;

	ret = nv_c2c_pmu_alloc_filter_inst_mask(c2c_pmu, c2c_pmu->filter_size);
	if (ret)
		return ret;

	*c2c_pmu->filter_inst_mask[0] = (1UL << c2c_pmu->num_inst) - 1;

	c2c_pmu->formats = nv_c2c_generic_pmu_formats;

	return 0;
}

static void *nv_c2c_pmu_init_pmu(struct platform_device *pdev)
{
	int ret;
	struct nv_c2c_pmu *c2c_pmu;
	struct acpi_device *acpi_dev;
	struct device *dev = &pdev->dev;

	acpi_dev = ACPI_COMPANION(dev);
	if (!acpi_dev)
		return ERR_PTR(-ENODEV);

	c2c_pmu = devm_kzalloc(dev, sizeof(*c2c_pmu), GFP_KERNEL);
	if (!c2c_pmu)
		return ERR_PTR(-ENOMEM);

	c2c_pmu->dev = dev;
	c2c_pmu->acpi_dev = acpi_dev;
	c2c_pmu->c2c_type = (unsigned int)device_get_match_data(dev);
	platform_set_drvdata(pdev, c2c_pmu);

	ret = nv_c2c_pmu_init_socket(c2c_pmu);
	if (ret)
		goto done;

	ret = nv_c2c_pmu_init_name(c2c_pmu);
	if (ret)
		goto done;

	if (c2c_pmu->c2c_type == C2C_GRS)
		ret = nv_c2c_pmu_init_grs(c2c_pmu);
	else if (c2c_pmu->c2c_type == C2C_UPHY)
		ret = nv_c2c_pmu_init_uphy(c2c_pmu);
	else if (c2c_pmu->c2c_type == C2C_LPI)
		ret = nv_c2c_pmu_init_lpi(c2c_pmu);

done:
	if (ret)
		return ERR_PTR(ret);

	return c2c_pmu;
}

static int nv_c2c_pmu_init_mmio(struct nv_c2c_pmu *c2c_pmu)
{
	int i;
	struct device *dev = c2c_pmu->dev;
	struct platform_device *pdev = to_platform_device(dev);

	/* Map the address of all the instances. */
	for (i = 0; i < c2c_pmu->num_inst; i++) {
		c2c_pmu->base[i] = devm_platform_ioremap_resource(pdev, i);
		if (IS_ERR(c2c_pmu->base[i])) {
			dev_err(dev, "Failed map address for instance %d\n", i);
			return PTR_ERR(c2c_pmu->base[i]);
		}
	}

	/* Map broadcast address. */
	c2c_pmu->base_broadcast = devm_platform_ioremap_resource(pdev,
								 c2c_pmu->num_inst);
	if (IS_ERR(c2c_pmu->base_broadcast)) {
		dev_err(dev, "Failed map broadcast address\n");
		return PTR_ERR(c2c_pmu->base_broadcast);
	}

	return 0;
}

static int nv_c2c_pmu_register_pmu(struct nv_c2c_pmu *c2c_pmu)
{
	int ret;

	ret = cpuhp_state_add_instance(nv_c2c_pmu_cpuhp_state,
				       &c2c_pmu->cpuhp_node);
	if (ret) {
		dev_err(c2c_pmu->dev, "Error %d registering hotplug\n", ret);
		return ret;
	}

	c2c_pmu->pmu = (struct pmu) {
		.parent		= c2c_pmu->dev,
		.task_ctx_nr	= perf_invalid_context,
		.pmu_enable	= nv_c2c_pmu_enable,
		.pmu_disable	= nv_c2c_pmu_disable,
		.event_init	= nv_c2c_pmu_event_init,
		.add		= nv_c2c_pmu_add,
		.del		= nv_c2c_pmu_del,
		.start		= nv_c2c_pmu_start,
		.stop		= nv_c2c_pmu_stop,
		.read		= nv_c2c_pmu_read,
		.attr_groups	= c2c_pmu->attr_groups,
		.capabilities	= PERF_PMU_CAP_NO_EXCLUDE |
					PERF_PMU_CAP_NO_INTERRUPT,
	};

	ret = perf_pmu_register(&c2c_pmu->pmu, c2c_pmu->name, -1);
	if (ret) {
		dev_err(c2c_pmu->dev, "Failed to register C2C PMU: %d\n", ret);
		cpuhp_state_remove_instance(nv_c2c_pmu_cpuhp_state,
					  &c2c_pmu->cpuhp_node);
		return ret;
	}

	return 0;
}

static int nv_c2c_pmu_probe(struct platform_device *pdev)
{
	int ret;
	struct nv_c2c_pmu *c2c_pmu;

	c2c_pmu = nv_c2c_pmu_init_pmu(pdev);
	if (IS_ERR(c2c_pmu))
		return PTR_ERR(c2c_pmu);

	ret = nv_c2c_pmu_init_mmio(c2c_pmu);
	if (ret)
		return ret;

	ret = nv_c2c_pmu_get_cpus(c2c_pmu);
	if (ret)
		return ret;

	ret = nv_c2c_pmu_alloc_attr_groups(c2c_pmu);
	if (ret)
		return ret;

	ret = nv_c2c_pmu_register_pmu(c2c_pmu);
	if (ret)
		return ret;

	dev_dbg(c2c_pmu->dev, "Registered %s PMU\n", c2c_pmu->name);

	return 0;
}

static void nv_c2c_pmu_device_remove(struct platform_device *pdev)
{
	struct nv_c2c_pmu *c2c_pmu = platform_get_drvdata(pdev);

	perf_pmu_unregister(&c2c_pmu->pmu);
	cpuhp_state_remove_instance(nv_c2c_pmu_cpuhp_state, &c2c_pmu->cpuhp_node);
}

static const struct acpi_device_id nv_c2c_pmu_acpi_match[] = {
	{ C2C_GRS_HID, (kernel_ulong_t)C2C_GRS },
	{ C2C_UPHY_HID, (kernel_ulong_t)C2C_UPHY },
	{ C2C_LPI_HID, (kernel_ulong_t)C2C_LPI },
	{ }
};
MODULE_DEVICE_TABLE(acpi, nv_c2c_pmu_acpi_match);

static struct platform_driver nv_c2c_pmu_driver = {
	.driver = {
		.name = "nvidia-t410-c2c-pmu",
		.acpi_match_table = ACPI_PTR(nv_c2c_pmu_acpi_match),
		.suppress_bind_attrs = true,
	},
	.probe = nv_c2c_pmu_probe,
	.remove = nv_c2c_pmu_device_remove,
};

static int __init nv_c2c_pmu_init(void)
{
	int ret;

	ret = cpuhp_setup_state_multi(CPUHP_AP_ONLINE_DYN,
				      "perf/nvidia/c2c:online",
				      nv_c2c_pmu_online_cpu,
				      nv_c2c_pmu_cpu_teardown);
	if (ret < 0)
		return ret;

	nv_c2c_pmu_cpuhp_state = ret;
	return platform_driver_register(&nv_c2c_pmu_driver);
}

static void __exit nv_c2c_pmu_exit(void)
{
	platform_driver_unregister(&nv_c2c_pmu_driver);
	cpuhp_remove_multi_state(nv_c2c_pmu_cpuhp_state);
}

module_init(nv_c2c_pmu_init);
module_exit(nv_c2c_pmu_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("NVIDIA Tegra410 C2C PMU driver");
MODULE_AUTHOR("Besar Wicaksono <bwicaksono@nvidia.com>");
