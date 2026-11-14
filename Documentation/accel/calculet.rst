.. SPDX-License-Identifier: GPL-2.0

=============================
Calculet KS1 accel driver
=============================

The ``calculet_pci`` driver supports the Calculet KS1 family of PCIe
compute accelerators.  It is a DRM compute-acceleration driver: every
board is registered as a DRM device with the ``DRIVER_COMPUTE_ACCEL``
feature and is exposed to userspace as ``/dev/accel/accelN``.

Driver-private ioctls use the DRM command range starting at
``DRM_COMMAND_BASE``.  The userspace interface is defined in
``include/uapi/drm/calculet_accel.h``.

Device discovery
================

Each PCI function with vendor ID ``0x20f5`` is bound to the driver and
gets its own DRM accel device.  The DRM accel minor number is unrelated
to the driver's internal board index, which is used by
``/proc/calculet_monitor`` and by the PCIe soft-reset slot reservation.

The following attributes are available on the PCI device:

``board_state``
	Current board state: ``ACTIVE``, ``SUSPENDED`` or ``REMOVING``.

``health_status``
	Board health check result.

``poll_mode``
	DMA completion mode selected with the ``poll_mode`` module
	parameter.

``driver_version``
	Driver version string.

Process accounting is exported through ``/proc/calculet_monitor``.  Its
``summary`` and ``processes`` files list the boards and the processes
that currently hold an accel device open.

Ioctls
======

The interface mirrors the KS1 register and DMA model:

``DRM_IOCTL_CALCULET_BOARD_INFO``
	Return static board information (names, PCI IDs, link status,
	memory size and bandwidth, limits).

``DRM_IOCTL_CALCULET_BAR64_TRANSFER``
``DRM_IOCTL_CALCULET_BAR32_TRANSFER``
	Read or write one register in BAR 2 (RT) or BAR 4 (DEVICE_INFO).

``DRM_IOCTL_CALCULET_GET_SN``
	Read the 32-byte serial number of one chiplet.

``DRM_IOCTL_CALCULET_BLOCK_DMA_TRANSFER``
	Perform a synchronous 32-bit DMA transfer on one of the eight
	read or eight write channels.

``DRM_IOCTL_CALCULET_INTERRUPT_POLL``
	Consume one completion word in polled-completion mode.

``DRM_IOCTL_CALCULET_EP_MSI_REQ``
	Wait for an endpoint status interrupt.

``DRM_IOCTL_CALCULET_SOFT_RESET``
	Reset the endpoint and rescan its PCIe bus.

``DRM_IOCTL_CALCULET_GET_PROCESS_LIST``
	Return the processes that currently hold the device open.

The raw BAR, endpoint MSI and soft-reset ioctls can be restricted to
privileged callers with the ``restrict_privileged_io`` module
parameter.
