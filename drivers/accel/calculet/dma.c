// SPDX-License-Identifier: GPL-2.0-only
/*
 * dma.c - DMA channel engine, poll-mode completion and the DMA ioctl.
 *
 * Reconstructed from calculet_pci.ko 1.1.4 (dma.c strings at .rodata 0x51f).
 *
 * Model:
 *   - BAR2 holds 8 read (H2C) and 8 write (C2H) channel register blocks.
 *   - Each transfer is staged through a coherent buffer owned by the channel.
 *   - Completion arrives either as an MSI (vector 0..15) or through a 4-byte
 *     coherent status word written by the device (poll mode).
 */
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/ktime.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "calculet_pci.h"

int calculet_dma_read_profile_status(struct calculet_dma_channel *ch,
					    bool is_write, u8 *done,
					    u8 *error)
{
	struct calculet_pci_board *board;
	const struct calculet_hw_profile *profile;

	if (!ch)
		return -EINVAL;

	*done = 1;
	*error = 0;

	board = ch->board;
	if (!board || !board->profile)
		return 0;
	profile = board->profile;
	if (!profile->dma_status)
		return 0;

	return profile->dma_status(board, is_write, ch->id, done, error);
}

static int calculet_dma_prepare_buffer(struct device *dev,
				       struct calculet_dma_channel *ch)
{
	dma_addr_t dma;
	void *buf;
	size_t len;

	if (!dev || !ch || !ch->length ||
	    ch->length > CALCULET_DMA_MAX_TRANSFER)
		return -EINVAL;

	len = ch->length;
	mutex_lock(&ch->buf_lock);
	if (!ch->buf || ch->buf_size < len) {
		buf = dma_alloc_coherent(dev, len, &dma, GFP_KERNEL);
		if (!buf) {
			mutex_unlock(&ch->buf_lock);
			return -ENOMEM;
		}
		if (ch->buf)
			dma_free_coherent(dev, ch->buf_size, ch->buf,
					  ch->buf_dma);
		ch->buf = buf;
		ch->buf_size = len;
		ch->buf_dma = dma;
	}
	mutex_unlock(&ch->buf_lock);
	return 0;
}

static void calculet_dma_release_buffer(struct device *dev,
					struct calculet_dma_channel *ch)
{
	if (!dev || !ch)
		return;
	mutex_lock(&ch->buf_lock);
	if (ch->buf) {
		dma_free_coherent(dev, ch->buf_size, ch->buf, ch->buf_dma);
		ch->buf = NULL;
		ch->buf_size = 0;
		ch->buf_dma = 0;
	}
	mutex_unlock(&ch->buf_lock);
}

static int calculet_dma_abort_channel(struct calculet_dma_channel *ch,
				      bool is_write)
{
	struct calculet_pci_board *board = ch->board;
	const struct calculet_hw_profile *profile;

	if (!board || !board->profile)
		return -EINVAL;
	profile = board->profile;
	if (!profile->dma_abort)
		return -EOPNOTSUPP;

	return profile->dma_abort(board, is_write, ch->id);
}

static void calculet_dma_poison_if_abort_failed(struct calculet_dma_channel *ch,
						bool is_write)
{
	unsigned long flags;

	if (!ch || ch->poisoned)
		return;
	if (calculet_dma_abort_channel(ch, is_write) == 0)
		return;

	ch->poisoned = true;
	spin_lock_irqsave(&ch->lock, flags);
	if (ch->state != CALCULET_DMA_FAILED) {
		ch->state = CALCULET_DMA_FAILED;
		ch->started_at = ktime_get();
	}
	spin_unlock_irqrestore(&ch->lock, flags);
}

void calculet_dma_channel_set_state(struct calculet_dma_channel *ch, int state)
{
	unsigned long flags;

	if (!ch)
		return;
	spin_lock_irqsave(&ch->lock, flags);
	if (ch->state != state) {
		ch->state = state;
		ch->started_at = ktime_get();
	}
	spin_unlock_irqrestore(&ch->lock, flags);
}

void calculet_dma_controller_set_timeout(struct calculet_dma_controller *ctrl,
					 u32 timeout_ms)
{
	if (ctrl)
		ctrl->timeout_ms = timeout_ms ? timeout_ms
					      : CALCULET_DEFAULT_DMA_TIMEOUT_MS;
}

static void calculet_dma_init_channel(struct calculet_dma_channel *ch,
				      struct calculet_dma_controller *ctrl,
				      u8 id, u8 vector, bool is_write)
{
	memset(ch, 0, sizeof(*ch));
	ch->id = id;
	ch->vector = vector;
	ch->is_write = is_write;
	ch->ctrl = ctrl;
	ch->board = ctrl->board;
	ch->os_base = ctrl->board->res[CALCULET_RES_OS].io;
	spin_lock_init(&ch->lock);
	init_waitqueue_head(&ch->wq);
	mutex_init(&ch->buf_lock);
	mutex_init(&ch->ioctl_lock);
	ch->state = CALCULET_DMA_IDLE;
}

int calculet_dma_controller_init(struct calculet_dma_controller *ctrl,
				 struct device *dev, bool poll)
{
	struct calculet_poll_ctx *ctx;
	struct calculet_poll_status *status;
	int ret, i;

	if (!ctrl || !dev) {
		calculet_err("Controller or device pointer is NULL\n");
		return -EINVAL;
	}
	if (ctrl->ready || ctrl->accepting ||
	    (ctrl->poll && ctrl->poll->status && ctrl->poll->status->ptr))
		return -EBUSY;

	ctrl->dev = dev;
	if (!ctrl->initialized) {
		mutex_init(&ctrl->state_lock);
		init_waitqueue_head(&ctrl->active_wq);
		ctrl->initialized = true;
	}
	atomic_set(&ctrl->active, 0);
	if (!ctrl->timeout_ms)
		ctrl->timeout_ms = CALCULET_DEFAULT_DMA_TIMEOUT_MS;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (ret) {
		ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(40));
		if (ret) {
			ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(36));
			if (ret) {
				ret = dma_set_mask_and_coherent(dev,
								DMA_BIT_MASK(32));
				if (ret) {
					calculet_err("Error enabling dma %d\n",
						     ret);
					return ret;
				}
				calculet_info("Enabled 32 bit dma\n");
			}
		}
	} else {
		calculet_info("Enabled 64 bit dma\n");
	}

	for (i = 0; i < CALCULET_READ_CHANNELS; i++)
		calculet_dma_init_channel(&ctrl->rd[i], ctrl, i, i, false);
	for (i = 0; i < CALCULET_WRITE_CHANNELS; i++)
		calculet_dma_init_channel(&ctrl->wr[i], ctrl, i, 8 + i, true);
	for (i = 0; i < 16; i++)
		calculet_dma_init_channel(&ctrl->ep[i], ctrl, i, 16 + i, false);

	if (poll) {
		ctx = ctrl->poll;
		if (!ctx) {
			ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
			if (!ctx)
				return -ENOMEM;
			ctrl->poll = ctx;
		}
		status = ctx->status;
		if (!status) {
			status = devm_kzalloc(dev, sizeof(*status), GFP_KERNEL);
			if (!status)
				return -ENOMEM;
			ctx->status = status;
		}
		if (!status->ptr) {
			status->ptr = dma_alloc_coherent(dev, sizeof(u32),
							 &status->dma,
							 GFP_KERNEL);
			if (!status->ptr) {
				calculet_err("Failed to allocate coherent memory for status region\n");
				return -ENOMEM;
			}
		}
		*status->ptr = 0;
		status->running = 0;
		for (i = 0; i < 16; i++)
			status->map[i] = 100 + i;
		spin_lock_init(&status->lock);
	}

	ctrl->ready = true;
	ctrl->accepting = true;
	return 0;
}

void calculet_dma_stop_new_transfers(struct calculet_dma_controller *ctrl)
{
	if (!ctrl)
		return;
	mutex_lock(&ctrl->state_lock);
	ctrl->accepting = false;
	mutex_unlock(&ctrl->state_lock);
}

void calculet_dma_wait_for_transfers(struct calculet_dma_controller *ctrl)
{
	if (!ctrl)
		return;
	wait_event(ctrl->active_wq, atomic_read(&ctrl->active) == 0);
}

void calculet_dma_fail_active_transfers(struct calculet_dma_controller *ctrl,
					int status)
{
	unsigned long flags;
	int i;

	if (!ctrl)
		return;
	if (status >= 0)
		status = -EIO;

	if (ctrl->initialized)
		mutex_lock(&ctrl->state_lock);
	ctrl->accepting = false;
	ctrl->ready = false;

	for (i = 0; i < CALCULET_READ_CHANNELS; i++) {
		struct calculet_dma_channel *ch = &ctrl->rd[i];

		if (ch->state != CALCULET_DMA_ACTIVE &&
		    ch->state != CALCULET_DMA_COMPLETED)
			continue;
		ch->poisoned = true;
		spin_lock_irqsave(&ch->lock, flags);
		if (ch->state != CALCULET_DMA_FAILED) {
			ch->state = CALCULET_DMA_FAILED;
			ch->started_at = ktime_get();
		}
		spin_unlock_irqrestore(&ch->lock, flags);
		WRITE_ONCE(ch->result, status);
		wake_up(&ch->wq);
	}
	for (i = 0; i < CALCULET_WRITE_CHANNELS; i++) {
		struct calculet_dma_channel *ch = &ctrl->wr[i];

		if (ch->state != CALCULET_DMA_ACTIVE &&
		    ch->state != CALCULET_DMA_COMPLETED)
			continue;
		ch->poisoned = true;
		spin_lock_irqsave(&ch->lock, flags);
		if (ch->state != CALCULET_DMA_FAILED) {
			ch->state = CALCULET_DMA_FAILED;
			ch->started_at = ktime_get();
		}
		spin_unlock_irqrestore(&ch->lock, flags);
		WRITE_ONCE(ch->result, status);
		wake_up(&ch->wq);
	}
	for (i = 0; i < 16; i++) {
		WRITE_ONCE(ctrl->ep[i].result, status);
		wake_up(&ctrl->ep[i].wq);
	}
	if (ctrl->initialized)
		mutex_unlock(&ctrl->state_lock);
	wake_up_all(&ctrl->active_wq);
}

void calculet_dma_stop_poll_thread(struct calculet_dma_controller *ctrl,
				   bool poll)
{
	struct calculet_poll_ctx *ctx;

	if (!ctrl || !poll || !ctrl->poll)
		return;
	ctx = ctrl->poll;
	if (!ctx->thread)
		return;
	ctx->stop = 1;
	kthread_stop(ctx->thread);
	wait_for_completion(&ctx->stopped);
	ctx->thread = NULL;
}

void calculet_dma_controller_deinit(struct calculet_dma_controller *ctrl,
				    bool poll)
{
	int i;

	if (!ctrl || !ctrl->dev)
		return;

	mutex_lock(&ctrl->state_lock);
	ctrl->accepting = false;
	mutex_unlock(&ctrl->state_lock);
	calculet_dma_wait_for_transfers(ctrl);
	calculet_dma_stop_poll_thread(ctrl, poll);

	ctrl->ready = false;
	for (i = 0; i < CALCULET_READ_CHANNELS; i++) {
		calculet_dma_channel_set_state(&ctrl->rd[i],
					       CALCULET_DMA_DEAD);
		calculet_dma_release_buffer(ctrl->dev, &ctrl->rd[i]);
	}
	for (i = 0; i < CALCULET_WRITE_CHANNELS; i++) {
		calculet_dma_channel_set_state(&ctrl->wr[i],
					       CALCULET_DMA_DEAD);
		calculet_dma_release_buffer(ctrl->dev, &ctrl->wr[i]);
	}

	if (ctrl->poll && ctrl->poll->status) {
		struct calculet_poll_status *status = ctrl->poll->status;

		memset(status->map, 0, sizeof(status->map));
		if (status->ptr) {
			dma_free_coherent(ctrl->dev, sizeof(u32), status->ptr,
					  status->dma);
			status->ptr = NULL;
		}
	}
}

static int calculet_dma_complete_polled_channel(struct calculet_dma_channel *ch,
						bool is_write)
{
	u8 done = 0, error = 0;
	int ret;

	if (!ch || !ch->board || !ch->ctrl || !ch->ctrl->ready)
		return 0;
	if (!ch->armed)
		return 0;

	ret = calculet_dma_read_profile_status(ch, is_write, &done, &error);
	if (ret)
		ch->result = -EIO;
	else if (done)
		ch->result = error ? -EIO : 1;
	else
		return 0;

	wake_up(&ch->wq);
	return 0;
}

static struct calculet_dma_channel *
calculet_dma_channel_for_status(struct calculet_dma_controller *ctrl, u32 id)
{
	int i;

	for (i = 0; i < 16; i++) {
		if (ctrl->poll->status->map[i] != id)
			continue;
		if (i < 8)
			return &ctrl->rd[i];
		return &ctrl->wr[i - 8];
	}
	return NULL;
}

static int calculet_poll_handler(void *data)
{
	struct calculet_dma_controller *ctrl = data;
	struct calculet_poll_status *status;
	unsigned long flags;
	u32 id;

	if (!ctrl || !ctrl->poll)
		return 0;
	status = ctrl->poll->status;
	if (!status || !status->ptr) {
		complete(&ctrl->poll->stopped);
		return 0;
	}

	status->running = 1;
	while (!kthread_should_stop() && !ctrl->poll->stop) {
		spin_lock_irqsave(&status->lock, flags);
		id = *status->ptr;
		if (id)
			*status->ptr = 0;
		spin_unlock_irqrestore(&status->lock, flags);

		if (id) {
			struct calculet_dma_channel *ch;

			ch = calculet_dma_channel_for_status(ctrl, id);
			if (ch)
				calculet_dma_complete_polled_channel(ch,
								     ch->is_write);
		}
		schedule_timeout_interruptible(msecs_to_jiffies(1));
	}
	status->running = 0;
	complete(&ctrl->poll->stopped);
	return 0;
}

int calculet_pcie_dma_controller_init(struct calculet_pci_board *board)
{
	struct calculet_dma_controller *ctrl;
	struct calculet_poll_ctx *ctx;
	struct device *dev;
	int ret;

	if (!board || !board->pdev)
		return -EINVAL;

	ctrl = &board->dma;
	dev = &board->pdev->dev;
	ctrl->board = board;
	calculet_dma_controller_set_timeout(ctrl, dma_timeout_ms);

	ret = calculet_dma_controller_init(ctrl, dev, poll_mode != 0);
	if (ret < 0) {
		calculet_err("Failed to initialize DMA controller: %d\n", ret);
		return ret;
	}

	if (!poll_mode)
		return 0;

	calculet_info("Poll mode enabled for %s\n", dev_name(dev));

	ctx = ctrl->poll;
	if (!ctx)
		return 0;
	snprintf(ctx->name, sizeof(ctx->name), "pcie_%04x:%02x:%02x.%x",
		 board->pdev->vendor, board->pdev->bus->number,
		 PCI_SLOT(board->pdev->devfn), PCI_FUNC(board->pdev->devfn));
	ctx->thread = kthread_create_on_node(calculet_poll_handler, ctrl,
					     NUMA_NO_NODE, "%s", ctx->name);
	if (IS_ERR(ctx->thread)) {
		ret = PTR_ERR(ctx->thread);
		ctx->thread = NULL;
		calculet_dma_controller_deinit(ctrl, true);
		calculet_err("Failed to start poll thread: %d\n", ret);
		return ret;
	}
	wake_up_process(ctx->thread);
	return 0;
}

int calculet_pcie_enable_poll(struct pci_dev *pdev,
			      struct calculet_pci_res *res)
{
	struct calculet_pci_board *board = pci_get_drvdata(pdev);
	struct calculet_dma_controller *ctrl = &board->dma;
	struct calculet_poll_status *status;
	u16 control = 0;
	u64 addr;
	int cap;
	int ret;
	u32 i;

	if (!pdev || !res || !board || !board->operational) {
		calculet_err("Invalid parameters for poll mode setup\n");
		return -EINVAL;
	}
	if (!ctrl->poll || !ctrl->poll->status ||
	    !ctrl->poll->status->ptr) {
		calculet_err("Poll status region is not allocated\n");
		return -ENOMEM;
	}
	status = ctrl->poll->status;
	addr = status->dma;

	cap = pci_find_capability(pdev, PCI_CAP_ID_MSI);
	if (!cap) {
		calculet_err("Device does not support MSI\n");
		return -ESRCH;
	}
	ret = pci_read_config_word(pdev, cap + PCI_MSI_FLAGS, &control);
	if (ret) {
		calculet_err("Failed to read MSI flags for poll mode\n");
		return -EIO;
	}
	if (!(control & PCI_MSI_FLAGS_64BIT) && upper_32_bits(addr)) {
		calculet_err("Poll status DMA address 0x%llx needs 64-bit MSI\n",
			     addr);
		return -EOVERFLOW;
	}

	calculet_resource_write32(res, CALCULET_RD_MSI_ADDR_LO,
				  lower_32_bits(addr));
	calculet_resource_write32(res, CALCULET_WR0_MSI_ADDR_LO,
				  lower_32_bits(addr));
	if (control & PCI_MSI_FLAGS_64BIT) {
		calculet_resource_write32(res, CALCULET_RD_MSI_ADDR_HI,
					  upper_32_bits(addr));
		calculet_resource_write32(res, CALCULET_WR0_MSI_ADDR_HI,
					  upper_32_bits(addr));
	}
	calculet_resource_write32(res, CALCULET_WR1_MSI_ADDR_LO,
				  lower_32_bits(addr));
	calculet_resource_write32(res, CALCULET_WR2_MSI_ADDR_LO,
				  lower_32_bits(addr));
	if (control & PCI_MSI_FLAGS_64BIT) {
		calculet_resource_write32(res, CALCULET_WR1_MSI_ADDR_HI,
					  upper_32_bits(addr));
		calculet_resource_write32(res, CALCULET_WR2_MSI_ADDR_HI,
					  upper_32_bits(addr));
	}

	for (i = 0; i < 4; i++) {
		u32 data = ((status->map[i * 2 + 1] << 16) |
			    (status->map[i * 2] & 0xffff));
		calculet_resource_write32(res, CALCULET_WR_MSI_DATA0 + i * 4,
					  data);
	}
	for (i = 0; i < 4; i++) {
		u32 data = ((status->map[8 + i * 2 + 1] << 16) |
			    (status->map[8 + i * 2] & 0xffff));
		calculet_resource_write32(res, CALCULET_RD_MSI_DATA0 + i * 4,
					  data);
	}
	calculet_resource_write32(res, CALCULET_OS_IRQ_EN, 1);
	calculet_resource_write32(res, CALCULET_RT_IRQ_EN, 1);
	return 0;
}

/* ------------------------------------------------------------------ */
/* ioctl                                                              */
/* ------------------------------------------------------------------ */

static int calculet_dma_wait(struct calculet_dma_channel *ch, u32 timeout_ms)
{
	long timeout = msecs_to_jiffies(timeout_ms);

	if (!timeout)
		timeout = 1;
	return wait_event_interruptible_timeout(ch->wq,
						READ_ONCE(ch->result) != 0,
						timeout);
}

static void calculet_dma_program_read(struct calculet_pci_res *res,
				      struct calculet_dma_channel *ch)
{
	unsigned long base = CALCULET_DMA_RD_BASE(ch->id);

	calculet_resource_write32(res, base + CALCULET_DMA_CTRL, 0x10);
	calculet_resource_write32(res, base + CALCULET_DMA_LEN,
				  lower_32_bits(ch->length));
	calculet_resource_write32(res, base + CALCULET_DMA_ADDR_LO,
				  lower_32_bits(ch->buf_dma));
	calculet_resource_write32(res, base + CALCULET_DMA_ADDR_HI,
				  upper_32_bits(ch->buf_dma));
	calculet_resource_write32(res, base + CALCULET_DMA_HOST_LO,
				  lower_32_bits(ch->device_addr));
	calculet_resource_write32(res, base + CALCULET_DMA_HOST_HI,
				  upper_32_bits(ch->device_addr));
	calculet_resource_write32(res, CALCULET_DMA_RD_SELECT, ch->id);
}

static void calculet_dma_program_write(struct calculet_pci_res *res,
				       struct calculet_dma_channel *ch)
{
	unsigned long base = CALCULET_DMA_WR_BASE(ch->id);

	calculet_resource_write32(res, base + CALCULET_DMA_CTRL, 0x10);
	calculet_resource_write32(res, base + CALCULET_DMA_LEN,
				  lower_32_bits(ch->length));
	calculet_resource_write32(res, base + CALCULET_DMA_ADDR_LO,
				  lower_32_bits(ch->device_addr));
	calculet_resource_write32(res, base + CALCULET_DMA_ADDR_HI,
				  upper_32_bits(ch->device_addr));
	calculet_resource_write32(res, base + CALCULET_DMA_HOST_LO,
				  lower_32_bits(ch->buf_dma));
	calculet_resource_write32(res, base + CALCULET_DMA_HOST_HI,
				  upper_32_bits(ch->buf_dma));
	calculet_resource_write32(res, CALCULET_DMA_WR_SELECT, ch->id);
}

static long calculet_dma_transfer(struct calculet_pci_res *res,
				  struct calculet_dma_controller *ctrl,
				  struct calculet_dma_channel *ch,
				  struct calculet_dma_transfer_channels_params *p)
{
	struct calculet_pci_board *board = ctrl->board;
	bool h2c = p->flags == 1;
	unsigned long flags;
	long left;
	int ret;

	ret = mutex_lock_interruptible(&ch->ioctl_lock);
	if (ret)
		return -ERESTARTSYS;

	mutex_lock(&ctrl->state_lock);
	if (!ctrl->ready || !ctrl->accepting ||
	    ch->state == CALCULET_DMA_ACTIVE ||
	    ch->state == CALCULET_DMA_COMPLETED) {
		mutex_unlock(&ctrl->state_lock);
		mutex_unlock(&ch->ioctl_lock);
		return -EBUSY;
	}
	if (ch->state == CALCULET_DMA_FAILED) {
		mutex_unlock(&ctrl->state_lock);
		mutex_unlock(&ch->ioctl_lock);
		return -EBUSY;
	}
	spin_lock_irqsave(&ch->lock, flags);
	ch->state = CALCULET_DMA_ACTIVE;
	ch->result = 0;
	ch->errors = 0;
	ch->started_at = ktime_get();
	spin_unlock_irqrestore(&ch->lock, flags);
	atomic_inc(&ctrl->active);
	mutex_unlock(&ctrl->state_lock);

	ch->length = p->length;
	ch->user_ptr = p->user_ptr;
	ch->device_addr = p->device_addr;
	ch->armed = false;

	ret = calculet_dma_prepare_buffer(ctrl->dev, ch);
	if (ret) {
		if (h2c)
			dev_err(ctrl->dev, "Failed to prepare H2C DMA buffer (%ld)\n",
				(long)ret);
		else
			dev_err(ctrl->dev, "Failed to prepare C2H DMA buffer (%ld)\n",
				(long)ret);
		goto out_fail;
	}

	if (h2c &&
	    copy_from_user(ch->buf, (void __user *)ch->user_ptr, ch->length)) {
		dev_err(ctrl->dev, "Failed to copy_from_user\n");
		ret = -EFAULT;
		goto out_fail;
	}

	if (!h2c)
		calculet_dma_program_write(res, ch);
	else
		calculet_dma_program_read(res, ch);

	ch->armed = true;
	left = calculet_dma_wait(ch, ctrl->timeout_ms);
	if (left > 0) {
		if (ch->result == 1) {
			if (!h2c &&
			    copy_to_user((void __user *)ch->user_ptr, ch->buf,
					 ch->length)) {
				dev_err(ctrl->dev, "Failed to copy_to_user\n");
				ret = -EFAULT;
				goto out_fail;
			}
			board->stat_ops++;
			ret = ch->length;
		} else {
			dev_err(ctrl->dev, "%s transfer failed with status %d\n",
				h2c ? "H2C" : "C2H", ch->result);
			ret = ch->result;
		}
	} else if (left == 0) {
		dev_err(ctrl->dev, "%s transfer timed out\n",
			h2c ? "H2C" : "C2H");
		board->stat_timeouts++;
		ch->errors++;
		ch->started_at = ktime_get();
		ret = -ETIMEDOUT;
		calculet_dma_poison_if_abort_failed(ch, h2c);
	} else {
		dev_err(ctrl->dev, "%s transfer interrupted\n",
			h2c ? "H2C" : "C2H");
		ret = -ERESTARTSYS;
		calculet_dma_poison_if_abort_failed(ch, h2c);
	}

	ch->armed = false;
	if (ret >= 0)
		calculet_dma_channel_set_state(ch, CALCULET_DMA_IDLE);
	atomic_dec(&ctrl->active);
	wake_up_all(&ctrl->active_wq);
	mutex_unlock(&ch->ioctl_lock);
	return ret;

out_fail:
	/* Release the channel; the vendor binary leaves it ACTIVE here. */
	calculet_dma_channel_set_state(ch, CALCULET_DMA_IDLE);
	atomic_dec(&ctrl->active);
	wake_up_all(&ctrl->active_wq);
	mutex_unlock(&ch->ioctl_lock);
	return ret;
}

int calculet_dma_poll_once(struct calculet_dma_controller *ctrl)
{
	struct calculet_poll_status *status;
	unsigned long flags;
	u32 id;

	if (!ctrl->poll)
		return -EINVAL;
	status = ctrl->poll->status;
	if (!status || !status->ptr)
		return -ESRCH;

	spin_lock_irqsave(&status->lock, flags);
	id = *status->ptr;
	if (id)
		*status->ptr = 0;
	spin_unlock_irqrestore(&status->lock, flags);

	if (id) {
		struct calculet_dma_channel *ch;

		ch = calculet_dma_channel_for_status(ctrl, id);
		if (ch)
			calculet_dma_complete_polled_channel(ch, ch->is_write);
	}
	/* The vendor ioctl returns the consumed status word (0 if empty). */
	return id;
}

long calculet_dma_transfer_params(struct calculet_pci_res *res,
				  struct calculet_dma_controller *ctrl,
				  struct calculet_dma_transfer_channels_params *p)
{
	struct calculet_dma_channel *ch;

	if (!res || !ctrl || !p)
		return -EINVAL;

	if (p->device_addr & 3) {
		calculet_err("Device address 0x%llx is not 4-byte aligned\n",
			     p->device_addr);
		return -EINVAL;
	}
	if (p->flags > 1 || p->channel >= 8 || !p->length ||
	    p->length > CALCULET_DMA_MAX_TRANSFER || !p->user_ptr)
		return -EINVAL;

	ch = p->flags == 1 ? &ctrl->rd[p->channel] : &ctrl->wr[p->channel];
	return calculet_dma_transfer(res, ctrl, ch, p);
}
