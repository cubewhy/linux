/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * calculet_accel.h - UAPI of the CALCULET KS1 PCIe accelerator driver.
 *
 * The driver registers every board as a DRM compute-acceleration device and
 * exposes it through /dev/accel/accelN.  The requests below are driver
 * private DRM ioctls.  Register offsets, channel counts and request semantics
 * follow the KS1 datasheet.
 */
#ifndef _UAPI_CALCULET_ACCEL_H
#define _UAPI_CALCULET_ACCEL_H

#include <linux/types.h>

#include <drm/drm.h>

#define CALCULET_DRV_VER_MAJOR		1
#define CALCULET_DRV_VER_MINOR		1
#define CALCULET_DRV_VER_REVISION	4

#define CALCULET_READ_CHANNELS		8
#define CALCULET_WRITE_CHANNELS		8
#define CALCULET_TOTAL_CHANNELS		16
#define CALCULET_MAX_CHANNELS		32

#define CALCULET_MAX_BOARDS		64
#define CALCULET_MAX_STRING_LEN		0x100
#define CALCULET_MAX_UUID_LEN		0x40
#define CALCULET_MAX_PROCESS_ENTRIES	100

#define CALCULET_DMA_MAX_TRANSFER	(32u * 1024 * 1024)

/* BAR indices accepted by the raw BAR-transfer ioctls. */
#define CALCULET_BAR_RT			2
#define CALCULET_BAR_DEVINFO		4

/* op values for struct calculet_bar{32,64}_transfer_params. */
#define CALCULET_BAR_OP_READ		0
#define CALCULET_BAR_OP_WRITE		1

struct calculet_bar32_transfer_params {
	__u32 op;		/* CALCULET_BAR_OP_* */
	__u32 bar;		/* CALCULET_BAR_RT or CALCULET_BAR_DEVINFO */
	__aligned_u64 address;	/* BAR-relative, 4-byte aligned */
	__u32 value;		/* in for write, out for read */
};

struct calculet_bar64_transfer_params {
	__u32 op;
	__u32 bar;
	__aligned_u64 address;	/* BAR-relative, 8-byte aligned */
	__aligned_u64 value;
};

struct calculet_dma_transfer_channels_params {
	__u32 flags;		/* 0 = C2H (device -> host), 1 = H2C */
	__u32 channel;		/* 0..7 */
	__aligned_u64 length;	/* 1 .. 32 MiB */
	__aligned_u64 user_ptr;	/* userspace buffer */
	__aligned_u64 device_addr;	/* device address, 4-byte aligned */
};

struct calculet_sn_info {
	__u32 slot;		/* 0..15 */
	char sn[CALCULET_MAX_STRING_LEN];
};

struct calculet_msi_req {
	__u32 channel;		/* EP status channel, 0..15 */
};

/**
 * struct calculet_board_info - CALCULET_BOARD_INFO payload
 * @device_name: device name from DEVICE_INFO
 * @product_name: product name from DEVICE_INFO
 * @board_name: board name from DEVICE_INFO
 * @serial_uuid: UUID derived from the product name
 * @device_id: PCI device ID
 * @vendor_id: PCI vendor ID
 * @subsystem_id: PCI subsystem device ID
 * @subsystem_vendor_id: PCI subsystem vendor ID
 * @domain_number: PCI domain
 * @bus_number: PCI bus number
 * @device_number: PCI slot number
 * @function_number: PCI function number
 * @link_speed: PCIe link speed from PCI_EXP_LNKSTA
 * @link_width: PCIe link width from PCI_EXP_LNKSTA
 * @api_version: driver API version, currently 1
 * @abi_version: driver ABI version, currently 1
 * @driver_version: driver version string
 * @mac_address: device MAC address
 * @memory_size: device memory in bytes
 * @memory_bandwidth: device memory bandwidth in GB/s
 * @ddr_frequency: DDR frequency in MHz
 * @power_limit: board power limit in watts
 * @temperature_limit: board temperature limit in degrees Celsius
 */
struct calculet_board_info {
	char device_name[CALCULET_MAX_STRING_LEN];
	char product_name[CALCULET_MAX_STRING_LEN];
	char board_name[CALCULET_MAX_STRING_LEN];
	char serial_uuid[CALCULET_MAX_UUID_LEN];
	__u16 device_id;
	__u16 vendor_id;
	__u16 subsystem_id;
	__u16 subsystem_vendor_id;
	__u8 domain_number;
	__u8 bus_number;
	__u8 device_number;
	__u8 function_number;
	__u8 link_speed;
	__u8 link_width;
	__u32 api_version;
	__u32 abi_version;
	char driver_version[8];
	__u8 mac_address[12];
	__aligned_u64 memory_size;
	__aligned_u64 memory_bandwidth;
	__u32 ddr_frequency;
	__u32 power_limit;
	__u32 temperature_limit;
};

struct calculet_process_info_user {
	__u32 pid;
	__u32 ppid;
	__u32 uid;
	char comm[16];
	__u8 active;
	__u8 _pad[3];
};

struct calculet_process_info_list_user {
	__u32 count;
	__u32 board;
	struct calculet_process_info_user procs[CALCULET_MAX_PROCESS_ENTRIES];
};

/**
 * enum calculet_ioctl - CALCULET driver-private DRM ioctls
 * @CALCULET_BOARD_INFO: query static board information
 * @CALCULET_BAR64_TRANSFER: 64-bit BAR register access
 * @CALCULET_BAR32_TRANSFER: 32-bit BAR register access
 * @CALCULET_GET_SN: read the per-chiplet serial number
 * @CALCULET_BLOCK_DMA_TRANSFER: synchronous DMA transfer on one channel
 * @CALCULET_INTERRUPT_POLL: consume one poll-mode completion word
 * @CALCULET_EP_MSI_REQ: wait for an endpoint status interrupt
 * @CALCULET_SOFT_RESET: reset the endpoint through a PCIe bus rescan
 * @CALCULET_GET_PROCESS_LIST: list processes holding the device open
 */
enum calculet_ioctl {
	CALCULET_BOARD_INFO = 0,
	CALCULET_BAR64_TRANSFER,
	CALCULET_BAR32_TRANSFER,
	CALCULET_GET_SN,
	CALCULET_BLOCK_DMA_TRANSFER,
	CALCULET_INTERRUPT_POLL,
	CALCULET_EP_MSI_REQ,
	CALCULET_SOFT_RESET,
	CALCULET_GET_PROCESS_LIST,
};

#define DRM_IOCTL_CALCULET_BOARD_INFO					\
	DRM_IOWR(DRM_COMMAND_BASE + CALCULET_BOARD_INFO,			\
		 struct calculet_board_info)
#define DRM_IOCTL_CALCULET_BAR64_TRANSFER				\
	DRM_IOWR(DRM_COMMAND_BASE + CALCULET_BAR64_TRANSFER,		\
		 struct calculet_bar64_transfer_params)
#define DRM_IOCTL_CALCULET_BAR32_TRANSFER				\
	DRM_IOWR(DRM_COMMAND_BASE + CALCULET_BAR32_TRANSFER,		\
		 struct calculet_bar32_transfer_params)
#define DRM_IOCTL_CALCULET_GET_SN					\
	DRM_IOWR(DRM_COMMAND_BASE + CALCULET_GET_SN,			\
		 struct calculet_sn_info)
#define DRM_IOCTL_CALCULET_BLOCK_DMA_TRANSFER				\
	DRM_IOWR(DRM_COMMAND_BASE + CALCULET_BLOCK_DMA_TRANSFER,		\
		 struct calculet_dma_transfer_channels_params)
#define DRM_IOCTL_CALCULET_INTERRUPT_POLL				\
	DRM_IO(DRM_COMMAND_BASE + CALCULET_INTERRUPT_POLL)
#define DRM_IOCTL_CALCULET_EP_MSI_REQ					\
	DRM_IOWR(DRM_COMMAND_BASE + CALCULET_EP_MSI_REQ,			\
		 struct calculet_msi_req)
#define DRM_IOCTL_CALCULET_SOFT_RESET					\
	DRM_IO(DRM_COMMAND_BASE + CALCULET_SOFT_RESET)
#define DRM_IOCTL_CALCULET_GET_PROCESS_LIST				\
	DRM_IOWR(DRM_COMMAND_BASE + CALCULET_GET_PROCESS_LIST,		\
		 struct calculet_process_info_list_user)

#endif /* _UAPI_CALCULET_ACCEL_H */
