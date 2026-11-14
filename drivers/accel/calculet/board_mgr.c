// SPDX-License-Identifier: GPL-2.0-only
/*
 * board_mgr.c - board lifetime, registry, reset slots and health checks.
 *
 * Reconstructed from calculet_pci.ko 1.1.4 (board_mgr.c strings at .rodata
 * 0x2b3 and 0x7f0..).
 */
#include <linux/module.h>
#include <linux/slab.h>

#include "calculet_pci.h"

struct calculet_reset_slot {
	bool reserved;		/* +0x00 */
	u32 board_index;	/* +0x04 */
	u32 devfn;		/* +0x08 */
	u8 bus;			/* +0x0c */
	u8 pad[3];		/* +0x0d */
	u32 domain;		/* +0x10 */
};				/* sizeof == 0x14 */

static struct calculet_board_mgr {
	struct list_head boards;
	struct mutex lock;
	spinlock_t idr_lock;
	struct idr idr;
	int board_count;
	struct {
		struct mutex lock;
		int active;
		int device_count;
		wait_queue_head_t reset_wq;
	} reset_ctrl;
	bool initialized;
} g_board_mgr;

static DEFINE_SPINLOCK(g_reset_slot_lock);
static struct calculet_reset_slot g_reset_slots[CALCULET_MAX_BOARDS];

unsigned int calculet_max_boards_limit(void)
{
	return min_t(unsigned int, max_boards, CALCULET_MAX_BOARDS);
}

int calculet_board_init(struct calculet_pci_board *board)
{
	if (!board)
		return -EINVAL;

	INIT_LIST_HEAD(&board->list);
	refcount_set(&board->refcount, 1);
	mutex_init(&board->state_lock);
	atomic_set(&board->active_ops, 0);
	init_waitqueue_head(&board->op_wq);
	board->state = CALCULET_BOARD_INVALID;

	board->irq_enabled = false;
	board->irq_count = 0;
	memset(&board->irq_used, 0, sizeof(board->irq_used));
	memset(&board->stat_ops, 0, sizeof(board->stat_ops));
	board->stat_errors = 0;
	board->stat_irqs = 0;
	board->stat_timeouts = 0;

	INIT_LIST_HEAD(&board->procs);
	mutex_init(&board->proc_lock);
	board->proc_count = 0;
	board->proc_entry = NULL;

	board->operational = false;
	board->regions_requested = false;
	board->reset_in_progress = false;
	board->soft_reset_pending = false;
	board->suspended = false;
	board->fail_closed = false;
	return 0;
}

struct calculet_pci_board *calculet_board_get(struct calculet_pci_board *board)
{
	if (!board)
		return NULL;
	if (!refcount_inc_not_zero(&board->refcount)) {
		refcount_warn_saturate(&board->refcount, REFCOUNT_ADD_UAF);
		return NULL;
	}
	return board;
}

void calculet_board_put(struct calculet_pci_board *board)
{
	if (!board)
		return;
	if (refcount_dec_and_test(&board->refcount))
		kfree(board);
}

void calculet_board_set_state(struct calculet_pci_board *board, int state)
{
	if (board && board->state != state)
		board->state = state;
}

int calculet_board_get_state(struct calculet_pci_board *board)
{
	return board ? board->state : CALCULET_BOARD_INVALID;
}

bool calculet_board_is_operational(struct calculet_pci_board *board)
{
	return board && board->state == CALCULET_BOARD_ACTIVE &&
	       board->operational;
}

int calculet_board_begin_op(struct calculet_pci_board *board)
{
	if (!board)
		return -ESRCH;

	mutex_lock(&board->state_lock);
	if (board->state == CALCULET_BOARD_ACTIVE && board->operational) {
		atomic_inc(&board->active_ops);
		mutex_unlock(&board->state_lock);
		return 0;
	}
	mutex_unlock(&board->state_lock);
	return -ESRCH;
}

void calculet_board_end_op(struct calculet_pci_board *board)
{
	if (!board)
		return;
	atomic_dec(&board->active_ops);
	wake_up_all(&board->op_wq);
}

void calculet_board_begin_remove(struct calculet_pci_board *board)
{
	if (!board)
		return;

	mutex_lock(&board->state_lock);
	board->state = CALCULET_BOARD_REMOVING;
	mutex_unlock(&board->state_lock);

	wait_event(board->op_wq, atomic_read(&board->active_ops) <= 0);
}

int calculet_board_mgr_init(void)
{
	if (g_board_mgr.initialized)
		return 0;

	INIT_LIST_HEAD(&g_board_mgr.boards);
	mutex_init(&g_board_mgr.lock);
	spin_lock_init(&g_board_mgr.idr_lock);
	idr_init(&g_board_mgr.idr);
	g_board_mgr.board_count = 0;
	mutex_init(&g_board_mgr.reset_ctrl.lock);
	g_board_mgr.reset_ctrl.active = 0;
	g_board_mgr.reset_ctrl.device_count = 0;
	init_waitqueue_head(&g_board_mgr.reset_ctrl.reset_wq);
	g_board_mgr.initialized = true;
	return 0;
}

void calculet_board_mgr_cleanup(void)
{
	if (!g_board_mgr.initialized)
		return;
	idr_destroy(&g_board_mgr.idr);
	memset(&g_board_mgr, 0, sizeof(g_board_mgr));
}

void calculet_board_mgr_put_board(struct calculet_pci_board *board)
{
	calculet_board_put(board);
}

static bool board_matches_reserved_slot(struct calculet_pci_board *board,
					const struct calculet_reset_slot *slot)
{
	struct pci_dev *pdev = board->pdev;
	struct pci_bus *bus = pdev->bus;

	return slot->reserved &&
	       slot->devfn == pdev->devfn &&
	       slot->bus == bus->number &&
	       slot->domain == pci_domain_nr(bus);
}

int calculet_board_mgr_add_board(struct calculet_pci_board *board)
{
	unsigned int limit = calculet_max_boards_limit();
	int index = -ENOSPC;
	u32 i;
	int ret;

	if (!g_board_mgr.initialized || !board)
		return -EINVAL;
	if (!limit)
		return -ENOSPC;

	spin_lock(&g_reset_slot_lock);
	for (i = 0; i < limit; i++) {
		if (!board_matches_reserved_slot(board, &g_reset_slots[i]))
			continue;
		index = g_reset_slots[i].board_index;
		g_reset_slots[i].reserved = false;
		spin_unlock(&g_reset_slot_lock);
		goto found;
	}
	spin_unlock(&g_reset_slot_lock);

found:
	mutex_lock(&g_board_mgr.lock);
	spin_lock(&g_board_mgr.idr_lock);
	if (index < 0)
		ret = idr_alloc(&g_board_mgr.idr, board, 0, limit, GFP_KERNEL);
	else
		ret = idr_alloc(&g_board_mgr.idr, board, index, index + 1,
				GFP_KERNEL);
	if (ret < 0 && index >= 0) {
		calculet_warn("Failed to reuse reserved_id=%d (err=%d), falling back to auto-alloc\n",
			      index, ret);
		ret = idr_alloc(&g_board_mgr.idr, board, 0, limit, GFP_KERNEL);
	}
	spin_unlock(&g_board_mgr.idr_lock);
	if (ret < 0) {
		mutex_unlock(&g_board_mgr.lock);
		calculet_err("Failed to allocate board index: %d\n", ret);
		return ret;
	}

	board->index = ret;
	calculet_board_set_state(board, CALCULET_BOARD_ACTIVE);
	board->operational = true;
	list_add_tail(&board->list, &g_board_mgr.boards);
	g_board_mgr.board_count++;
	g_board_mgr.reset_ctrl.device_count++;

	snprintf(board->proc_name, sizeof(board->proc_name), "device%u",
		 board->index);
	if (calculet_proc_add_device(board) < 0) {
		list_del(&board->list);
		g_board_mgr.board_count--;
		g_board_mgr.reset_ctrl.device_count--;
		spin_lock(&g_board_mgr.idr_lock);
		idr_remove(&g_board_mgr.idr, board->index);
		spin_unlock(&g_board_mgr.idr_lock);
		mutex_unlock(&g_board_mgr.lock);
		return -ENOMEM;
	}
	mutex_unlock(&g_board_mgr.lock);
	return board->index;
}

void calculet_board_mgr_remove_board(struct calculet_pci_board *board)
{
	if (!board)
		return;

	board->state = CALCULET_BOARD_REMOVING;
	mutex_lock(&g_board_mgr.lock);
	calculet_proc_cleanup_processes(board);
	calculet_proc_remove_device(board);
	list_del(&board->list);
	g_board_mgr.board_count--;
	g_board_mgr.reset_ctrl.device_count--;
	board->operational = false;
	spin_lock(&g_board_mgr.idr_lock);
	idr_remove(&g_board_mgr.idr, board->index);
	spin_unlock(&g_board_mgr.idr_lock);
	mutex_unlock(&g_board_mgr.lock);

	calculet_board_put(board);
	calculet_dbg("Board removed successfully\n");
}

int calculet_board_mgr_get_all_boards(struct calculet_pci_board **boards,
				      int max)
{
	struct calculet_pci_board *board;
	int count = 0;

	if (!g_board_mgr.initialized || !boards)
		return 0;

	mutex_lock(&g_board_mgr.lock);
	list_for_each_entry(board, &g_board_mgr.boards, list) {
		if (count >= max)
			break;
		if (board->state == CALCULET_BOARD_ACTIVE &&
		    board->operational) {
			boards[count++] = board;
			calculet_board_get(board);
		}
	}
	mutex_unlock(&g_board_mgr.lock);
	return count;
}

int calculet_board_health_check(struct calculet_pci_board *board)
{
	u64 total;
	unsigned long rate;

	if (!board) {
		calculet_err("Board is NULL or invalid\n");
		return -ESRCH;
	}
	if (!calculet_board_is_operational(board))
		return -ESRCH;
	if (!board->pdev || board->pdev->current_state != PCI_D0) {
		calculet_err("Board %u PCIe device is offline\n", board->index);
		return -EIO;
	}
	if (!board->operational) {
		calculet_err("Board %u resources are invalid\n", board->index);
		return -EINVAL;
	}

	total = board->stat_ops + board->stat_errors;
	if (total) {
		rate = div64_u64(board->stat_timeouts * 100, total);
		if (rate > 5) {
			calculet_warn("Board %u high error rate: %llu%%\n",
				      board->index, (u64)rate);
			return -EFAULT;
		}
	}
	return 0;
}

int calculet_global_soft_reset_prepare(struct calculet_pci_board *board)
{
	if (!board)
		return -EINVAL;

	if (cmpxchg(&board->reset_in_progress, false, true)) {
		calculet_warn("Reset already in progress for board %u\n",
			      board->index);
		return -EBUSY;
	}

	mutex_lock(&g_board_mgr.reset_ctrl.lock);
	g_board_mgr.reset_ctrl.active++;
	mutex_unlock(&g_board_mgr.reset_ctrl.lock);
	return 0;
}

void calculet_global_soft_reset_complete(struct calculet_pci_board *board)
{
	if (!board)
		return;

	mutex_lock(&g_board_mgr.reset_ctrl.lock);
	if (g_board_mgr.reset_ctrl.active > 0)
		g_board_mgr.reset_ctrl.active--;
	board->reset_in_progress = false;
	wake_up_all(&g_board_mgr.reset_ctrl.reset_wq);
	mutex_unlock(&g_board_mgr.reset_ctrl.lock);
}

bool calculet_global_soft_reset_can_rescan(void)
{
	return g_board_mgr.reset_ctrl.active == 0 &&
	       g_board_mgr.reset_ctrl.device_count == 0;
}

void calculet_board_reserve_slot(struct calculet_pci_board *board)
{
	unsigned int limit = calculet_max_boards_limit();
	struct pci_dev *pdev;
	struct pci_bus *bus;
	u32 i;

	if (!board || !board->pdev)
		return;

	pdev = board->pdev;
	bus = pdev->bus;

	spin_lock(&g_reset_slot_lock);
	for (i = 0; i < limit; i++) {
		if (g_reset_slots[i].reserved)
			continue;
		g_reset_slots[i].reserved = true;
		g_reset_slots[i].board_index = board->index;
		g_reset_slots[i].devfn = pdev->devfn;
		g_reset_slots[i].bus = bus->number;
		g_reset_slots[i].domain = pci_domain_nr(bus);
		break;
	}
	spin_unlock(&g_reset_slot_lock);
}
