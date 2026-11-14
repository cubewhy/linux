/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * UAPI for the CALCULET KS1 PCIe accelerator driver.
 */
#ifndef _UAPI_DRM_CALCULET_ACCEL_H
#define _UAPI_DRM_CALCULET_ACCEL_H

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
#define CALCULET_MAX_STRING_LEN		256
#define CALCULET_MAX_UUID_LEN		64
#define CALCULET_MAX_CHIPS		16
#define CALCULET_MAX_PROCESS_ENTRIES	100

#define CALCULET_DMA_MAX_TRANSFER	(32U * 1024U * 1024U)

/* Transfer direction for the raw BAR and block-DMA requests. */
enum calculet_transfer_direction {
	CALCULET_TRANSFER_C2H = 0,	/* device to host */
	CALCULET_TRANSFER_H2C,		/* host to device */
};

/* BAR selection for the raw register requests. */
enum calculet_bar {
	CALCULET_BAR_RT = 2,
	CALCULET_BAR_SMI = 4,
};

struct calculet_bar32_transfer_params {
	__u32 direction;	/* enum calculet_transfer_direction */
	__u32 bar;		/* enum calculet_bar */
	__aligned_u64 offset;	/* BAR-relative, 4-byte aligned */
	__u32 value;		/* in for H2C, out for C2H */
	__u32 pad;
};

struct calculet_bar64_transfer_params {
	__u32 direction;
	__u32 bar;
	__aligned_u64 offset;	/* BAR-relative, 4-byte aligned */
	__aligned_u64 value;	/* in for H2C, out for C2H */
};

struct calculet_dma_transfer_params {
	__u32 direction;	/* enum calculet_transfer_direction */
	__u32 channel;		/* 0..CALCULET_{READ,WRITE}_CHANNELS - 1 */
	__aligned_u64 size;	/* 1..CALCULET_DMA_MAX_TRANSFER */
	__aligned_u64 host_address;
	__aligned_u64 device_address;
};

/**
 * struct calculet_board_info - static board description
 * @product_name: product name from the SMI BAR
 * @serial_number: board serial number from the SMI BAR
 * @product_number: product number from the SMI BAR
 * @uuid: UUID derived from @serial_number
 * @device_id: PCI device ID
 * @vendor_id: PCI vendor ID
 * @subsystem_device: PCI subsystem device ID
 * @subsystem_vendor: PCI subsystem vendor ID
 * @domain: PCI domain number
 * @bus: PCI bus number
 * @device: PCI slot number
 * @function: PCI function number
 * @link_speed: negotiated PCIe link speed
 * @link_width: negotiated PCIe link width
 * @driver_version_major: driver major version
 * @driver_version_minor: driver minor version
 * @driver_version: driver version string
 * @firmware_version: firmware version from the SMI BAR
 * @total_memory: total device memory in bytes
 * @memory_bandwidth: memory bandwidth as IEEE 754 binary64 bits
 * @ddr_frequency: DDR frequency in MHz
 * @max_power_limit: board power limit in watts
 * @temp_limit: board temperature limit in degrees Celsius
 */
struct calculet_board_info {
	char product_name[CALCULET_MAX_STRING_LEN];
	char serial_number[CALCULET_MAX_STRING_LEN];
	char product_number[CALCULET_MAX_STRING_LEN];
	char uuid[CALCULET_MAX_UUID_LEN];

	__u16 device_id;
	__u16 vendor_id;
	__u16 subsystem_device;
	__u16 subsystem_vendor;

	__u8 domain;
	__u8 bus;
	__u8 device;
	__u8 function;
	__u8 link_speed;
	__u8 link_width;
	__u16 pad0;

	__u32 driver_version_major;
	__u32 driver_version_minor;
	char driver_version[CALCULET_MAX_STRING_LEN];

	char firmware_version[12];
	__u32 pad1;

	__aligned_u64 total_memory;
	__aligned_u64 memory_bandwidth;
	__u32 ddr_frequency;
	__u32 max_power_limit;
	__u32 temp_limit;
	__u32 pad2;
};

struct calculet_process_info {
	__s32 pid;
	__s32 ppid;
	__u32 uid;
	char comm[16];
	__u8 active;
	__u8 pad[3];
};

struct calculet_process_list {
	__u32 count;
	__u32 board_index;
	struct calculet_process_info process[CALCULET_MAX_PROCESS_ENTRIES];
};

struct calculet_sn_info {
	__u32 chip_id;		/* 0..CALCULET_MAX_CHIPS - 1 */
	char sn[CALCULET_MAX_STRING_LEN];
};

struct calculet_msi_request {
	__u32 channel;		/* 0..CALCULET_TOTAL_CHANNELS - 1 */
};

/**
 * enum calculet_ioctl - driver-private DRM ioctls
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
		 struct calculet_dma_transfer_params)
#define DRM_IOCTL_CALCULET_INTERRUPT_POLL				\
	DRM_IO(DRM_COMMAND_BASE + CALCULET_INTERRUPT_POLL)
#define DRM_IOCTL_CALCULET_EP_MSI_REQ					\
	DRM_IOWR(DRM_COMMAND_BASE + CALCULET_EP_MSI_REQ,			\
		 struct calculet_msi_request)
#define DRM_IOCTL_CALCULET_SOFT_RESET					\
	DRM_IO(DRM_COMMAND_BASE + CALCULET_SOFT_RESET)
#define DRM_IOCTL_CALCULET_GET_PROCESS_LIST				\
	DRM_IOWR(DRM_COMMAND_BASE + CALCULET_GET_PROCESS_LIST,		\
		 struct calculet_process_list)

#endif /* _UAPI_DRM_CALCULET_ACCEL_H */
