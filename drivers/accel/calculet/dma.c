// SPDX-License-Identifier: GPL-2.0-only
/*
 * DMA channels, coherent staging buffers and completion handling.
 */

#include <linux/dma-mapping.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "calculet_pci.h"

static int calculet_set_dma_mask(struct device *dev)
{
	static const u64 masks[] = {
		DMA_BIT_MASK(64), DMA_BIT_MASK(48), DMA_BIT_MASK(40),
		DMA_BIT_MASK(36), DMA_BIT_MASK(32),
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(masks); i++)
		if (!dma_set_mask_and_coherent(dev, masks[i]))
			return 0;

	return -EINVAL;
}

static void calculet_channel_init(struct calculet_dma_channel *ch,
				  struct calculet_dma_controller *ctrl,
				  unsigned int index)
{
	memset(ch, 0, sizeof(*ch));
	ch->index = index;
	ch->vector = 0xff;
	ch->state = CALCULET_DMA_IDLE;
	ch->ctrl = ctrl;
	ch->board = ctrl->board;
	spin_lock_init(&ch->state_lock);
	mutex_init(&ch->buf_lock);
	mutex_init(&ch->ioctl_lock);
	atomic_set(&ch->done, 0);
	init_waitqueue_head(&ch->wq);
}

static void calculet_poll_free(struct calculet_dma_controller *ctrl)
{
	struct calculet_dma_thread *thread = ctrl->thread;
	struct calculet_dma_poll *poll;

	if (!thread)
		return;

	poll = thread->poll;
	if (poll) {
		if (poll->status)
			dma_free_coherent(ctrl->dev, sizeof(u32), poll->status,
					  poll->phys);
		kfree(poll);
	}
	kfree(thread);
	ctrl->thread = NULL;
}

int calculet_dma_init(struct calculet_dma_controller *ctrl, struct device *dev,
		      bool poll)
{
	struct calculet_dma_thread *thread;
	struct calculet_dma_poll *status;
	int i, ret;

	if (!ctrl || !dev)
		return -EINVAL;

	if (READ_ONCE(ctrl->enabled) || READ_ONCE(ctrl->accepting) ||
	    (ctrl->thread && ctrl->thread->poll &&
	     ctrl->thread->poll->status))
		return -EBUSY;

	ctrl->dev = dev;
	WRITE_ONCE(ctrl->accepting, false);
	WRITE_ONCE(ctrl->enabled, false);

	if (!READ_ONCE(ctrl->sync_initialized)) {
		mutex_init(&ctrl->lock);
		init_waitqueue_head(&ctrl->active_wq);
		WRITE_ONCE(ctrl->sync_initialized, true);
	}
	atomic_set(&ctrl->active, 0);
	if (!ctrl->timeout_ms)
		ctrl->timeout_ms = CALCULET_DEFAULT_DMA_TIMEOUT_MS;

	ret = calculet_set_dma_mask(dev);
	if (ret)
		return ret;

	for (i = 0; i < CALCULET_READ_CHANNELS; i++)
		calculet_channel_init(&ctrl->rd[i], ctrl, i);
	for (i = 0; i < CALCULET_WRITE_CHANNELS; i++)
		calculet_channel_init(&ctrl->wr[i], ctrl, i);
	for (i = 0; i < CALCULET_TOTAL_CHANNELS; i++)
		calculet_channel_init(&ctrl->ep[i], ctrl, i);

	if (poll) {
		if (!ctrl->thread) {
			thread = kzalloc_obj(*thread);
			if (!thread)
				return -ENOMEM;
			ctrl->thread = thread;
		}
		thread = ctrl->thread;

		if (!thread->poll) {
			status = kzalloc_obj(*status);
			if (!status)
				goto err_thread;
			thread->poll = status;
		}
		status = thread->poll;

		status->status = dma_alloc_coherent(dev, sizeof(u32),
						    &status->phys, GFP_KERNEL);
		if (!status->status)
			goto err_thread;
		WRITE_ONCE(*status->status, 0);

		spin_lock_init(&status->lock);
		atomic_set(&status->active, 0);
		for (i = 0; i < CALCULET_TOTAL_CHANNELS; i++)
			status->map[i] = i + 100;

		thread->interval_ms = 1;
		atomic_set(&thread->stop, 0);
		init_completion(&thread->done);
	}

	WRITE_ONCE(ctrl->enabled, true);
	WRITE_ONCE(ctrl->accepting, true);

	if (poll) {
		thread->task = kthread_run(calculet_dma_poll_thread, ctrl,
					   "calculet_%s", dev_name(dev));
		if (IS_ERR(thread->task)) {
			ret = PTR_ERR(thread->task);
			thread->task = NULL;
			WRITE_ONCE(ctrl->enabled, false);
			WRITE_ONCE(ctrl->accepting, false);
			calculet_poll_free(ctrl);
			return ret;
		}
	}

	return 0;

err_thread:
	calculet_poll_free(ctrl);
	return -ENOMEM;
}

void calculet_dma_stop(struct calculet_dma_controller *ctrl)
{
	if (!ctrl)
		return;

	if (!READ_ONCE(ctrl->sync_initialized)) {
		WRITE_ONCE(ctrl->accepting, false);
		return;
	}

	guard(mutex)(&ctrl->lock);
	WRITE_ONCE(ctrl->accepting, false);
}

void calculet_dma_wait(struct calculet_dma_controller *ctrl)
{
	if (!ctrl || !READ_ONCE(ctrl->sync_initialized))
		return;

	wait_event(ctrl->active_wq, atomic_read(&ctrl->active) == 0);
}

void calculet_dma_stop_poll_thread(struct calculet_dma_controller *ctrl)
{
	struct calculet_dma_thread *thread;

	if (!ctrl)
		return;

	thread = ctrl->thread;
	if (!thread || !thread->task)
		return;

	atomic_set(&thread->stop, 1);
	kthread_stop(thread->task);
	wait_for_completion(&thread->done);
	thread->task = NULL;
}

void calculet_dma_fail(struct calculet_dma_controller *ctrl, int error)
{
	bool was_enabled;
	int i;

	if (!ctrl)
		return;
	if (error >= 0)
		error = -EIO;
	if (!READ_ONCE(ctrl->sync_initialized))
		return;

	was_enabled = READ_ONCE(ctrl->enabled);

	{
		guard(mutex)(&ctrl->lock);
		WRITE_ONCE(ctrl->accepting, false);
		WRITE_ONCE(ctrl->enabled, false);

		if (was_enabled) {
			for (i = 0; i < CALCULET_READ_CHANNELS; i++) {
				struct calculet_dma_channel *ch = &ctrl->rd[i];

				if (ch->state != CALCULET_DMA_BUSY &&
				    ch->state != CALCULET_DMA_COMPLETING)
					continue;
				ch->poisoned = true;
				calculet_dma_channel_set_state(ch,
							       CALCULET_DMA_ERROR);
				atomic_set(&ch->done, error);
				wake_up_interruptible(&ch->wq);
			}
			for (i = 0; i < CALCULET_WRITE_CHANNELS; i++) {
				struct calculet_dma_channel *ch = &ctrl->wr[i];

				if (ch->state != CALCULET_DMA_BUSY &&
				    ch->state != CALCULET_DMA_COMPLETING)
					continue;
				ch->poisoned = true;
				calculet_dma_channel_set_state(ch,
							       CALCULET_DMA_ERROR);
				atomic_set(&ch->done, error);
				wake_up_interruptible(&ch->wq);
			}
			for (i = 0; i < CALCULET_TOTAL_CHANNELS; i++) {
				atomic_set(&ctrl->ep[i].done, error);
				wake_up_interruptible(&ctrl->ep[i].wq);
			}
		}
	}

	wake_up_all(&ctrl->active_wq);
}

void calculet_dma_deinit(struct calculet_dma_controller *ctrl)
{
	int i;

	if (!ctrl || !ctrl->dev)
		return;

	calculet_dma_stop(ctrl);
	calculet_dma_wait(ctrl);
	calculet_dma_stop_poll_thread(ctrl);
	WRITE_ONCE(ctrl->enabled, false);

	for (i = 0; i < CALCULET_READ_CHANNELS; i++) {
		struct calculet_dma_channel *ch = &ctrl->rd[i];

		calculet_dma_channel_set_state(ch, CALCULET_DMA_DISABLED);
		{
			guard(mutex)(&ch->buf_lock);
			if (ch->buf) {
				dma_free_coherent(ctrl->dev, ch->buf_len,
						  ch->buf, ch->buf_dma);
				ch->buf = NULL;
				ch->buf_len = 0;
			}
		}
	}
	for (i = 0; i < CALCULET_WRITE_CHANNELS; i++) {
		struct calculet_dma_channel *ch = &ctrl->wr[i];

		calculet_dma_channel_set_state(ch, CALCULET_DMA_DISABLED);
		{
			guard(mutex)(&ch->buf_lock);
			if (ch->buf) {
				dma_free_coherent(ctrl->dev, ch->buf_len,
						  ch->buf, ch->buf_dma);
				ch->buf = NULL;
				ch->buf_len = 0;
			}
		}
	}

	calculet_poll_free(ctrl);
}

void calculet_dma_set_timeout(struct calculet_dma_controller *ctrl,
			      unsigned int timeout_ms)
{
	if (!ctrl)
		return;

	ctrl->timeout_ms = timeout_ms ? timeout_ms :
			   CALCULET_DEFAULT_DMA_TIMEOUT_MS;
}

void calculet_dma_channel_set_state(struct calculet_dma_channel *ch,
				    enum calculet_dma_channel_state state)
{
	unsigned long flags;
	enum calculet_dma_channel_state old;

	if (!ch)
		return;

	spin_lock_irqsave(&ch->state_lock, flags);
	old = ch->state;
	if (old != state)
		WRITE_ONCE(ch->state, state);
	spin_unlock_irqrestore(&ch->state_lock, flags);

	if (old != state && ch->board)
		calculet_dbg(ch->board, "DMA channel %u state %d -> %d\n",
			     ch->index, old, state);
}

static int calculet_dma_buffer(struct device *dev,
			       struct calculet_dma_channel *ch)
{
	void *buf;
	dma_addr_t dma;
	size_t len = ch->xfer_size;

	if (!len || len > CALCULET_DMA_MAX_SIZE)
		return -EINVAL;

	guard(mutex)(&ch->buf_lock);
	if (ch->buf && ch->buf_len >= len)
		return 0;

	buf = dma_alloc_coherent(dev, len, &dma, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	if (ch->buf)
		dma_free_coherent(dev, ch->buf_len, ch->buf, ch->buf_dma);
	ch->buf = buf;
	ch->buf_len = len;
	ch->buf_dma = dma;

	return 0;
}

static bool calculet_reg_valid(struct calculet_resource *res, size_t off)
{
	return res && res->addr && off <= res->size &&
	       sizeof(u32) <= res->size - off;
}

static void calculet_dma_ack_status(struct calculet_dma_channel *ch,
				    bool is_write)
{
	const struct calculet_chip_profile *profile = ch->board->profile;

	if (profile->ack_dma_status)
		profile->ack_dma_status(ch->board, is_write, ch->index);
}

static int calculet_dma_read_status(struct calculet_dma_channel *ch,
				    bool is_write, bool *done, bool *error)
{
	const struct calculet_chip_profile *profile = ch->board->profile;

	*done = true;
	*error = false;
	if (!profile->read_dma_status)
		return 0;
	return profile->read_dma_status(ch->board, is_write, ch->index, done,
					error);
}

static void calculet_dma_poison(struct calculet_dma_channel *ch, bool is_write)
{
	const struct calculet_chip_profile *profile = ch->board->profile;

	if (ch->poisoned)
		return;
	if (profile->abort_dma &&
	    profile->abort_dma(ch->board, is_write, ch->index) == 0)
		return;

	ch->poisoned = true;
	calculet_dma_channel_set_state(ch, CALCULET_DMA_ERROR);
}

static int calculet_dma_wait_done(struct calculet_dma_channel *ch)
{
	unsigned int timeout_ms = ch->ctrl->timeout_ms ?
				  ch->ctrl->timeout_ms :
				  CALCULET_DEFAULT_DMA_TIMEOUT_MS;
	long ret;

	ret = wait_event_interruptible_timeout(ch->wq,
					       atomic_read(&ch->done) != 0,
					       msecs_to_jiffies(timeout_ms));
	/* Ignore late completions once the waiter is done. */
	smp_store_release(&ch->armed, false);

	if (ret > 0)
		return atomic_read(&ch->done);
	if (ret == 0)
		return -ETIMEDOUT;
	return -ERESTARTSYS;
}

static long calculet_dma_h2c(struct device *dev,
			     struct calculet_dma_channel *ch,
			     struct calculet_resource *res)
{
	struct calculet_board *board = ch->board;
	unsigned int n = ch->index;
	int ret;

	if (!calculet_board_is_operational(board))
		return -ENODEV;
	if (!calculet_reg_valid(res, CALCULET_DMA_RD_CTRL(n)) ||
	    !calculet_reg_valid(res, CALCULET_DMA_RD_SIZE(n)) ||
	    !calculet_reg_valid(res, CALCULET_DMA_RD_SAR_LO(n)) ||
	    !calculet_reg_valid(res, CALCULET_DMA_RD_SAR_HI(n)) ||
	    !calculet_reg_valid(res, CALCULET_DMA_RD_DAR_LO(n)) ||
	    !calculet_reg_valid(res, CALCULET_DMA_RD_DAR_HI(n)) ||
	    !calculet_reg_valid(res, CALCULET_DMA_RD_DOORBELL))
		return -EINVAL;

	ret = calculet_dma_buffer(dev, ch);
	if (ret)
		return ret;

	if (copy_from_user(ch->buf, ch->user_buf, ch->xfer_size))
		return -EFAULT;

	{
		guard(mutex)(&res->dma_lock);
		if (!READ_ONCE(ch->ctrl->accepting) ||
		    !READ_ONCE(ch->ctrl->enabled) ||
		    !calculet_board_is_operational(board))
			return -ENODEV;

		writel(0x10, res->addr + CALCULET_DMA_RD_CTRL(n));
		writel(lower_32_bits(ch->xfer_size),
		       res->addr + CALCULET_DMA_RD_SIZE(n));
		writel(lower_32_bits(ch->buf_dma),
		       res->addr + CALCULET_DMA_RD_SAR_LO(n));
		writel(upper_32_bits(ch->buf_dma),
		       res->addr + CALCULET_DMA_RD_SAR_HI(n));
		writel(lower_32_bits(ch->dev_addr),
		       res->addr + CALCULET_DMA_RD_DAR_LO(n));
		writel(upper_32_bits(ch->dev_addr),
		       res->addr + CALCULET_DMA_RD_DAR_HI(n));

		atomic_set(&ch->done, 0);
		/* Order the register writes before the doorbell. */
		dma_wmb();
		/* Publish the armed flag before the device can complete. */
		smp_store_release(&ch->armed, true);
		writel(n, res->addr + CALCULET_DMA_RD_DOORBELL);
	}

	ret = calculet_dma_wait_done(ch);
	if (ret == 1) {
		calculet_stats_h2c(board);
		return 0;
	}

	atomic64_inc(&board->stats.errors);
	calculet_dma_poison(ch, false);
	return ret;
}

static long calculet_dma_c2h(struct device *dev,
			     struct calculet_dma_channel *ch,
			     struct calculet_resource *res)
{
	struct calculet_board *board = ch->board;
	unsigned int n = ch->index;
	int ret;

	if (!calculet_board_is_operational(board))
		return -ENODEV;
	if (!calculet_reg_valid(res, CALCULET_DMA_WR_CTRL(n)) ||
	    !calculet_reg_valid(res, CALCULET_DMA_WR_SIZE(n)) ||
	    !calculet_reg_valid(res, CALCULET_DMA_WR_SAR_LO(n)) ||
	    !calculet_reg_valid(res, CALCULET_DMA_WR_SAR_HI(n)) ||
	    !calculet_reg_valid(res, CALCULET_DMA_WR_DAR_LO(n)) ||
	    !calculet_reg_valid(res, CALCULET_DMA_WR_DAR_HI(n)) ||
	    !calculet_reg_valid(res, CALCULET_DMA_WR_DOORBELL))
		return -EINVAL;

	ret = calculet_dma_buffer(dev, ch);
	if (ret)
		return ret;

	{
		guard(mutex)(&res->dma_lock);
		if (!READ_ONCE(ch->ctrl->accepting) ||
		    !READ_ONCE(ch->ctrl->enabled) ||
		    !calculet_board_is_operational(board))
			return -ENODEV;

		writel(0x10, res->addr + CALCULET_DMA_WR_CTRL(n));
		writel(lower_32_bits(ch->xfer_size),
		       res->addr + CALCULET_DMA_WR_SIZE(n));
		writel(lower_32_bits(ch->dev_addr),
		       res->addr + CALCULET_DMA_WR_SAR_LO(n));
		writel(upper_32_bits(ch->dev_addr),
		       res->addr + CALCULET_DMA_WR_SAR_HI(n));
		writel(lower_32_bits(ch->buf_dma),
		       res->addr + CALCULET_DMA_WR_DAR_LO(n));
		writel(upper_32_bits(ch->buf_dma),
		       res->addr + CALCULET_DMA_WR_DAR_HI(n));

		atomic_set(&ch->done, 0);
		/* Order the register writes before the doorbell. */
		dma_wmb();
		/* Publish the armed flag before the device can complete. */
		smp_store_release(&ch->armed, true);
		writel(n, res->addr + CALCULET_DMA_WR_DOORBELL);
	}

	ret = calculet_dma_wait_done(ch);
	if (ret == 1) {
		dma_rmb();
		if (copy_to_user(ch->user_buf, ch->buf, ch->xfer_size)) {
			atomic64_inc(&board->stats.errors);
			return -EFAULT;
		}
		calculet_stats_c2h(board);
		return 0;
	}

	atomic64_inc(&board->stats.errors);
	calculet_dma_poison(ch, true);
	return ret;
}

long calculet_dma_transfer(struct calculet_dma_controller *ctrl,
			   struct calculet_resource *res,
			   struct calculet_dma_transfer_params *params)
{
	struct calculet_dma_channel *ch;
	enum calculet_dma_channel_state state;
	bool is_write;
	long ret;

	if (!ctrl || !res || !params)
		return -EINVAL;

	if (params->direction != CALCULET_TRANSFER_C2H &&
	    params->direction != CALCULET_TRANSFER_H2C)
		return -EINVAL;
	if (!params->size || params->size > CALCULET_DMA_MAX_SIZE)
		return -EINVAL;
	if (!params->host_address)
		return -EFAULT;
	if (params->device_address & 3)
		dev_warn(ctrl->dev,
			 "device address 0x%llx is not 4-byte aligned\n",
			 params->device_address);

	is_write = params->direction == CALCULET_TRANSFER_H2C;
	if (params->channel >= (is_write ? CALCULET_READ_CHANNELS :
					  CALCULET_WRITE_CHANNELS))
		return -EINVAL;

	if (!READ_ONCE(ctrl->accepting) || !READ_ONCE(ctrl->enabled) ||
	    !res->addr || !res->size)
		return -ENODEV;

	ch = is_write ? &ctrl->rd[params->channel] :
			&ctrl->wr[params->channel];

	scoped_cond_guard(mutex_intr, return -ERESTARTSYS, &ch->ioctl_lock) {
		{
			guard(mutex)(&ctrl->lock);
			state = ch->state;
			if (!READ_ONCE(ctrl->accepting) ||
			    !READ_ONCE(ctrl->enabled) ||
			    state == CALCULET_DMA_DISABLED)
				return -ENODEV;
			if (state == CALCULET_DMA_BUSY ||
			    state == CALCULET_DMA_COMPLETING)
				return -EBUSY;
			if (state == CALCULET_DMA_ERROR)
				return -EIO;
			calculet_dma_channel_set_state(ch, CALCULET_DMA_BUSY);
			atomic_inc(&ctrl->active);
		}

		if (ch->poisoned) {
			calculet_dma_channel_set_state(ch, CALCULET_DMA_ERROR);
			atomic_dec(&ctrl->active);
			wake_up_all(&ctrl->active_wq);
			return -EIO;
		}

		ch->xfer_size = (size_t)params->size;
		ch->user_buf = (void __user *)params->host_address;
		ch->dev_addr = params->device_address;
		/* A stale completion must not satisfy this request. */
		smp_store_release(&ch->armed, false);

		if (is_write)
			ret = calculet_dma_h2c(ctrl->dev, ch, res);
		else
			ret = calculet_dma_c2h(ctrl->dev, ch, res);

		atomic_dec(&ctrl->active);
		wake_up_all(&ctrl->active_wq);
		if (ch->state != CALCULET_DMA_ERROR)
			calculet_dma_channel_set_state(ch, CALCULET_DMA_IDLE);

		return ret;
	}
}

static void calculet_dma_complete_polled(struct calculet_dma_channel *ch,
					 bool is_write)
{
	bool armed, done, error;
	int ret;

	if (!ch || !ch->ctrl || !READ_ONCE(ch->ctrl->enabled))
		return;

	/* Pair with the release store before the doorbell. */
	armed = smp_load_acquire(&ch->armed);
	ret = calculet_dma_read_status(ch, is_write, &done, &error);
	if (ch->board->profile->read_dma_status && !ret && (done || error))
		calculet_dma_ack_status(ch, is_write);

	if (!armed)
		return;
	if (ret || error)
		atomic_set(&ch->done, -EIO);
	else if (done)
		atomic_set(&ch->done, 1);
	else
		return;
	wake_up_interruptible(&ch->wq);
}

int calculet_dma_poll(struct calculet_dma_controller *ctrl)
{
	struct calculet_dma_poll *poll;
	int region = 0;
	int i;

	if (!ctrl || !ctrl->thread || !ctrl->thread->poll)
		return -EINVAL;

	poll = ctrl->thread->poll;
	if (!poll->status)
		return -ENODEV;

	spin_lock(&poll->lock);
	dma_rmb();
	region = (int)READ_ONCE(*poll->status);
	if (region)
		WRITE_ONCE(*poll->status, 0);
	spin_unlock(&poll->lock);

	if (!region)
		return 0;

	for (i = 0; i < CALCULET_TOTAL_CHANNELS; i++) {
		if (poll->map[i] != region)
			continue;
		if (i < CALCULET_READ_CHANNELS)
			calculet_dma_complete_polled(&ctrl->rd[i], false);
		else
			calculet_dma_complete_polled(
				&ctrl->wr[i - CALCULET_READ_CHANNELS], true);
		break;
	}

	return region;
}

int calculet_dma_msi_request(struct calculet_dma_controller *ctrl, u32 channel)
{
	struct calculet_dma_channel *ch;
	long ret;
	int status;

	if (!ctrl || channel >= CALCULET_TOTAL_CHANNELS)
		return -EINVAL;

	ch = &ctrl->ep[channel];
	if (!READ_ONCE(ctrl->enabled) || !ctrl->board ||
	    ch->vector < CALCULET_TOTAL_CHANNELS ||
	    ch->vector >= ctrl->board->irq_vectors)
		return -EOPNOTSUPP;

	scoped_cond_guard(mutex_try, return -EBUSY, &ch->ioctl_lock) {
		atomic_set(&ch->done, 0);
		ret = wait_event_interruptible_timeout(ch->wq,
						       atomic_read(&ch->done) != 0,
						       msecs_to_jiffies(CALCULET_MSI_WAIT_MS));
		if (ret > 0) {
			status = atomic_read(&ch->done);
			atomic_set(&ch->done, 0);
			if (status == 1)
				return 0;
			return status < 0 ? status : -EIO;
		}

		atomic_set(&ch->done, 0);
		if (signal_pending(current))
			return -ERESTARTSYS;
		return -ETIMEDOUT;
	}
}

irqreturn_t calculet_dma_irq(int irq, void *data)
{
	struct calculet_dma_channel *ch = data;
	struct calculet_dma_controller *ctrl;
	struct calculet_board *board;
	bool armed, done, error, is_write;
	int ret;

	if (!ch)
		return IRQ_NONE;

	ctrl = ch->ctrl;
	board = ch->board;
	if (!board || !ctrl || !READ_ONCE(ctrl->enabled))
		return IRQ_NONE;

	if (ch->vector < CALCULET_TOTAL_CHANNELS) {
		is_write = ch->vector >= CALCULET_READ_CHANNELS;
		/* Pair with the release store before the doorbell. */
		armed = smp_load_acquire(&ch->armed);

		if (board->profile->read_dma_status) {
			ret = calculet_dma_read_status(ch, is_write, &done,
						       &error);
			if (!ret && (done || error))
				calculet_dma_ack_status(ch, is_write);
			atomic64_inc(&board->stats.irqs);

			if (!armed)
				return IRQ_HANDLED;
			if (ret || error)
				atomic_set(&ch->done, -EIO);
			else if (done)
				atomic_set(&ch->done, 1);
			else
				return IRQ_HANDLED;
			wake_up_interruptible(&ch->wq);
			return IRQ_HANDLED;
		}

		if (!armed)
			return IRQ_NONE;
	}

	atomic_set(&ch->done, 1);
	wake_up_interruptible(&ch->wq);
	atomic64_inc(&board->stats.irqs);

	return IRQ_HANDLED;
}

int calculet_dma_poll_thread(void *data)
{
	struct calculet_dma_controller *ctrl = data;
	struct calculet_dma_thread *thread;

	if (!ctrl || !ctrl->thread)
		return 0;

	thread = ctrl->thread;
	if (!thread->poll || !thread->poll->status) {
		if (thread->poll)
			atomic_set(&thread->poll->active, 0);
		complete(&thread->done);
		return 0;
	}

	atomic_set(&thread->poll->active, 1);

	while (!kthread_should_stop() &&
	       !atomic_read(&thread->stop)) {
		calculet_dma_poll(ctrl);
		schedule_timeout_interruptible(
			msecs_to_jiffies(max(thread->interval_ms, 1U)));
	}

	atomic_set(&thread->poll->active, 0);
	complete(&thread->done);
	return 0;
}
