// SPDX-License-Identifier: GPL-2.0-only
/*
 * Board lifetime, state and index management.
 */

#include <linux/slab.h>

#include "calculet_pci.h"

static struct calculet_board_manager calculet_mgr;

void calculet_board_init(struct calculet_board *board)
{
	INIT_LIST_HEAD(&board->list);
	mutex_init(&board->op_mutex);
	atomic_set(&board->active_ops, 0);
	init_waitqueue_head(&board->op_wq);
	WRITE_ONCE(board->state, CALCULET_BOARD_UNKNOWN);

	board->interrupts_enabled = false;
	board->irq_vectors = 0;
	board->irq_handlers = 0;
	memset(board->irq_registered, 0, sizeof(board->irq_registered));

	atomic64_set(&board->stats.h2c_count, 0);
	atomic64_set(&board->stats.c2h_count, 0);
	atomic64_set(&board->stats.irqs, 0);
	atomic64_set(&board->stats.errors, 0);

	INIT_LIST_HEAD(&board->procs);
	mutex_init(&board->proc_lock);

	atomic_set(&board->reset_in_progress, 0);
	board->suspended = false;
	board->fail_closed = false;

	mutex_init(&board->res.dma.dma_lock);
	spin_lock_init(&board->res.dma.access_lock);
	WRITE_ONCE(board->res.valid, false);
}

void calculet_board_get(struct calculet_board *board)
{
	if (board)
		drm_dev_get(&board->drm);
}

void calculet_board_put(struct calculet_board *board)
{
	if (board)
		drm_dev_put(&board->drm);
}

void calculet_board_set_state(struct calculet_board *board,
			      enum calculet_board_state state)
{
	enum calculet_board_state old;

	if (!board)
		return;

	old = READ_ONCE(board->state);
	if (old == state)
		return;

	calculet_dbg(board, "state %d -> %d\n", old, state);
	WRITE_ONCE(board->state, state);
}

enum calculet_board_state calculet_board_get_state(struct calculet_board *board)
{
	return board ? READ_ONCE(board->state) : CALCULET_BOARD_UNKNOWN;
}

bool calculet_board_is_operational(struct calculet_board *board)
{
	if (!board)
		return false;
	if (calculet_board_get_state(board) != CALCULET_BOARD_ACTIVE)
		return false;
	return READ_ONCE(board->res.valid);
}

int calculet_board_begin_op(struct calculet_board *board)
{
	if (!board)
		return -ENODEV;

	guard(mutex)(&board->op_mutex);
	if (!calculet_board_is_operational(board))
		return -ENODEV;
	atomic_inc(&board->active_ops);
	return 0;
}

void calculet_board_end_op(struct calculet_board *board)
{
	if (!board)
		return;

	atomic_dec(&board->active_ops);
	wake_up_all(&board->op_wq);
}

void calculet_board_begin_remove(struct calculet_board *board)
{
	int allowed;

	if (!board)
		return;

	{
		guard(mutex)(&board->op_mutex);
		calculet_board_set_state(board, CALCULET_BOARD_REMOVING);
	}

	/* A soft reset is issued from an ioctl and the PCI core removes the
	 * device synchronously from that same call stack.
	 */
	allowed = atomic_read(&board->reset_in_progress) ? 1 : 0;
	wait_event(board->op_wq, atomic_read(&board->active_ops) <= allowed);
}

int calculet_board_mgr_init(void)
{
	if (calculet_mgr.initialized)
		return 0;

	INIT_LIST_HEAD(&calculet_mgr.list);
	mutex_init(&calculet_mgr.lock);
	xa_init_flags(&calculet_mgr.xa, XA_FLAGS_ALLOC);
	spin_lock_init(&calculet_mgr.slot_lock);
	calculet_mgr.initialized = true;

	return 0;
}

void calculet_board_mgr_cleanup(void)
{
	struct calculet_board *board, *tmp;

	if (!calculet_mgr.initialized)
		return;

	{
		guard(mutex)(&calculet_mgr.lock);
		list_for_each_entry_safe(board, tmp, &calculet_mgr.list, list) {
			list_del(&board->list);
			calculet_board_set_state(board,
						 CALCULET_BOARD_REMOVING);
			calculet_board_put(board);
		}
	}

	xa_destroy(&calculet_mgr.xa);
	calculet_mgr.initialized = false;
}

struct calculet_board *calculet_board_mgr_lookup(u32 index)
{
	struct calculet_board *board;

	if (!calculet_mgr.initialized)
		return NULL;

	guard(mutex)(&calculet_mgr.lock);
	board = xa_load(&calculet_mgr.xa, index);
	if (board)
		calculet_board_get(board);
	return board;
}

int calculet_board_mgr_get_all_boards(struct calculet_board **boards, int max)
{
	struct calculet_board *board;
	int count = 0;

	if (!calculet_mgr.initialized || !boards)
		return 0;

	guard(mutex)(&calculet_mgr.lock);
	list_for_each_entry(board, &calculet_mgr.list, list) {
		if (count >= max)
			break;
		if (!calculet_board_is_operational(board))
			continue;
		calculet_board_get(board);
		boards[count++] = board;
	}

	return count;
}

static int calculet_board_reuse_slot(struct calculet_board *board)
{
	int reserved = -1;
	int i;

	spin_lock(&calculet_mgr.slot_lock);
	for (i = 0; i < CALCULET_MAX_BOARDS; i++) {
		struct calculet_reset_slot *slot = &calculet_mgr.slots[i];

		if (!slot->valid)
			continue;
		if (slot->devfn != board->pdev->devfn ||
		    slot->bus != board->pdev->bus->number ||
		    slot->domain != pci_domain_nr(board->pdev->bus))
			continue;
		reserved = slot->index;
		slot->valid = false;
		break;
	}
	spin_unlock(&calculet_mgr.slot_lock);

	return reserved;
}

int calculet_board_mgr_add_board(struct calculet_board *board)
{
	unsigned int limit;
	int reserved;
	int index = -1;
	int ret = 0;

	if (!calculet_mgr.initialized || !board)
		return -EINVAL;

	limit = calculet_max_boards_limit();
	if (!limit)
		return -ENOSPC;

	reserved = calculet_board_reuse_slot(board);
	if (reserved >= (int)limit)
		reserved = -1;
	calculet_board_set_state(board, CALCULET_BOARD_ACTIVE);

	guard(mutex)(&calculet_mgr.lock);

	if (reserved >= 0) {
		u32 id = reserved;

		ret = xa_alloc(&calculet_mgr.xa, &id, board,
			       XA_LIMIT(reserved, reserved), GFP_KERNEL);
		if (!ret)
			index = id;
	}
	if (index < 0) {
		u32 id;

		ret = xa_alloc(&calculet_mgr.xa, &id, board,
			       XA_LIMIT(0, limit - 1), GFP_KERNEL);
		if (!ret)
			index = id;
	}
	if (index < 0)
		return ret ?: -ENOSPC;

	board->index = index;
	WRITE_ONCE(board->res.valid, true);

	list_add_tail(&board->list, &calculet_mgr.list);

	snprintf(board->proc_name, sizeof(board->proc_name), "device%u", index);
	ret = calculet_proc_add_device(board);
	if (ret) {
		list_del(&board->list);
		xa_erase(&calculet_mgr.xa, index);
		return ret;
	}

	/* The manager owns a reference until remove_board(). */
	calculet_board_get(board);

	calculet_dbg(board, "registered board %u\n", index);
	return 0;
}

void calculet_board_mgr_remove_board(struct calculet_board *board)
{
	if (!board)
		return;

	calculet_board_set_state(board, CALCULET_BOARD_REMOVING);

	{
		guard(mutex)(&calculet_mgr.lock);
		calculet_proc_cleanup_processes(board);
		calculet_proc_remove_device(board);

		list_del(&board->list);
		WRITE_ONCE(board->res.valid, false);
		xa_erase(&calculet_mgr.xa, board->index);
	}

	calculet_board_put(board);
}

int calculet_board_health_check(struct calculet_board *board)
{
	u64 total;
	u64 errors;

	if (!board || !calculet_board_is_operational(board))
		return -ENODEV;

	if (!board->pdev || pci_channel_offline(board->pdev))
		return -EIO;

	total = atomic64_read(&board->stats.h2c_count) +
		atomic64_read(&board->stats.c2h_count);
	errors = atomic64_read(&board->stats.errors);
	if (total && errors * 100 / total > 5)
		return -EFAULT;

	return 0;
}

int calculet_global_soft_reset_prepare(struct calculet_board *board)
{
	if (!board)
		return -EINVAL;

	if (atomic_cmpxchg(&board->reset_in_progress, 0, 1) != 0)
		return -EBUSY;

	return 0;
}

void calculet_global_soft_reset_complete(struct calculet_board *board)
{
	if (!board)
		return;

	atomic_set(&board->reset_in_progress, 0);
}

void calculet_board_reserve_slot(struct calculet_board *board)
{
	unsigned int limit;
	int i;

	if (!board || !board->pdev)
		return;

	limit = calculet_max_boards_limit();
	spin_lock(&calculet_mgr.slot_lock);
	for (i = 0; i < limit; i++) {
		struct calculet_reset_slot *slot = &calculet_mgr.slots[i];

		if (slot->valid)
			continue;
		slot->valid = true;
		slot->index = board->index;
		slot->devfn = board->pdev->devfn;
		slot->bus = board->pdev->bus->number;
		slot->domain = pci_domain_nr(board->pdev->bus);
		break;
	}
	spin_unlock(&calculet_mgr.slot_lock);
}

unsigned int calculet_max_boards_limit(void)
{
	return min_t(unsigned int, max_boards, CALCULET_MAX_BOARDS);
}
