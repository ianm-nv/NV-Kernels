=====================================================================
NVIDIA Tegra410 SoC Uncore Performance Monitoring Unit (PMU)
=====================================================================

The NVIDIA Tegra410 SoC includes various system PMUs to measure key performance
metrics like memory bandwidth, latency, and utilization:

* Unified Coherence Fabric (UCF)
* PCIE
* PCIE-TGT
* CPU Memory
* NVLink-C2C
* NV-CLink
* NV-DLink

PMU Driver
----------

The PMU driver describes the available events and configuration of each PMU in
sysfs. Please see the sections below to get the sysfs path of each PMU. Like
other uncore PMU drivers, the driver provides "cpumask" sysfs attribute to show
the CPU id used to handle the PMU event. There is also "associated_cpus"
sysfs attribute, which contains a list of CPUs associated with the PMU instance.

UCF PMU
-------

The Unified Coherence Fabric (UCF) in the NVIDIA Tegra410 SoC serves as a
distributed cache, last level for CPU Memory and CXL Memory, and cache coherent
interconnect that supports hardware coherence across multiple coherently caching
agents, including:

  * CPU clusters
  * GPU
  * PCIe Ordering Controller Unit (OCU)
  * Other IO-coherent requesters

The UCF PMU monitors system level cache (SLC) events and MMIO requests.
Please see :ref:`NVIDIA_T410_PMU_Traffic_Coverage_Section` for more info about
the PMU traffic coverage.

The events and configuration options of this PMU device are described in sysfs,
see /sys/bus/event_source/devices/nvidia_ucf_pmu_<socket-id>.

Some of the events available in this PMU can be used to measure bandwidth and
utilization:

  * slc_access_rd: count the number of read requests to SLC.
  * slc_access_wr: count the number of write requests to SLC.
  * slc_bytes_rd: count the number of bytes transferred by slc_access_rd.
  * slc_bytes_wr: count the number of bytes transferred by slc_access_wr.
  * mem_access_rd: count the number of read requests to local or remote memory.
  * mem_access_wr: count the number of write requests to local or remote memory.
  * mem_bytes_rd: count the number of bytes transferred by mem_access_rd.
  * mem_bytes_wr: count the number of bytes transferred by mem_access_wr.
  * cycles: counts the UCF cycles.

The average bandwidth is calculated as::

   AVG_SLC_READ_BANDWIDTH_IN_GBPS = SLC_BYTES_RD / ELAPSED_TIME_IN_NS
   AVG_SLC_WRITE_BANDWIDTH_IN_GBPS = SLC_BYTES_WR / ELAPSED_TIME_IN_NS
   AVG_MEM_READ_BANDWIDTH_IN_GBPS = MEM_BYTES_RD / ELAPSED_TIME_IN_NS
   AVG_MEM_WRITE_BANDWIDTH_IN_GBPS = MEM_BYTES_WR / ELAPSED_TIME_IN_NS

The average request rate is calculated as::

   AVG_SLC_READ_REQUEST_RATE = SLC_ACCESS_RD / CYCLES
   AVG_SLC_WRITE_REQUEST_RATE = SLC_ACCESS_WR / CYCLES
   AVG_MEM_READ_REQUEST_RATE = MEM_ACCESS_RD / CYCLES
   AVG_MEM_WRITE_REQUEST_RATE = MEM_ACCESS_WR / CYCLES

More details about what other events are available can be found in Tegra410 SoC
technical manual.

The events can be filtered based on source or destination. The source filter
indicates the traffic initiator to the SLC, e.g local CPU, non-CPU device, or
remote socket. The destination filter specifies the destination memory type,
e.g. local system memory (CMEM), local GPU memory (GMEM), or remote memory. The
local/remote classification of the destination filter is based on the home
socket of the address, not where the data actually resides. The available
filters are described in
/sys/bus/event_source/devices/nvidia_ucf_pmu_<socket-id>/format/.

The list of UCF PMU event filters:

* Source filter:

  * src_loc_cpu: if set, count events from local CPU
  * src_loc_noncpu: if set, count events from local non-CPU device
  * src_rem: if set, count events from remote socket

* Destination filter:

  * dst_loc_cmem: if set, count events to local system memory (CMEM) address
  * dst_loc_gmem: if set, count events to local GPU memory (GMEM) address
  * dst_loc_other: if set, count events to local MMIO / CXL memory address
  * dst_rem: if set, count events to remote memory address

If the source is not specified, the PMU will count events from all sources. If
the destination is not specified, the PMU will count events to all destinations.

Example usage:

* Count event id 0x0 in socket 0 from all sources and to all destinations::

   perf stat -a -e nvidia_ucf_pmu_0/event=0x0/

* Count event id 0x0 in socket 0 with source filter = local CPU and destination
  filter = local system memory (CMEM)::

   perf stat -a -e nvidia_ucf_pmu_0/event=0x0,src_loc_cpu=0x1,dst_loc_cmem=0x1/

* Count event id 0x0 in socket 1 with source filter = local non-CPU device and
  destination filter = remote memory::

   perf stat -a -e nvidia_ucf_pmu_1/event=0x0,src_loc_noncpu=0x1,dst_rem=0x1/

PCIE PMU
----------------

This PMU monitors all read/write traffic from the root port(s) or a particular
BDF in a PCIE chiplet to local/remote memory. There is one PCIE PMU per PCIE
chiplet in the SoC. Please see :ref:`NVIDIA_T410_PMU_Traffic_Coverage_Section`
for more info about the PMU traffic coverage.

The events and configuration options of this PMU device are described in sysfs,
see /sys/bus/event_source/devices/nvidia_pcie_pmu_<socket-id>_chiplet_<pcie-chiplet-id>.

The events in this PMU can be used to measure bandwidth, utilization, and
latency:

  * rd_req: count the number of read requests by PCIE device.
  * wr_req: count the number of write requests by PCIE device.
  * rd_bytes: count the number of bytes transferred by rd_req.
  * wr_bytes: count the number of bytes transferred by wr_req.
  * rd_cum_outs: count outstanding rd_req each cycle.
  * cycles: counts the PCIE cycles.

The average bandwidth is calculated as::

   AVG_RD_BANDWIDTH_IN_GBPS = RD_BYTES / ELAPSED_TIME_IN_NS
   AVG_WR_BANDWIDTH_IN_GBPS = WR_BYTES / ELAPSED_TIME_IN_NS

The average request rate is calculated as::

   AVG_RD_REQUEST_RATE = RD_REQ / CYCLES
   AVG_WR_REQUEST_RATE = WR_REQ / CYCLES


The average latency is calculated as::

   FREQ_IN_GHZ = CYCLES / ELAPSED_TIME_IN_NS
   AVG_LATENCY_IN_CYCLES = RD_CUM_OUTS / RD_REQ
   AVERAGE_LATENCY_IN_NS = AVG_LATENCY_IN_CYCLES / FREQ_IN_GHZ

The PMU events can be filtered based on the traffic source and destination.
The source filter indicates the PCIE traffic initiator. The destination filter
specifies the destination memory type, e.g. local system memory (CMEM), local
GPU memory (GMEM), or remote memory. The local/remote classification of the
destination filter is based on the home socket of the address, not where the
data actually resides. The available filters are described in
/sys/bus/event_source/devices/nvidia_pcie_pmu_<socket-id>_chiplet_<pcie-chiplet-id>/format/.

The list of event filters:

* Source filter:

  * src_root_port: bitmap parameter to select the root port(s) in the PCIE
    chiplet that initiates the traffic. i.e. "src_root_port=0xF" corresponds to
    root port 0 to 3 in the chiplet.
  * src_bdf: the BDF that initiates the traffic. This is a 16-bit value that
    follows formula: (bus << 8) + (device << 3) + (function). For example, the
    value of BDF 27:01.1 is 0x2781.
  * src_bdf_en: enable the BDF filter. If this is set, the BDF filter value in
    "src_bdf" is used to filter the traffic.

  Note that Root-Port and BDF filters are mutually exclusive. If BDF filter is
  enabled, the BDF filter value will be applied to all events.

* Destination filter:

  * dst_loc_cmem: if set, count events to local system memory (CMEM) address
  * dst_loc_gmem: if set, count events to local GPU memory (GMEM) address
  * dst_loc_pcie_p2p: if set, count events to local PCIE peer address
  * dst_loc_pcie_cxl: if set, count events to local CXL memory address
  * dst_rem: if set, count events to remote memory address

If the source filter is not specified, the PMU will count events from all root
ports. If the destination filter is not specified, the PMU will count events
to all destinations.

Example usage:

* Count event id 0x0 from root port 0 of PCIE chiplet 0 on socket 0 targeting all
  destinations::

   perf stat -a -e nvidia_pcie_pmu_0_chiplet_0/event=0x0,root_port=0x1/

* Count event id 0x1 from root port 0 and 1 of PCIE chiplet 1 on socket 0 and
  targeting just local CMEM of socket 0::

   perf stat -a -e nvidia_pcie_pmu_0_chiplet_1/event=0x1,root_port=0x3,dst_loc_cmem=0x1/

* Count event id 0x2 from root port 0 of PCIE chiplet 2 on socket 1 targeting all
  destinations::

   perf stat -a -e nvidia_pcie_pmu_1_chiplet_2/event=0x2,root_port=0x1/

* Count event id 0x3 from root port 0 and 1 of PCIE chiplet 3 on socket 1 and
  targeting just local CMEM of socket 1::

   perf stat -a -e nvidia_pcie_pmu_1_chiplet_3/event=0x3,root_port=0x3,dst_loc_cmem=0x1/

* Count event id 0x4 from BDF 01:01.0 of PCIE chiplet 4 on socket 0 targeting all
  destinations::

   perf stat -a -e nvidia_pcie_pmu_0_chiplet_4/event=0x4,src_bdf=0x0180,src_bdf_en=0x1/

PCIE-TGT PMU
------------

The PCIE-TGT PMU monitors traffic targeting PCIE root ports or PCIE/CXL BAR.
There is one PCIE-TGT PMU per PCIE chiplet in the SoC. Please see
:ref:`NVIDIA_T410_PMU_Traffic_Coverage_Section` for more info about the PMU
traffic coverage.

The events and configuration options of this PMU device are described in sysfs,
see /sys/bus/event_source/devices/nvidia_pcie_tgt_pmu_<socket-id>_chiplet_<pcie-chiplet-id>.

The events in this PMU can be used to measure bandwidth and utilization:

  * rd_req: count the number of read requests to PCIE.
  * wr_req: count the number of write requests to PCIE.
  * rd_bytes: count the number of bytes transferred by rd_req.
  * wr_bytes: count the number of bytes transferred by wr_req.
  * cycles: counts the PCIE cycles.

The average bandwidth is calculated as::

   AVG_RD_BANDWIDTH_IN_GBPS = RD_BYTES / ELAPSED_TIME_IN_NS
   AVG_WR_BANDWIDTH_IN_GBPS = WR_BYTES / ELAPSED_TIME_IN_NS

The average request rate is calculated as::

   AVG_RD_REQUEST_RATE = RD_REQ / CYCLES
   AVG_WR_REQUEST_RATE = WR_REQ / CYCLES

The PMU events can be filtered based on the destination root port or target BAR
address range. The available filters are described in
/sys/bus/event_source/devices/nvidia_pcie_tgt_pmu_<socket-id>_chiplet_<pcie-chiplet-id>/format/.

The list of event filters:

* Destination filter:

  * dst_root_port: bitmap parameter to select the root port(s).
    I.e. "dst_root_port=0xF" corresponds to root port 0 to 3 in the PCIE chiplet.
  * dst_bar_addr: BAR filter base address.
  * dst_bar_mask: BAR filter address mask.
  * dst_bar_en: enable the BAR filter. If this is set, the BAR filter address
    range specified by "dst_bar_addr" and "dst_bar_mask" will be used to filter
    the traffic. The PMU uses the following comparison to determine if the
    traffic destination address falls within the filter range::

      (txn's addr & dst_bar_mask) == (dst_bar_addr & dst_bar_mask)

    If the comparison succeeds, then the event will be counted.

If the destination filter is not specified, the PMU will count events to all
root ports.

Example usage:

* Count event id 0x0 to root port 0 and 1 of PCIE chiplet 0 on socket 0::

   perf stat -a -e nvidia_pcie_tgt_pmu_0_chiplet_0/event=0x0,dst_root_port=0x3/

* Count event id 0x1 that falls within address range 0x10000 to 0x100FF in PCIE
  chiplet 0 on socket 0::

   perf stat -a -e nvidia_pcie_tgt_pmu_0_chiplet_0/event=0x1,dst_bar_addr=0x10000,dst_bar_mask=0xFFF00,dst_bar_en=0x1/

CPU Memory PMU
--------------

This PMU monitors bandwidth and latency events of memory read requests to local
CPU DRAM:

  * RD_REQ counters: count read requests (32B per request).
  * RD_CUM_OUTS counters: accumulated outstanding request counter, which track
    how many cycles the read requests are in flight.
  * CYCLES counter: counts the number of elapsed cycles.

The average bandwidth is calculated as::

   AVG_BANDWIDTH_IN_GBPS = RD_REQ * 32 / ELAPSED_TIME_IN_NS

The average latency is calculated as::

   FREQ_IN_GHZ = CYCLES / ELAPSED_TIME_IN_NS
   AVG_LATENCY_IN_CYCLES = RD_CUM_OUTS / RD_REQ
   AVERAGE_LATENCY_IN_NS = AVG_LATENCY_IN_CYCLES / FREQ_IN_GHZ

The events and configuration options of this PMU device are described in sysfs,
see /sys/bus/event_source/devices/nvidia_t410_cpu_mem_pmu_<socket-id>.

Example usage::

   perf stat -a -e '{nvidia_t410_cpu_mem_pmu_0/rd_req/,nvidia_t410_cpu_mem_pmu_0/rd_cum_outs/,nvidia_t410_cpu_mem_pmu_0/cycles/}' sleep 1

Please see :ref:`NVIDIA_T410_PMU_Traffic_Coverage_Section` for more info about
the PMU traffic coverage.


NVLink-C2C PMU
--------------

This PMU monitors memory read/write requests that are passing through the
NVIDIA Chip-to-Chip (C2C) interface.

The events and configuration options of this PMU device are described in sysfs,
see /sys/bus/event_source/devices/nvidia_nvlink_c2c_pmu_<socket-id>.

The list of events:

  * IN_RD_CUM_OUTS: accumulated outstanding request (in cycles) of incoming read requests.
  * IN_RD_REQ: the number of incoming read requests.
  * IN_WR_CUM_OUTS: accumulated outstanding request (in cycles) of incoming write requests.
  * IN_WR_REQ: the number of incoming write requests.
  * OUT_RD_CUM_OUTS: accumulated outstanding request (in cycles) of outgoing read requests.
  * OUT_RD_REQ: the number of outgoing read requests.
  * OUT_WR_CUM_OUTS: accumulated outstanding request (in cycles) of outgoing write requests.
  * OUT_WR_REQ: the number of outgoing write requests.
  * CYCLES: NVLink-C2C interface cycle counts.

The incoming events count the reads/writes from remote device to the SoC.
The outgoing events count the reads/writes from the SoC to remote device.

When the C2C interface is connected the SoC to GPU(s), the user can use the
"gpu" parameter to select the GPU(s) to monitor. Each bit represents the GPU
number, e.g. "gpu=0x1" corresponds to GPU 0 and "gpu=0x3" is for GPU 0 and 1.
The PMU will monitor all GPUs by default if not specified.

When connected to another SoC, only the read events are available.

The events can be used to calculate the average latency of the read/write requests::

   C2C_FREQ_IN_GHZ = CYCLES / ELAPSED_TIME_IN_NS

   IN_RD_AVG_LATENCY_IN_CYCLES = IN_RD_CUM_OUTS / IN_RD_REQ
   IN_RD_AVG_LATENCY_IN_NS = IN_RD_AVG_LATENCY_IN_CYCLES / C2C_FREQ_IN_GHZ

   IN_WR_AVG_LATENCY_IN_CYCLES = IN_WR_CUM_OUTS / IN_WR_REQ
   IN_WR_AVG_LATENCY_IN_NS = IN_WR_AVG_LATENCY_IN_CYCLES / C2C_FREQ_IN_GHZ

   OUT_RD_AVG_LATENCY_IN_CYCLES = OUT_RD_CUM_OUTS / OUT_RD_REQ
   OUT_RD_AVG_LATENCY_IN_NS = OUT_RD_AVG_LATENCY_IN_CYCLES / C2C_FREQ_IN_GHZ

   OUT_WR_AVG_LATENCY_IN_CYCLES = OUT_WR_CUM_OUTS / OUT_WR_REQ
   OUT_WR_AVG_LATENCY_IN_NS = OUT_WR_AVG_LATENCY_IN_CYCLES / C2C_FREQ_IN_GHZ

Example usage:

  * Count incoming traffic from all GPUs connected via nvlink_c2c:

    perf stat -a -e nvidia_nvlink_c2c_pmu_0/in_rd_req/ sleep 1

  * Count incoming traffic from GPU 0 connected via nvlink_c2c:

    perf stat -a -e nvidia_nvlink_c2c_pmu_0/in_rd_cum_outs,gpu=0x1/ sleep 1

  * Count outgoing traffic from the SoC to all GPUs connected via nvlink_c2c:

    perf stat -a -e nvidia_nvlink_c2c_pmu_0/out_rd_req/ sleep 1

  * Count outgoing traffic from the SoC to GPU 0 connected via nvlink_c2c:

    perf stat -a -e nvidia_nvlink_c2c_pmu_0/out_rd_cum_outs,gpu=0x1/ sleep 1

Please see :ref:`NVIDIA_T410_PMU_Traffic_Coverage_Section` for more info about
the PMU traffic coverage.

NV-CLink PMU
------------

This PMU monitors memory read/write requests that are passing through the NV-CLINK
interface.

The events and configuration options of this PMU device are described in sysfs,
see /sys/bus/event_source/devices/nvidia_nvclink_pmu_<socket-id>.

The list of events:

  * IN_RD_CUM_OUTS: accumulated outstanding request (in cycles) of incoming read requests.
  * IN_RD_REQ: the number of incoming read requests.
  * IN_WR_CUM_OUTS: accumulated outstanding request (in cycles) of incoming write requests.
  * IN_WR_REQ: the number of incoming write requests.
  * OUT_RD_CUM_OUTS: accumulated outstanding request (in cycles) of outgoing read requests.
  * OUT_RD_REQ: the number of outgoing read requests.
  * OUT_WR_CUM_OUTS: accumulated outstanding request (in cycles) of outgoing write requests.
  * OUT_WR_REQ: the number of outgoing write requests.
  * CYCLES: NV-CLINK interface cycle counts.

The incoming events count the reads/writes from remote device to the SoC.
The outgoing events count the reads/writes from the SoC to remote device.

Each SoC can have multiple instances of NV-CLINK. The user can use the
"instance" parameter to select the instance(s) to monitor. Each bit represents
the instance number, e.g. "instance=0x1" corresponds to instance 0 and
"instance=0x3" is for instance 0 and 1. The PMU will monitor all instances by
default if not specified.

When the NV-CLINK is connected to another SoC, only the read events are available.

The events can be used to calculate the average latency of the read/write requests::

   C2C_FREQ_IN_GHZ = CYCLES / ELAPSED_TIME_IN_NS

   IN_RD_AVG_LATENCY_IN_CYCLES = IN_RD_CUM_OUTS / IN_RD_REQ
   IN_RD_AVG_LATENCY_IN_NS = IN_RD_AVG_LATENCY_IN_CYCLES / C2C_FREQ_IN_GHZ

   IN_WR_AVG_LATENCY_IN_CYCLES = IN_WR_CUM_OUTS / IN_WR_REQ
   IN_WR_AVG_LATENCY_IN_NS = IN_WR_AVG_LATENCY_IN_CYCLES / C2C_FREQ_IN_GHZ

   OUT_RD_AVG_LATENCY_IN_CYCLES = OUT_RD_CUM_OUTS / OUT_RD_REQ
   OUT_RD_AVG_LATENCY_IN_NS = OUT_RD_AVG_LATENCY_IN_CYCLES / C2C_FREQ_IN_GHZ

   OUT_WR_AVG_LATENCY_IN_CYCLES = OUT_WR_CUM_OUTS / OUT_WR_REQ
   OUT_WR_AVG_LATENCY_IN_NS = OUT_WR_AVG_LATENCY_IN_CYCLES / C2C_FREQ_IN_GHZ

Example usage:

  * Count incoming traffic from all devices connected via NV-CLINK:

    perf stat -a -e nvidia_nvclink_pmu_0/in_rd_req/ sleep 1

  * Count incoming traffic from NV-CLINK instance 0:

    perf stat -a -e nvidia_nvclink_pmu_0/in_rd_cum_outs,instance=0x1/ sleep 1

  * Count outgoing traffic from the SoC to all devices connected via NV-CLINK:

    perf stat -a -e nvidia_nvclink_pmu_0/out_rd_req/ sleep 1

  * Count outgoing traffic from the SoC to devices connected via NV-CLINK instance 0 and 1:

    perf stat -a -e nvidia_nvclink_pmu_0/out_rd_cum_outs,instance=0x3/ sleep 1

Please see :ref:`NVIDIA_T410_PMU_Traffic_Coverage_Section` for more info about
the PMU traffic coverage.

NV-DLink PMU
------------

This PMU monitors memory read requests that are passing through the NV-DLINK
interface. In Tegra410 SoC, this PMU only counts read traffic from SoC to CXL memory.

The events and configuration options of this PMU device are described in sysfs,
see /sys/bus/event_source/devices/nvidia_nvdlink_pmu_<socket-id>.

The list of events:

  * IN_RD_CUM_OUTS: accumulated outstanding request (in cycles) of incoming read requests from SoC to CXL memory.
  * IN_RD_REQ: the number of incoming read requests from SoC to CXL memory.
  * CYCLES: NV-DLINK interface cycle counts.

The events can be used to calculate the average latency of the read/write requests::

   C2C_FREQ_IN_GHZ = CYCLES / ELAPSED_TIME_IN_NS

   IN_RD_AVG_LATENCY_IN_CYCLES = IN_RD_CUM_OUTS / IN_RD_REQ
   IN_RD_AVG_LATENCY_IN_NS = IN_RD_AVG_LATENCY_IN_CYCLES / C2C_FREQ_IN_GHZ

Example usage:

  * Count read events from the SoC to CXL memory:

    perf stat -a -e '{nvidia_nvdlink_pmu_0/in_rd_req/,nvidia_nvdlink_pmu_0/in_rd_cum_outs/}' sleep 1

Please see :ref:`NVIDIA_T410_PMU_Traffic_Coverage_Section` for more info about
the PMU traffic coverage.

.. _NVIDIA_T410_PMU_Traffic_Coverage_Section:

Traffic Coverage
----------------

The PMU traffic coverage may vary dependent on the chip configuration:

* **NVIDIA Tegra410 CPU-GPU Superchip**:

  Tegra410 SoC is connected with GPU(s) via NVLink-C2C and to other Tegra410
  SoC via NV-CLink. Example configuration::

   *************************************************             ************************************************
   * SOCKET-A                                      *             * SOCKET-B                                     *
   *                                               *             *                                              *
   *  ........                ::::::::::           *             *          ::::::::::                ........  *
   *  : GMEM :    /---------->: NIC A1 :           *             *          : NIC B1 :<----------\    : GMEM :  *
   *  ........   |            ::::::::::           *             *          ::::::::::            |   ........  *
   *        |    v                    |            *             *           |                    v     |       *
   *     ::::::::::                   |            *             *           |                   ::::::::::     *
   *     : GPU A1 :<--              :::::::::      *             *     :::::::::              -->: GPU B1 :     *
   *     ::::::::::  |              :       :      *             *     :       :              |  ::::::::::     *
   *                 |--NVLink C2C->: T410  :<--------NV-CLINK-------->: T410  :<-NVLink C2C--|                 *
   *     ::::::::::  |              : SoC A :      *             *     : SoC B :              |  ::::::::::     *
   *     : GPU A2 :<--     /------->:::::::::      *             *     :::::::::<---------\   -->: GPU B2 :     *
   *     ::::::::::       /         /    \         *             *        /    \          |      ::::::::::     *
   *        |    ^       |  ..........   ........  *             *  ........  ..........  |       ^     |       *
   *  ........   |       |  : CXLMEM :   : CMEM :  *             *  : CMEM :  : CXLMEM :  |       |   ........  *
   *  : GMEM :   |       |  ..........   ........  *             *  ........  ..........  |       |   : GMEM :  *
   *  ........   |       v                         *             *                        v       |   ........  *
   *             |     ::::::::::                  *             *                  ::::::::::    |             *
   *             ----->: NIC A2 :                  *             *                  : NIC B2 :<----             *
   *                   ::::::::::                  *             *                  ::::::::::                  *
   *                                               *             *                                              *
   *************************************************             ************************************************

   GMEM = GPU Memory (e.g. HBM)
   CMEM = CPU Memory (e.g. LPDDR5X)

 Traffic coverage of the PMUs in socket-A:

  * Traffic from Socket A CPU to following memory types:

    * Socket A CMEM:

      - UCF PMU: source filter = src_loc_cpu, destination filter = dst_loc_cmem
      - CPU Memory PMU:

        This PMU counts read traffic to local CMEM from all sources.

    * GPU A1 or A2 GMEM:

      - UCF PMU: source filter = src_loc_cpu, destination filter = dst_loc_gmem

        This PMU can not distinguish GPU A1 and A2 memory and always count both.

      - NVLink-C2C PMU:

        Use the outgoing events to count read/write trafficfrom all sources to GMEM.

    * Socket A PCIE BAR:

      - PCIE-TGT PMU:

        This PMU also counts traffic to CXLMEM. It can not distinguish CXLMEM
        vs PCIE BAR. It does not have source filter and counts traffic from any
        source.

    * Any CXLMEM of Socket A:

      - UCF PMU: source filter = src_loc_cpu, destination filter = dst_loc_other
      - PCIE-TGT PMU:

        Unlike UCF PMU, PCIE-TGT PMU can filter the destination based on
        root port or BAR address range. This PMU also counts traffic to PCIE
        BAR. It can not distinguish CXLMEM vs PCIE BAR. It does not have source
        filter and counts traffic from any source.

      - NV-DLink PMU:

        Use the incoming events to count read traffic from all sources.

    * Any memory of Socket B:

      - UCF PMU: source filter = src_loc_cpu, destination filter = dst_loc_rem

      - NV-CLink PMU:

        Use the outgoing events to count read/write traffic from all local sources.

  * Traffic from any non-CPU device of Socket A (i.e GPU or PCIE device) to following memory types:

    * Socket A CMEM:

      - UCF PMU: source filter = src_loc_noncpu, destination filter = dst_loc_cmem
      - PCIE PMU: destination filter = dst_loc_cmem

        This PMU only counts traffic from PCIE device.

      - CPU Memory PMU:

        This PMU counts read traffic to local CMEM from all sources.

      - NVLink-C2C PMU:

        Use the incoming events to count read/write traffic from GPU A1 or A2.

    * GPU A1 or A2 GMEM:

      - UCF PMU: source filter = src_loc_noncpu, destination filter = dst_loc_gmem

        This PMU can not distinguish GPU A1 and A2 memory and always count both.

      - PCIE PMU: destination filter = dst_loc_gmem

        This PMU only counts traffic from PCIE device.

      - NVLink-C2C PMU:

        Use the outgoing events to count read/write traffic from all sources to GMEM.

    * Socket A PCIE BAR:

      - PCIE-TGT PMU:

        This PMU also counts traffic to CXLMEM. It can not distinguish CXLMEM
        vs PCIE BAR. It does not have source filter and counts traffic from any
        source.

    * Any CXLMEM of Socket A:

      - UCF PMU: source filter = src_loc_noncpu, destination filter = dst_loc_other

      - PCIE PMU: destination filter = dst_loc_pcie_cxl

        This PMU only counts traffic from PCIE device.

      - PCIE-TGT PMU:

        Unlike UCF or PCIE PMU, PCIE-TGT PMU can filter the destination based
        on root port or BAR address range. This PMU also counts traffic to PCIE
        BAR. It can not distinguish CXLMEM vs PCIE BAR. It does not have source
        filter and counts traffic from any source.

      - NVLink-C2C PMU:

        Use the incoming events to count read/write traffic from GPU A1 or A2.

      - NV-DLink PMU:

        Use the incoming events to count read traffic from all sources.

    * Any memory of Socket B:

      - UCF PMU: source filter = src_loc_noncpu, destination filter = dst_loc_rem

      - PCIE PMU: destination filter = dst_loc_rem

        This PMU only counts traffic from PCIE device.

      - NVLink-C2C PMU:

        Use the incoming events to count read/write traffic from GPU A1 or A2.

      - NV-CLink PMU:

        Use the outgoing events to count read/write traffic from all local sources.

  * Traffic from any device in Socket B to following memory types:

    * Socket A CMEM:

      - UCF PMU: source filter = src_rem, destination filter = dst_loc_cmem
      - CPU Memory PMU:

        This PMU counts read traffic to local CMEM from all sources.

      - NV-CLink PMU:

        Use the incoming events to count read/write traffic to any local memory.

    * GPU A1 or A2 GMEM:

      - UCF PMU: source filter = src_rem, destination filter = dst_loc_gmem

        This PMU can not distinguish GPU A1 and A2 memory and always count both.

      - NVLink-C2C PMU:

        Use the outgoing events to count read/write traffic from all sources to GMEM.

      - NV-CLink PMU:

        Use the incoming events to count read/write traffic to any local memory.

    * Socket A PCIE BAR:

      - PCIE-TGT PMU:

        This PMU also counts traffic to CXLMEM. It can not distinguish CXLMEM
        vs PCIE BAR. It does not have source filter and counts traffic from any
        source.

    * Any CXLMEM of Socket A:

      - UCF PMU: source filter = src_rem, destination filter = dst_loc_other
      - PCIE-TGT PMU:

        Unlike UCF PMU, PCIE-TGT PMU can filter the destination based on
        root port or BAR address range. This PMU also counts traffic to PCIE
        BAR. It can not distinguish CXLMEM vs PCIE BAR. It does not have source
        filter and counts traffic from any source.

      - NV-CLink PMU:

        Use the incoming events to count read/write traffic to any local memory.

      - NV-DLink PMU:

        Use the incoming events to count read traffic from all sources.


* **NVIDIA Tegra410 CPU-CPU Superchip**:

  Tegra410 SoC is connected with GPU(s) via NV-CLink and to other Tegra410
  SoC via NVLink-C2C. Example configuration::

   *************************************************             ************************************************
   * SOCKET-A                                      *             * SOCKET-B                                     *
   *                                               *             *                                              *
   *  ........                ::::::::::           *             *          ::::::::::                ........  *
   *  : GMEM :    /---------->: NIC A1 :           *             *          : NIC B1 :<----------\    : GMEM :  *
   *  ........   |            ::::::::::           *             *          ::::::::::            |   ........  *
   *        |    v                    |            *             *           |                    v     |       *
   *     ::::::::::                   |            *             *           |                   ::::::::::     *
   *     : GPU A1 :<--              :::::::::      *             *     :::::::::              -->: GPU B1 :     *
   *     ::::::::::  |              :       :      *             *     :       :              |  ::::::::::     *
   *                 |---NV-CLINK-->: T410  :<-------NVLink C2C------->: T410  :<--NV-CLINK---|                 *
   *     ::::::::::  |              : SoC A :      *             *     : SoC B :              |  ::::::::::     *
   *     : GPU A2 :<--     /------->:::::::::      *             *     :::::::::<---------\   -->: GPU B2 :     *
   *     ::::::::::       /         /    \         *             *        /    \          |      ::::::::::     *
   *        |    ^       |  ..........   ........  *             *  ........  ..........  |       ^     |       *
   *  ........   |       |  : CXLMEM :   : CMEM :  *             *  : CMEM :  : CXLMEM :  |       |   ........  *
   *  : GMEM :   |       |  ..........   ........  *             *  ........  ..........  |       |   : GMEM :  *
   *  ........   |       v                         *             *                        v       |   ........  *
   *             |     ::::::::::                  *             *                  ::::::::::    |             *
   *             ----->: NIC A2 :                  *             *                  : NIC B2 :<----             *
   *                   ::::::::::                  *             *                  ::::::::::                  *
   *                                               *             *                                              *
   *************************************************             ************************************************

   GMEM = GPU Memory (e.g. HBM)
   CMEM = CPU Memory (e.g. LPDDR5X)

  The traffic coverage is similar to the ones in CPU-GPU Superchip, except that
  the NVLink-C2C PMU coverage needs to be swapped with NV-CLink PMU.