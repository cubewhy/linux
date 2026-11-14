/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * CALCULET KS1 PCIe accel driver internal definitions.
 */
#ifndef _CALCULET_DRV_H
#define _CALCULET_DRV_H

#include <linux/atomic.h>
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
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/xarray.h>

#include <drm/calculet_accel.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_print.h>

#define CALCULET_DRV_NAME		"calculet"
#define CALCULET_PCI_VENDOR_ID		0x20f5
#define CALCULET_STRINGIFY_(x)		#x
#define CALCULET_STRINGIFY(x)		CALCULET_STRINGIFY_(x)
#define CALCULET_DRV_VER						\
	CALCULET_STRINGIFY(CALCULET_DRV_VER_MAJOR) "."			\
	CALCULET_STRINGIFY(CALCULET_DRV_VER_MINOR) "."			\
	CALCULET_STRINGIFY(CALCULET_DRV_VER_REVISION)
#define CALCULET_DEFAULT_DMA_TIMEOUT_MS	100000
#define CALCULET_DEFAULT_MAX_BOARDS	CALCULET_MAX_BOARDS
#define CALCULET_MSI_WAIT_MS		30000
#define CALCULET_MAX_IRQ_VECTORS	CALCULET_MAX_CHANNELS
#define CALCULET_DMA_MAX_SIZE		CALCULET_DMA_MAX_TRANSFER

#define CALCULET_DMA_BAR		0
#define CALCULET_RT_BAR			2
#define CALCULET_SMI_BAR		4

/* DMA engine registers in BAR0. */
#define CALCULET_DMA_WR_ENGINE_EN	0x0c
#define CALCULET_DMA_WR_DOORBELL	0x10
#define CALCULET_DMA_RD_ENGINE_EN	0x2c
#define CALCULET_DMA_RD_DOORBELL	0x30

#define CALCULET_DMA_WR_CTRL(ch)	(0x200 + (ch) * 0x200)
#define CALCULET_DMA_WR_SIZE(ch)	(0x208 + (ch) * 0x200)
#define CALCULET_DMA_WR_SAR_LO(ch)	(0x20c + (ch) * 0x200)
#define CALCULET_DMA_WR_SAR_HI(ch)	(0x210 + (ch) * 0x200)
#define CALCULET_DMA_WR_DAR_LO(ch)	(0x214 + (ch) * 0x200)
#define CALCULET_DMA_WR_DAR_HI(ch)	(0x218 + (ch) * 0x200)

#define CALCULET_DMA_RD_CTRL(ch)	(0x300 + (ch) * 0x200)
#define CALCULET_DMA_RD_SIZE(ch)	(0x308 + (ch) * 0x200)
#define CALCULET_DMA_RD_SAR_LO(ch)	(0x30c + (ch) * 0x200)
#define CALCULET_DMA_RD_SAR_HI(ch)	(0x310 + (ch) * 0x200)
#define CALCULET_DMA_RD_DAR_LO(ch)	(0x314 + (ch) * 0x200)
#define CALCULET_DMA_RD_DAR_HI(ch)	(0x318 + (ch) * 0x200)

/* MSI write-back registers in BAR0. */
#define CALCULET_DMA_WR_DONE_ADDR_LO	0x60
#define CALCULET_DMA_WR_DONE_ADDR_HI	0x64
#define CALCULET_DMA_WR_ABORT_ADDR_LO	0x68
#define CALCULET_DMA_WR_ABORT_ADDR_HI	0x6c
#define CALCULET_DMA_WR_DATA		0x70
#define CALCULET_DMA_RD_DONE_ADDR_LO	0xcc
#define CALCULET_DMA_RD_DONE_ADDR_HI	0xd0
#define CALCULET_DMA_RD_ABORT_ADDR_LO	0xd4
#define CALCULET_DMA_RD_ABORT_ADDR_HI	0xd8
#define CALCULET_DMA_RD_DATA		0xdc

/* RT status registers in BAR2. */
#define CALCULET_RT_OS_STATUS_REG	0x0223810c
#define CALCULET_RT_RT_STATUS_REG	0x02238250
#define CALCULET_RT_SOFT_RESET_REG	0x02238108

/* SMI registers in BAR4. */
#define CALCULET_SMI_PRODUCT_NAME	0x0000
#define CALCULET_SMI_FIRMWARE_VERSION	0x0020
#define CALCULET_SMI_PRODUCT_NUMBER	0x002c
#define CALCULET_SMI_TOTAL_MEMORY	0x004c
#define CALCULET_SMI_MEMORY_BANDWIDTH	0x0054
#define CALCULET_SMI_DDR_FREQUENCY	0x005c
#define CALCULET_SMI_MAX_POWER_LIMIT	0x0060
#define CALCULET_SMI_TEMP_LIMIT		0x0064
#define CALCULET_SMI_SERIAL_NUMBER	0x0200
#define CALCULET_SMI_CHIP_SN		0x4800
#define CALCULET_SMI_CHIP_STRIDE	0x400

#define CALCULET_DMA_H(addr)		((u32)((addr) >> 32))
#define CALCULET_DMA_L(addr)		((u32)(addr))

struct calculet_board;
struct calculet_dma_controller;

enum calculet_board_state {
	CALCULET_BOARD_UNKNOWN = 0,
	CALCULET_BOARD_ACTIVE,
	CALCULET_BOARD_SUSPENDED,
	CALCULET_BOARD_REMOVING,
};

enum calculet_dma_channel_state {
	CALCULET_DMA_IDLE = 0,
	CALCULET_DMA_BUSY,
	CALCULET_DMA_COMPLETING,
	CALCULET_DMA_ERROR,
	CALCULET_DMA_DISABLED,
};

struct calculet_chip_profile {
	const char *name;
	u8 dma_bar;
	u8 rt_bar;
	u8 smi_bar;
	u8 read_channels;
	u8 write_channels;
	u32 os_status_reg;
	u32 rt_status_reg;
	u32 soft_reset_reg;

	int (*read_dma_status)(struct calculet_board *board, bool is_write,
			       unsigned int channel, bool *done, bool *error);
	void (*ack_dma_status)(struct calculet_board *board, bool is_write,
			       unsigned int channel);
	int (*abort_dma)(struct calculet_board *board, bool is_write,
			 unsigned int channel);
};

struct calculet_resource {
	resource_size_t phys;
	void __iomem *addr;
	size_t size;
	struct mutex dma_lock;
	spinlock_t access_lock;
};

struct calculet_pcie_resources {
	struct calculet_resource dma;
	struct calculet_resource rt;
	struct calculet_resource smi;
	bool valid;
	bool regions_requested;
};

struct calculet_stats {
	atomic64_t h2c_count;
	atomic64_t c2h_count;
	atomic64_t irqs;
	atomic64_t errors;
};

struct calculet_dma_channel {
	u8 index;
	u8 vector;
	int irq;
	enum calculet_dma_channel_state state;
	spinlock_t state_lock;
	atomic_t done;
	bool armed;
	struct calculet_dma_controller *ctrl;
	struct calculet_board *board;
	void __iomem *regs;

	size_t xfer_size;
	void __user *user_buf;
	u64 dev_addr;

	wait_queue_head_t wq;
	void *buf;
	size_t buf_len;
	dma_addr_t buf_dma;
	struct mutex buf_lock;
	bool poisoned;

	struct mutex ioctl_lock;
};

struct calculet_dma_poll {
	dma_addr_t phys;
	u32 *status;
	u32 map[CALCULET_TOTAL_CHANNELS];
	spinlock_t lock;
	atomic_t active;
};

struct calculet_dma_thread {
	struct calculet_dma_poll *poll;
	struct task_struct *task;
	u32 interval_ms;
	atomic_t stop;
	struct completion done;
};

struct calculet_dma_controller {
	struct device *dev;
	struct calculet_board *board;
	struct calculet_dma_channel rd[CALCULET_READ_CHANNELS];
	struct calculet_dma_channel wr[CALCULET_WRITE_CHANNELS];
	struct calculet_dma_channel ep[CALCULET_TOTAL_CHANNELS];
	struct calculet_dma_thread *thread;
	struct mutex lock;
	atomic_t active;
	wait_queue_head_t active_wq;
	unsigned int timeout_ms;
	bool accepting;
	bool enabled;
	bool sync_initialized;
};

struct calculet_process {
	struct list_head list;
	pid_t pid;
	pid_t ppid;
	struct pid *pid_ref;
	unsigned int open_count;
	kuid_t uid;
	char comm[TASK_COMM_LEN];
	bool active;
};

struct calculet_board {
	struct drm_device drm;
	struct list_head list;
	struct pci_dev *pdev;
	const struct calculet_chip_profile *profile;
	u32 index;

	struct mutex op_mutex;
	atomic_t active_ops;
	wait_queue_head_t op_wq;
	enum calculet_board_state state;

	struct calculet_pcie_resources res;
	struct calculet_dma_controller dma;
	bool interrupts_enabled;
	int irq_vectors;
	int irq_handlers;
	bool irq_registered[CALCULET_MAX_IRQ_VECTORS];

	struct calculet_stats stats;

	struct list_head procs;
	struct mutex proc_lock;
	struct proc_dir_entry *proc_entry;
	char proc_name[32];

	atomic_t reset_in_progress;

	bool suspended;
	bool fail_closed;
};

struct calculet_reset_slot {
	bool valid;
	u32 index;
	u32 devfn;
	u32 bus;
	int domain;
};

struct calculet_board_manager {
	struct list_head list;
	struct mutex lock;
	struct xarray xa;
	struct calculet_reset_slot slots[CALCULET_MAX_BOARDS];
	spinlock_t slot_lock;
	bool initialized;
};

extern unsigned int poll_mode;
extern unsigned int max_boards;
extern unsigned int dma_timeout_ms;
extern bool calculet_restrict_privileged_io;
extern const struct drm_driver calculet_drm_driver;

#define calculet_dbg(board, fmt, ...)					\
	drm_dbg(&(board)->drm, fmt, ##__VA_ARGS__)
#define calculet_info(board, fmt, ...)					\
	drm_info(&(board)->drm, fmt, ##__VA_ARGS__)
#define calculet_warn(board, fmt, ...)					\
	drm_warn(&(board)->drm, fmt, ##__VA_ARGS__)
#define calculet_err(board, fmt, ...)					\
	drm_err(&(board)->drm, fmt, ##__VA_ARGS__)

static inline void calculet_stats_h2c(struct calculet_board *board)
{
	atomic64_inc(&board->stats.h2c_count);
}

static inline void calculet_stats_c2h(struct calculet_board *board)
{
	atomic64_inc(&board->stats.c2h_count);
}

/* board_mgr.c */
void calculet_board_init(struct calculet_board *board);
void calculet_board_get(struct calculet_board *board);
void calculet_board_put(struct calculet_board *board);
void calculet_board_set_state(struct calculet_board *board,
			      enum calculet_board_state state);
enum calculet_board_state calculet_board_get_state(struct calculet_board *board);
bool calculet_board_is_operational(struct calculet_board *board);
int calculet_board_begin_op(struct calculet_board *board);
void calculet_board_end_op(struct calculet_board *board);
void calculet_board_begin_remove(struct calculet_board *board);

int calculet_board_mgr_init(void);
void calculet_board_mgr_cleanup(void);
struct calculet_board *calculet_board_mgr_lookup(u32 index);
struct calculet_board *calculet_board_mgr_get_board(u32 index);
int calculet_board_mgr_get_all_boards(struct calculet_board **boards, int max);
int calculet_board_mgr_add_board(struct calculet_board *board);
void calculet_board_mgr_remove_board(struct calculet_board *board);
int calculet_board_health_check(struct calculet_board *board);
int calculet_global_soft_reset_prepare(struct calculet_board *board);
void calculet_global_soft_reset_complete(struct calculet_board *board);
void calculet_board_reserve_slot(struct calculet_board *board);
unsigned int calculet_max_boards_limit(void);

/* calculet_pci_core.c */
int calculet_resources_init(struct pci_dev *pdev,
			    struct calculet_pcie_resources *res);
void calculet_resources_release(struct pci_dev *pdev,
				struct calculet_pcie_resources *res);
u32 calculet_reg_read32(struct calculet_resource *res, u64 off);
u64 calculet_reg_read64(struct calculet_resource *res, u64 off);
void calculet_reg_write32(struct calculet_resource *res, u64 off, u32 val);
void calculet_reg_write64(struct calculet_resource *res, u64 off, u64 val);
void calculet_read_strings(char *buf, size_t buf_size,
			   struct calculet_resource *res, u64 off,
			   size_t len);
int calculet_enable_interrupts(struct calculet_board *board);
void calculet_disable_interrupts(struct calculet_board *board);
int calculet_activate_board(struct calculet_board *board);

/* dma.c */
int calculet_dma_init(struct calculet_dma_controller *ctrl, struct device *dev,
		      bool poll);
void calculet_dma_deinit(struct calculet_dma_controller *ctrl);
void calculet_dma_set_timeout(struct calculet_dma_controller *ctrl,
			      unsigned int timeout_ms);
void calculet_dma_stop(struct calculet_dma_controller *ctrl);
void calculet_dma_stop_poll_thread(struct calculet_dma_controller *ctrl);
void calculet_dma_wait(struct calculet_dma_controller *ctrl);
void calculet_dma_fail(struct calculet_dma_controller *ctrl, int error);
void calculet_dma_channel_set_state(struct calculet_dma_channel *ch,
				    enum calculet_dma_channel_state state);
long calculet_dma_transfer(struct calculet_dma_controller *ctrl,
			   struct calculet_resource *res,
			   struct calculet_dma_transfer_params *params);
int calculet_dma_poll(struct calculet_dma_controller *ctrl);
int calculet_dma_msi_request(struct calculet_dma_controller *ctrl, u32 channel);
irqreturn_t calculet_dma_irq(int irq, void *data);
int calculet_dma_poll_thread(void *data);

/* fops.c */
int calculet_open(struct drm_device *dev, struct drm_file *file);
void calculet_postclose(struct drm_device *dev, struct drm_file *file);

/* proc.c */
int calculet_proc_init(void);
void calculet_proc_cleanup(void);
int calculet_proc_add_device(struct calculet_board *board);
void calculet_proc_remove_device(struct calculet_board *board);
int calculet_proc_add_process_ref(struct calculet_board *board,
				  struct task_struct *task,
				  struct pid *pid_ref);
void calculet_proc_remove_process_ref(struct calculet_board *board,
				      struct pid *pid_ref, pid_t pid);
void calculet_proc_cleanup_processes(struct calculet_board *board);

#endif /* _CALCULET_DRV_H */
