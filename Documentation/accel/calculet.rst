.. SPDX-License-Identifier: GPL-2.0

=====================
Calculet KS1 accel
=====================

The ``calculet_pci`` driver supports CALCULET KS1 PCIe compute
accelerators.  It is a DRM compute-acceleration driver: each board is
registered with ``DRIVER_COMPUTE_ACCEL`` and exposed as ``/dev/accel/accelN``.

Driver-private requests use the DRM command range starting at
``DRM_COMMAND_BASE`` and are defined in
``include/uapi/drm/calculet_accel.h``.  They replace the legacy character
device requests one-for-one; the request payloads keep the original field
meaning.

Device discovery
================

Every PCI function with vendor ID ``0x20f5`` is bound to the driver.  Each
board maps three BARs: BAR0 for the DMA engine, BAR2 for the runtime status
and reset registers, and BAR4 for the SMI/device-information block.

The accel device nodes use the standard DRM permissions; distributions that
need unprivileged access should ship a matching udev rule.

The following attributes are attached to the PCI device:

``driver_version``
	Driver version string.

``poll_mode``
	DMA completion mode selected with the ``poll_mode`` module parameter.

``board_state``
	``active``, ``suspended`` or ``removing``.

``health_status``
	Result of the board health check.

Requests
========

``DRM_IOCTL_CALCULET_BOARD_INFO``
	Return product and serial strings, PCI IDs, PCIe link status and the
	hardware specification registers from BAR4.

``DRM_IOCTL_CALCULET_BAR32_TRANSFER``
``DRM_IOCTL_CALCULET_BAR64_TRANSFER``
	Read or write one 32-bit or 64-bit register in BAR2 or BAR4.

``DRM_IOCTL_CALCULET_GET_SN``
	Read the 32-byte serial number of one chip.

``DRM_IOCTL_CALCULET_BLOCK_DMA_TRANSFER``
	Run a synchronous block transfer on one of the eight read or eight
	write channels.  Requests on the same channel are serialized; a
	transfer is limited to 32 MiB.

``DRM_IOCTL_CALCULET_INTERRUPT_POLL``
	Consume one completion word in polled-completion mode.

``DRM_IOCTL_CALCULET_EP_MSI_REQ``
	Wait for an endpoint status interrupt.

``DRM_IOCTL_CALCULET_SOFT_RESET``
	Reset the endpoint through its reset register and rescan the PCI bus.

``DRM_IOCTL_CALCULET_GET_PROCESS_LIST``
	Return the processes that currently hold the board open.

The raw BAR write, endpoint MSI and soft-reset requests can be restricted to
privileged callers with the ``restrict_privileged_io`` module parameter.

Completion modes
================

By default each DMA channel uses one MSI vector.  With ``poll_mode=1`` the
driver programs the endpoint to write completion words into a coherent
status page instead and completes transfers from a poll thread or from
``DRM_IOCTL_CALCULET_INTERRUPT_POLL``.

Process monitor
===============

``/proc/calculet_monitor/summary`` lists the active boards and their open
process count.  ``/proc/calculet_monitor/processes`` lists every open
process.  Each board also has a ``deviceN`` file with its process list.

Module parameters
=================

``poll_mode``
	Use polled completion instead of interrupts.  Read-only after load.

``max_boards``
	Maximum number of boards to support, capped at 64.

``dma_timeout_ms``
	Timeout for a synchronous block transfer, default 100000.

``restrict_privileged_io``
	Require ``CAP_SYS_RAWIO``/``CAP_SYS_ADMIN`` for raw BAR writes,
	endpoint MSI requests and soft reset.

Power management and errors
===========================

Suspend quiesces DMA, stops the poll thread, releases IRQ vectors and
coherent buffers, then puts the function into the selected low-power state.
Resume restores the PCI configuration and rebuilds the DMA and interrupt
state.

An AER event permanently marks the board unavailable; a normal PM resume
does not reactivate it.  Rebind the driver after validating the hardware.
