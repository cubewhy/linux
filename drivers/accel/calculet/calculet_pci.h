/* SPDX-License-Identifier: GPL-2.0 */
/*
 * calculet_pci.h - internal definitions of the CALCULET PCIe driver.
 *
 * Reconstructed from calculet_pci.ko 1.1.4.  Structure fields are ordered to
 * match the offsets used by the binary; comments give the recovered offset.
 */
#ifndef _CALCULET_PCI_H
#define _CALCULET_PCI_H

#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/idr.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/pid.h>
#include <linux/proc_fs.h>
#include <linux/refcount.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/wait.h>

#include <drm/drm_device.h>

#include <uapi/drm/calculet_accel.h>

#define CALCULET_DRV_NAME		"calculet"
#define CALCULET_MOD_NAME		"calculet_pci"
#define CALCULET_DRV_VERSION		"1.1.4"
#define CALCULET_PCI_VENDOR_ID		0x20f5
#define CALCULET_DEFAULT_DMA_TIMEOUT_MS	100000
#define CALCULET_DEFAULT_MAX_BOARDS	64
#define CALCULET_DEFAULT_LOG_LEVEL	6
#define CALCULET_IRQ_NAME		"calculet"
#define CALCULET_MAX_IRQ_VECTORS	CALCULET_MAX_CHANNELS

/* Resource indices inside struct calculet_pci_board::res[]. */
#define CALCULET_RES_OS			0	/* BAR0: OS/RT interrupt and status */
#define CALCULET_RES_RT			1	/* BAR2: device register window */
#define CALCULET_RES_DEVINFO		2	/* BAR4: DEVICE_INFO block */

/* BAR0 (OS) registers, offsets recovered from calculet_pcie_enable_interrupts. */
#define CALCULET_OS_IRQ_EN		0x0c
#define CALCULET_RT_IRQ_EN		0x2c
#define CALCULET_RD_MSI_ADDR_LO		0x60
#define CALCULET_RD_MSI_ADDR_HI		0x64
#define CALCULET_WR0_MSI_ADDR_LO	0x68
#define CALCULET_WR0_MSI_ADDR_HI	0x6c
#define CALCULET_WR1_MSI_ADDR_LO	0xcc
#define CALCULET_WR1_MSI_ADDR_HI	0xd0
#define CALCULET_WR2_MSI_ADDR_LO	0xd4
#define CALCULET_WR2_MSI_ADDR_HI	0xd8
#define CALCULET_RD_MSI_DATA0		0x70	/* 4 x u32, two vectors each */
#define CALCULET_WR_MSI_DATA0		0xdc	/* 4 x u32, two vectors each */

/* BAR2 (RT) registers. */
#define CALCULET_RT_OS_STATUS		0x0223810c
#define CALCULET_RT_RT_STATUS		0x02238250
#define CALCULET_RT_SOFT_RESET		0x02238108

/* BAR2 DMA engine: read channels select at +0x30, write channels at +0x10. */
#define CALCULET_DMA_RD_SELECT		0x30
#define CALCULET_DMA_WR_SELECT		0x10
#define CALCULET_DMA_RD_BASE(ch)	(0x300 + ((ch) * 0x200))
#define CALCULET_DMA_WR_BASE(ch)	(0x200 + ((ch) * 0x200))
#define CALCULET_DMA_CTRL		0x00	/* write 0x10 = start */
#define CALCULET_DMA_LEN		0x08
#define CALCULET_DMA_ADDR_LO		0x0c
#define CALCULET_DMA_ADDR_HI		0x10
#define CALCULET_DMA_HOST_LO		0x14
#define CALCULET_DMA_HOST_HI		0x18

/* DEVICE_INFO (BAR4) offsets. */
#define CALCULET_DEVI_DEVICE_NAME	0x00
#define CALCULET_DEVI_MAC		0x20
#define CALCULET_DEVI_BOARD_NAME	0x2c
#define CALCULET_DEVI_PRODUCT_NAME	0x200
#define CALCULET_DEVI_MEM_SIZE		0x4c
#define CALCULET_DEVI_MEM_BW		0x54
#define CALCULET_DEVI_DDR_FREQ		0x5c
#define CALCULET_DEVI_POWER_LIMIT	0x60
#define CALCULET_DEVI_TEMP_LIMIT	0x64
#define CALCULET_DEVI_SERIAL(chip)	(0x4800 + ((chip) * 0x400))

/* DMA channel states (calculet_dma_channel::state). */
enum calculet_dma_state {
	CALCULET_DMA_IDLE	= 0,
	CALCULET_DMA_ACTIVE	= 1,
	CALCULET_DMA_COMPLETED	= 2,
	CALCULET_DMA_FAILED	= 3,
	CALCULET_DMA_DEAD	= 4,
};

enum calculet_board_state {
	CALCULET_BOARD_INVALID		= 0,
	CALCULET_BOARD_ACTIVE		= 1,
	CALCULET_BOARD_SUSPENDED	= 2,
	CALCULET_BOARD_REMOVING		= 3,
};

struct calculet_pci_board;

/*
 * struct calculet_hw_profile - per-SKU register layout.
 *
 * The driver in this module only ships "legacy-ks1": os_bar = 0, rt_bar = 2,
 * dma_bar = 4, eight read and eight write DMA channels, and three BAR2
 * offsets.  The optional callbacks at +0x20..+0x38 are NULL in that profile.
 */
struct calculet_hw_profile {
	const char *name;			/* +0x00 */
	u8 os_bar;				/* +0x08 */
	u8 rt_bar;				/* +0x09 */
	u8 dma_bar;				/* +0x0a */
	u8 rd_channels;				/* +0x0b */
	u8 wr_channels;				/* +0x0c */
	u32 os_status_reg;			/* +0x10 */
	u32 rt_status_reg;			/* +0x14 */
	u32 soft_reset_reg;			/* +0x18 */
	int (*dma_status)(struct calculet_pci_board *board, bool is_write,
			  u8 channel, u8 *done, u8 *error);	/* +0x20 */
	void (*dma_event)(struct calculet_pci_board *board, bool is_write,
			  u8 channel);				/* +0x28 */
	int (*dma_abort)(struct calculet_pci_board *board, bool is_write,
			 u8 channel);				/* +0x30 */
};

struct calculet_pci_res {
	resource_size_t phys;		/* +0x00 */
	void __iomem *io;		/* +0x08 */
	resource_size_t size;		/* +0x10 */
	struct mutex lock;		/* +0x18 */
	spinlock_t reg_lock;		/* +0x38 */
};

struct calculet_dma_controller;

struct calculet_dma_channel {
	u8 id;				/* +0x00 */
	u8 vector;			/* +0x01 */
	u8 is_write;			/* +0x02 */
	u8 pad0;			/* +0x03 */
	u32 irq;			/* +0x04 */
	bool irq_registered;		/* +0x08 */
	u8 pad1[3];			/* +0x09 */
	int state;			/* +0x0c */
	int result;			/* +0x10 */
	spinlock_t lock;		/* +0x14 */
	struct calculet_dma_controller *ctrl;	/* +0x18 */
	struct calculet_pci_board *board;	/* +0x20 */
	void __iomem *os_base;		/* +0x28 */
	u64 length;			/* +0x30 */
	u64 user_ptr;			/* +0x38 */
	u64 device_addr;		/* +0x40 */
	wait_queue_head_t wq;		/* +0x48 */
	void *buf;			/* +0x60 */
	size_t buf_size;		/* +0x68 */
	dma_addr_t buf_dma;		/* +0x70 */
	struct mutex buf_lock;		/* +0x78 */
	bool poisoned;			/* +0x98 */
	bool armed;			/* +0x99 */
	u8 pad2[6];			/* +0x9a */
	u64 errors;			/* +0xa0 */
	u64 started_at;			/* +0xb0 */
	struct mutex ioctl_lock;	/* +0xb8 */
};

/* Poll-mode completion region (controller->poll->status). */
struct calculet_poll_status {
	dma_addr_t dma;			/* +0x00 */
	u32 *ptr;			/* +0x08 */
	u8 pad0[0x40];			/* +0x10..+0x4f */
	u32 map[16];			/* +0x50, initialised to 100 + i */
	spinlock_t lock;		/* +0x90 */
	u32 running;			/* +0x94 */
};

struct calculet_poll_ctx {
	struct calculet_poll_status *status;	/* +0x00 */
	struct task_struct *thread;		/* +0x08 */
	char name[24];				/* +0x10 */
	u8 pad0[4];				/* +0x28 */
	u32 stop;				/* +0x2c */
	struct completion stopped;		/* +0x30 */
};

struct calculet_dma_controller {
	struct device *dev;			/* +0x00 */
	struct calculet_pci_board *board;	/* +0x08 */
	struct calculet_dma_channel rd[CALCULET_READ_CHANNELS];	/* +0x10 */
	struct calculet_dma_channel wr[CALCULET_WRITE_CHANNELS];/* +0x6d0 */
	struct calculet_dma_channel ep[16];			/* +0xd90 */
	struct calculet_poll_ctx *poll;		/* +0x1b10 */
	struct mutex state_lock;		/* +0x1b18 */
	atomic_t active;			/* +0x1b38 */
	wait_queue_head_t active_wq;		/* +0x1b40 */
	u32 timeout_ms;				/* +0x1b58 */
	bool accepting;				/* +0x1b5c */
	bool ready;				/* +0x1b5d */
	bool initialized;			/* +0x1b5e */
};

struct calculet_process_info {
	struct list_head list;		/* +0x00 */
	u32 pid;			/* +0x10 */
	u32 ppid;			/* +0x14 */
	struct pid *pid_ref;		/* +0x18 */
	int refcount;			/* +0x20 */
	kuid_t uid;			/* +0x24 */
	char comm[16];			/* +0x28 */
	bool active;			/* +0x38 */
};

struct calculet_file_private {
	struct calculet_pci_board *board;
	u32 pid;
	struct pid *pid_ref;
	bool access;
};

struct calculet_pci_board {
	struct list_head list;			/* +0x00 */
	struct pci_dev *pdev;			/* +0x10 */
	const struct calculet_hw_profile *profile;	/* +0x18 */
	u32 index;				/* +0x20 */
	refcount_t refcount;			/* +0x24 */
	struct mutex state_lock;		/* +0x28 */
	atomic_t active_ops;			/* +0x48 */
	wait_queue_head_t op_wq;		/* +0x50 */
	int state;				/* +0x68 */
	struct calculet_pci_res res[3];		/* +0x70 */
	bool operational;			/* +0x130 */
	bool regions_requested;			/* +0x131 */
	struct calculet_dma_controller dma;	/* +0x138 */
	bool irq_enabled;			/* +0x1c98 */
	int irq_vectors;			/* +0x1c9c */
	int irq_count;				/* +0x1ca0 */
	bool irq_used[CALCULET_MAX_IRQ_VECTORS];/* +0x1ca4 */
	struct drm_device *drm;			/* +0x1cc8 */
	u64 stat_ops;				/* +0x1cd0 */
	u64 stat_errors;			/* +0x1cd8 */
	u64 stat_irqs;				/* +0x1cf0 */
	u64 stat_timeouts;			/* +0x1cf8 */
	struct list_head procs;			/* +0x1d08 */
	struct mutex proc_lock;			/* +0x1d18 */
	int proc_count;				/* +0x1d38 */
	struct proc_dir_entry *proc_entry;	/* +0x1d40 */
	char proc_name[32];			/* +0x1d48 */
	bool reset_in_progress;			/* +0x1d68 */
	u8 pad_reset[3];			/* +0x1d69 */
	bool soft_reset_pending;		/* +0x1d6c */
	u8 pad_reset2[3];			/* +0x1d6d */
	u32 pad_reset3;				/* +0x1d70 */
	bool suspended;				/* +0x1d74 */
	bool fail_closed;			/* +0x1d75 */
};

extern struct calculet_pci_board *calculet_board_from_dev(struct device *dev);

/* board_mgr.c */
int calculet_board_init(struct calculet_pci_board *board);
struct calculet_pci_board *calculet_board_get(struct calculet_pci_board *board);
void calculet_board_put(struct calculet_pci_board *board);
void calculet_board_set_state(struct calculet_pci_board *board, int state);
int calculet_board_get_state(struct calculet_pci_board *board);
bool calculet_board_is_operational(struct calculet_pci_board *board);
int calculet_board_begin_op(struct calculet_pci_board *board);
void calculet_board_end_op(struct calculet_pci_board *board);
void calculet_board_begin_remove(struct calculet_pci_board *board);
int calculet_board_mgr_init(void);
void calculet_board_mgr_cleanup(void);
void calculet_board_mgr_put_board(struct calculet_pci_board *board);
int calculet_board_mgr_add_board(struct calculet_pci_board *board);
void calculet_board_mgr_remove_board(struct calculet_pci_board *board);
int calculet_board_mgr_get_all_boards(struct calculet_pci_board **boards,
				      int max);
int calculet_board_health_check(struct calculet_pci_board *board);
int calculet_global_soft_reset_prepare(struct calculet_pci_board *board);
void calculet_global_soft_reset_complete(struct calculet_pci_board *board);
bool calculet_global_soft_reset_can_rescan(void);
void calculet_board_reserve_slot(struct calculet_pci_board *board);
unsigned int calculet_max_boards_limit(void);

/* calculet_pci_core.c */
int pcie_resources_init(struct pci_dev *pdev, void *resources);
void pcie_resources_release(struct pci_dev *pdev, void *resources);
int calculet_enable_interrupts(struct calculet_pci_board *board);
void calculet_disable_interrupts(struct calculet_pci_board *board);
u32 calculet_resource_read32(struct calculet_pci_res *res, unsigned long off);
u64 calculet_resource_read64(struct calculet_pci_res *res, unsigned long off);
void calculet_resource_write32(struct calculet_pci_res *res, unsigned long off,
			       u32 val);
void calculet_resource_write64(struct calculet_pci_res *res, unsigned long off,
			       u64 val);
void calculet_read_strings(void *dst, size_t dst_size,
			   struct calculet_pci_res *res, unsigned long off,
			   size_t len);
int calculet_activate_board(struct calculet_pci_board *board);

/* dma.c */
int calculet_dma_controller_init(struct calculet_dma_controller *ctrl,
				 struct device *dev, bool poll_mode);
void calculet_dma_controller_deinit(struct calculet_dma_controller *ctrl,
				    bool poll_mode);
void calculet_dma_controller_set_timeout(struct calculet_dma_controller *ctrl,
					 u32 timeout_ms);
void calculet_dma_stop_new_transfers(struct calculet_dma_controller *ctrl);
void calculet_dma_wait_for_transfers(struct calculet_dma_controller *ctrl);
void calculet_dma_fail_active_transfers(struct calculet_dma_controller *ctrl,
					int status);
void calculet_dma_channel_set_state(struct calculet_dma_channel *ch,
				    int state);
long calculet_dma_transfer_params(struct calculet_pci_res *res,
				  struct calculet_dma_controller *ctrl,
				  struct calculet_dma_transfer_channels_params *p);
int calculet_dma_poll_once(struct calculet_dma_controller *ctrl);
irqreturn_t calculet_irqhandler(int irq, void *data);

/* fops.c */
extern const struct drm_driver calculet_drm_driver;

/* proc.c */
int calculet_proc_init(void);
void calculet_proc_cleanup(void);
int calculet_proc_add_device(struct calculet_pci_board *board);
void calculet_proc_remove_device(struct calculet_pci_board *board);
int calculet_proc_add_process_ref(struct calculet_pci_board *board,
				  struct task_struct *task,
				  struct pid *pid);
int calculet_proc_add_process(struct calculet_pci_board *board,
			      struct task_struct *task);
void calculet_proc_remove_process(struct calculet_pci_board *board, u32 pid);
void calculet_proc_remove_process_ref(struct calculet_pci_board *board,
				      struct pid *pid, u32 pid_nr);
void calculet_proc_cleanup_processes(struct calculet_pci_board *board);
void calculet_proc_update_process_access(struct calculet_pci_board *board,
					 u32 pid);

/* fops.c helpers used by the core probe path. */
int calculet_pcie_bar32_transfer(struct calculet_pci_board *board,
				 struct calculet_bar32_transfer_params *p);
int calculet_pcie_bar64_transfer(struct calculet_pci_board *board,
				 struct calculet_bar64_transfer_params *p);

/* dma.c entry points used by the core. */
int calculet_pcie_dma_controller_init(struct calculet_pci_board *board);
int calculet_pcie_enable_poll(struct pci_dev *pdev,
			      struct calculet_pci_res *res);
void calculet_dma_stop_poll_thread(struct calculet_dma_controller *ctrl,
				   bool poll);
int calculet_dma_read_profile_status(struct calculet_dma_channel *ch,
				     bool is_write, u8 *done, u8 *error);

/* Module parameters and globals shared between files. */
extern int calculet_log_level;
extern unsigned int dma_timeout_ms;
extern unsigned int max_boards;
extern unsigned int poll_mode;
extern bool calculet_restrict_privileged_io;

/* Logging helpers; the binary tests calculet_log_level before every print. */
#define calculet_err(fmt, ...)						\
	do {								\
		if (calculet_log_level >= 3)				\
			pr_err("%scalculet: " fmt, "", ##__VA_ARGS__);	\
	} while (0)

#define calculet_warn(fmt, ...)						\
	do {								\
		if (calculet_log_level >= 4)				\
			pr_warn("%scalculet: " fmt, "", ##__VA_ARGS__);	\
	} while (0)

#define calculet_info(fmt, ...)						\
	do {								\
		if (calculet_log_level >= 6)				\
			pr_info("%scalculet: " fmt, "", ##__VA_ARGS__);	\
	} while (0)

#define calculet_dbg(fmt, ...)						\
	do {								\
		if (calculet_log_level >= 7)				\
			pr_debug("%scalculet: " fmt, "", ##__VA_ARGS__);\
	} while (0)

/* Layout checks for the userspace-visible structures. */
static_assert(sizeof(struct calculet_bar32_transfer_params) == 0x18,
	      "bar32 ABI size");
static_assert(sizeof(struct calculet_bar64_transfer_params) == 0x18,
	      "bar64 ABI size");
static_assert(sizeof(struct calculet_dma_transfer_channels_params) == 0x20,
	      "dma ABI size");
static_assert(sizeof(struct calculet_sn_info) == 0x104, "sn ABI size");
static_assert(sizeof(struct calculet_process_info_user) == 0x20,
	      "process entry ABI size");
static_assert(sizeof(struct calculet_process_info_list_user) == 0xc88,
	      "process list ABI size");
static_assert(sizeof(struct calculet_board_info) == 0x390,
	      "board info ABI size");
static_assert(DRM_COMMAND_END - DRM_COMMAND_BASE > CALCULET_GET_PROCESS_LIST,
	      "ioctl range");
/*
 * Kernel-internal (board/DMA/channel) offsets are config-dependent: mutex,
 * wait_queue_head_t and spinlock sizes change with lockdep and debug options.
 * The offsets quoted in the datasheet describe the vendor build; the source
 * keeps the same field order but does not pin those internal offsets.
 */

#endif /* _CALCULET_PCI_H */
