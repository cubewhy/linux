// SPDX-License-Identifier: GPL-2.0-only
/*
 * PCI probe/remove, BAR resources, MSI/poll setup and power management.
 */

#include <linux/delay.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/msi.h>
#include <linux/pci.h>
#include <linux/pci_regs.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <drm/drm_accel.h>
#include <drm/drm_drv.h>

#include "calculet_pci.h"

unsigned int poll_mode;
module_param(poll_mode, uint, 0444);
MODULE_PARM_DESC(poll_mode, "Use polled DMA completion instead of interrupts");

unsigned int max_boards = CALCULET_DEFAULT_MAX_BOARDS;
module_param(max_boards, uint, 0444);
MODULE_PARM_DESC(max_boards, "Maximum number of boards to support");

unsigned int dma_timeout_ms = CALCULET_DEFAULT_DMA_TIMEOUT_MS;
module_param(dma_timeout_ms, uint, 0444);
MODULE_PARM_DESC(dma_timeout_ms, "DMA transfer timeout in milliseconds");

bool calculet_restrict_privileged_io;
module_param_named(restrict_privileged_io, calculet_restrict_privileged_io,
		   bool, 0644);
MODULE_PARM_DESC(restrict_privileged_io,
		 "Require CAP_SYS_RAWIO/CAP_SYS_ADMIN for raw BAR, EP MSI and reset requests");

static const struct calculet_chip_profile calculet_legacy_profile = {
	.name		= "legacy-ks1",
	.dma_bar	= CALCULET_DMA_BAR,
	.rt_bar		= CALCULET_RT_BAR,
	.smi_bar	= CALCULET_SMI_BAR,
	.read_channels	= CALCULET_READ_CHANNELS,
	.write_channels	= CALCULET_WRITE_CHANNELS,
	.os_status_reg	= CALCULET_RT_OS_STATUS_REG,
	.rt_status_reg	= CALCULET_RT_RT_STATUS_REG,
	.soft_reset_reg	= CALCULET_RT_SOFT_RESET_REG,
};

static const struct calculet_chip_profile *
calculet_profile_from_id(const struct pci_device_id *id)
{
	if (id && id->driver_data)
		return (const void *)(unsigned long)id->driver_data;
	return &calculet_legacy_profile;
}

/* ---------------------------------------------------------------- */
/* Register access                                                  */
/* ---------------------------------------------------------------- */

static bool calculet_res_ok(struct calculet_resource *res, u64 off,
			    size_t len)
{
	return res->addr && !(off & 3) && off <= res->size &&
	       len <= res->size - off;
}

u32 calculet_reg_read32(struct calculet_resource *res, u64 off)
{
	unsigned long flags;
	u32 val = 0xffffffffU;

	if (!res)
		return val;

	spin_lock_irqsave(&res->access_lock, flags);
	if (calculet_res_ok(res, off, sizeof(u32)))
		val = readl(res->addr + off);
	spin_unlock_irqrestore(&res->access_lock, flags);

	return val;
}

u64 calculet_reg_read64(struct calculet_resource *res, u64 off)
{
	unsigned long flags;
	u64 val = ~0ULL;

	if (!res)
		return val;

	spin_lock_irqsave(&res->access_lock, flags);
	if (calculet_res_ok(res, off, sizeof(u64))) {
		u32 lo = readl(res->addr + off);
		u32 hi = readl(res->addr + off + sizeof(u32));

		val = ((u64)hi << 32) | lo;
	}
	spin_unlock_irqrestore(&res->access_lock, flags);

	return val;
}

void calculet_reg_write32(struct calculet_resource *res, u64 off, u32 val)
{
	unsigned long flags;

	if (!res)
		return;

	spin_lock_irqsave(&res->access_lock, flags);
	if (calculet_res_ok(res, off, sizeof(u32)))
		writel(val, res->addr + off);
	spin_unlock_irqrestore(&res->access_lock, flags);
}

void calculet_reg_write64(struct calculet_resource *res, u64 off, u64 val)
{
	unsigned long flags;

	if (!res)
		return;

	spin_lock_irqsave(&res->access_lock, flags);
	if (calculet_res_ok(res, off, sizeof(u64))) {
		writel((u32)val, res->addr + off);
		writel((u32)(val >> 32), res->addr + off + sizeof(u32));
	}
	spin_unlock_irqrestore(&res->access_lock, flags);
}

void calculet_read_strings(char *buf, size_t buf_size,
			   struct calculet_resource *res, u64 off, size_t len)
{
	unsigned long flags;
	size_t copy;
	bool ok;

	if (!buf || !buf_size || !res || !len)
		return;

	copy = min(len, buf_size - 1);

	spin_lock_irqsave(&res->access_lock, flags);
	ok = calculet_res_ok(res, off, len);
	if (ok)
		memcpy_fromio(buf, res->addr + off, copy);
	spin_unlock_irqrestore(&res->access_lock, flags);

	if (ok)
		buf[copy] = '\0';
}

/* ---------------------------------------------------------------- */
/* BAR resources                                                    */
/* ---------------------------------------------------------------- */

static int calculet_bar_map(struct pci_dev *pdev, int bar,
			    struct calculet_resource *res)
{
	unsigned long flags;

	if (bar < 0 || bar >= PCI_STD_NUM_BARS)
		return -EINVAL;

	flags = pci_resource_flags(pdev, bar);
	if (!(flags & IORESOURCE_MEM))
		return -ENODEV;

	res->phys = pci_resource_start(pdev, bar);
	res->size = pci_resource_len(pdev, bar);
	if (!res->size)
		return -ENODEV;

	res->addr = pci_iomap(pdev, bar, res->size);
	if (!res->addr) {
		res->size = 0;
		return -EINVAL;
	}

	return 0;
}

static void calculet_bar_unmap(struct pci_dev *pdev,
			       struct calculet_resource *res)
{
	unsigned long flags;
	void __iomem *addr;

	spin_lock_irqsave(&res->access_lock, flags);
	addr = res->addr;
	res->addr = NULL;
	res->size = 0;
	res->phys = 0;
	spin_unlock_irqrestore(&res->access_lock, flags);

	if (addr)
		pci_iounmap(pdev, addr);
}

static bool calculet_res_has_u32(struct calculet_resource *res, u32 off)
{
	return res->addr && !(off & 3) && off <= res->size &&
	       sizeof(u32) <= res->size - off;
}

static bool calculet_dma_layout_valid(struct calculet_resource *res,
				      const struct calculet_chip_profile *profile)
{
	size_t required;

	if (!res->addr || !profile->read_channels || !profile->write_channels)
		return false;

	required = CALCULET_DMA_RD_DAR_HI(profile->read_channels - 1) +
		   sizeof(u32);
	required = max(required,
		       (size_t)CALCULET_DMA_WR_DAR_HI(
			       profile->write_channels - 1) + sizeof(u32));
	required = max(required,
		       (size_t)CALCULET_DMA_RD_DATA +
			       ((profile->read_channels + 1) / 2 - 1) * 4 + 4);
	required = max(required,
		       (size_t)CALCULET_DMA_WR_DATA +
			       ((profile->write_channels + 1) / 2 - 1) * 4 + 4);
	required = max(required, (size_t)CALCULET_DMA_RD_DOORBELL + 4);

	return res->size >= required;
}

int calculet_resources_init(struct pci_dev *pdev,
			    struct calculet_pcie_resources *res)
{
	struct calculet_board *board = pci_get_drvdata(pdev);
	const struct calculet_chip_profile *profile = board->profile;
	int ret;

	WRITE_ONCE(res->valid, false);
	res->regions_requested = false;

	mutex_init(&res->rt.dma_lock);
	spin_lock_init(&res->rt.access_lock);
	mutex_init(&res->smi.dma_lock);
	spin_lock_init(&res->smi.access_lock);

	ret = pci_request_regions(pdev, CALCULET_DRV_NAME);
	if (ret)
		return ret;
	res->regions_requested = true;

	ret = calculet_bar_map(pdev, profile->dma_bar, &res->dma);
	if (ret)
		goto err_regions;

	ret = calculet_bar_map(pdev, profile->rt_bar, &res->rt);
	if (ret)
		goto err_dma;

	ret = calculet_bar_map(pdev, profile->smi_bar, &res->smi);
	if (ret)
		goto err_rt;

	if (!calculet_res_has_u32(&res->rt, profile->os_status_reg) ||
	    !calculet_res_has_u32(&res->rt, profile->rt_status_reg) ||
	    !calculet_res_has_u32(&res->rt, profile->soft_reset_reg) ||
	    !calculet_dma_layout_valid(&res->dma, profile)) {
		calculet_err(board, "profile %s register layout does not fit the BARs\n",
			     profile->name);
		ret = -EINVAL;
		goto err_smi;
	}

	WRITE_ONCE(res->valid, true);
	return 0;

err_smi:
	calculet_bar_unmap(pdev, &res->smi);
err_rt:
	calculet_bar_unmap(pdev, &res->rt);
err_dma:
	calculet_bar_unmap(pdev, &res->dma);
err_regions:
	pci_release_regions(pdev);
	res->regions_requested = false;
	return ret;
}

void calculet_resources_release(struct pci_dev *pdev,
				struct calculet_pcie_resources *res)
{
	WRITE_ONCE(res->valid, false);
	calculet_bar_unmap(pdev, &res->dma);
	calculet_bar_unmap(pdev, &res->rt);
	calculet_bar_unmap(pdev, &res->smi);
	if (res->regions_requested) {
		pci_release_regions(pdev);
		res->regions_requested = false;
	}
}

/* ---------------------------------------------------------------- */
/* MSI and poll programming                                         */
/* ---------------------------------------------------------------- */

static int calculet_cached_msi(struct pci_dev *pdev, unsigned int vector,
			       struct msi_msg *msg)
{
	int irq = pci_irq_vector(pdev, vector);

	if (irq < 0)
		return irq;
	get_cached_msi_msg(irq, msg);
	return 0;
}

static int calculet_program_msi(struct calculet_board *board)
{
	struct calculet_resource *dma = &board->res.dma;
	struct msi_msg msg;
	u32 addr_lo, addr_hi, base;
	int ret, i;

	BUILD_BUG_ON(CALCULET_READ_CHANNELS % 2);
	BUILD_BUG_ON(CALCULET_WRITE_CHANNELS % 2);

	ret = calculet_cached_msi(board->pdev, 0, &msg);
	if (ret)
		return ret;

	addr_lo = msg.address_lo;
	addr_hi = msg.address_hi;
	base = msg.data & 0xffff;
	if (!addr_lo)
		return -EINVAL;

	calculet_reg_write32(dma, CALCULET_DMA_WR_DONE_ADDR_LO, addr_lo);
	calculet_reg_write32(dma, CALCULET_DMA_WR_ABORT_ADDR_LO, addr_lo);
	calculet_reg_write32(dma, CALCULET_DMA_RD_DONE_ADDR_LO, addr_lo);
	calculet_reg_write32(dma, CALCULET_DMA_RD_ABORT_ADDR_LO, addr_lo);
	calculet_reg_write32(dma, CALCULET_DMA_WR_DONE_ADDR_HI, addr_hi);
	calculet_reg_write32(dma, CALCULET_DMA_WR_ABORT_ADDR_HI, addr_hi);
	calculet_reg_write32(dma, CALCULET_DMA_RD_DONE_ADDR_HI, addr_hi);
	calculet_reg_write32(dma, CALCULET_DMA_RD_ABORT_ADDR_HI, addr_hi);

	/* MSI carries one base data value; the endpoint DMA engine is
	 * programmed with the consecutive vector values.
	 */
	for (i = 0; i < CALCULET_READ_CHANNELS; i += 2) {
		u32 lo = (base + i) & 0xffff;
		u32 hi = (base + i + 1) & 0xffff;

		calculet_reg_write32(dma, CALCULET_DMA_RD_DATA + i / 2 * 4,
				     (hi << 16) | lo);
	}

	for (i = 0; i < CALCULET_WRITE_CHANNELS; i += 2) {
		u32 lo = (base + CALCULET_READ_CHANNELS + i) & 0xffff;
		u32 hi = (base + CALCULET_READ_CHANNELS + i + 1) & 0xffff;

		calculet_reg_write32(dma, CALCULET_DMA_WR_DATA + i / 2 * 4,
				     (hi << 16) | lo);
	}

	calculet_reg_write32(dma, CALCULET_DMA_WR_ENGINE_EN, 1);
	calculet_reg_write32(dma, CALCULET_DMA_RD_ENGINE_EN, 1);

	return 0;
}

static int calculet_program_poll(struct calculet_board *board)
{
	struct calculet_dma_poll *poll = board->dma.thread->poll;
	struct calculet_resource *dma = &board->res.dma;
	u16 control;
	u32 addr_lo, addr_hi, data;
	int pos, i, j;

	pos = pci_find_capability(board->pdev, PCI_CAP_ID_MSI);
	if (!pos)
		return -ENODEV;
	if (pci_read_config_word(board->pdev, pos + PCI_MSI_FLAGS, &control))
		return -EIO;

	addr_lo = lower_32_bits(poll->phys);
	addr_hi = upper_32_bits(poll->phys);
	if (!(control & PCI_MSI_FLAGS_64BIT) && addr_hi)
		return -EOVERFLOW;

	calculet_reg_write32(dma, CALCULET_DMA_WR_DONE_ADDR_LO, addr_lo);
	calculet_reg_write32(dma, CALCULET_DMA_WR_ABORT_ADDR_LO, addr_lo);
	calculet_reg_write32(dma, CALCULET_DMA_RD_DONE_ADDR_LO, addr_lo);
	calculet_reg_write32(dma, CALCULET_DMA_RD_ABORT_ADDR_LO, addr_lo);
	if (control & PCI_MSI_FLAGS_64BIT) {
		calculet_reg_write32(dma, CALCULET_DMA_WR_DONE_ADDR_HI, addr_hi);
		calculet_reg_write32(dma, CALCULET_DMA_WR_ABORT_ADDR_HI, addr_hi);
		calculet_reg_write32(dma, CALCULET_DMA_RD_DONE_ADDR_HI, addr_hi);
		calculet_reg_write32(dma, CALCULET_DMA_RD_ABORT_ADDR_HI, addr_hi);
	}

	for (i = 0, j = 0; i < CALCULET_READ_CHANNELS; i += 2, j++) {
		data = (poll->map[i + 1] << 16) | (poll->map[i] & 0xffff);
		calculet_reg_write32(dma, CALCULET_DMA_RD_DATA + j * 4, data);
	}

	for (i = 0, j = 0; i < CALCULET_WRITE_CHANNELS; i += 2, j++) {
		data = (poll->map[CALCULET_READ_CHANNELS + i + 1] << 16) |
		       (poll->map[CALCULET_READ_CHANNELS + i] & 0xffff);
		calculet_reg_write32(dma, CALCULET_DMA_WR_DATA + j * 4, data);
	}

	calculet_reg_write32(dma, CALCULET_DMA_WR_ENGINE_EN, 1);
	calculet_reg_write32(dma, CALCULET_DMA_RD_ENGINE_EN, 1);

	return 0;
}

static void calculet_disable_engines(struct calculet_board *board)
{
	if (!READ_ONCE(board->res.valid))
		return;
	calculet_reg_write32(&board->res.dma, CALCULET_DMA_WR_ENGINE_EN, 0);
	calculet_reg_write32(&board->res.dma, CALCULET_DMA_RD_ENGINE_EN, 0);
}

/* ---------------------------------------------------------------- */
/* Interrupts                                                       */
/* ---------------------------------------------------------------- */

static void calculet_init_dma_channel(struct calculet_board *board,
				      struct calculet_dma_channel *ch,
				      unsigned int index, unsigned int vector)
{
	ch->index = index;
	ch->vector = vector;
	ch->irq = 0;
	ch->regs = board->res.dma.addr;
	atomic_set(&ch->done, 0);
	calculet_dma_channel_set_state(ch, CALCULET_DMA_IDLE);
}

int calculet_enable_interrupts(struct calculet_board *board)
{
	struct calculet_dma_controller *ctrl = &board->dma;
	int rd_irqs = 0, wr_irqs = 0, ep_irqs = 0;
	int irq, irq_num, ret, i;

	if (!board || !board->pdev)
		return -EINVAL;
	if (!READ_ONCE(board->res.valid))
		return -ENODEV;

	if (poll_mode) {
		if (!ctrl->thread || !ctrl->thread->poll)
			return -EINVAL;

		ret = calculet_program_poll(board);
		if (ret)
			return ret;

		for (i = 0; i < CALCULET_READ_CHANNELS; i++)
			calculet_init_dma_channel(board, &ctrl->rd[i], i, i);
		for (i = 0; i < CALCULET_WRITE_CHANNELS; i++)
			calculet_init_dma_channel(board, &ctrl->wr[i], i,
						  CALCULET_READ_CHANNELS + i);

		board->interrupts_enabled = true;
		board->irq_vectors = 0;
		board->irq_handlers = 0;
		return 0;
	}

	if (board->interrupts_enabled)
		return -EINVAL;

	irq_num = pci_alloc_irq_vectors(board->pdev, CALCULET_TOTAL_CHANNELS,
					CALCULET_MAX_CHANNELS, PCI_IRQ_MSI);
	if (irq_num < 0)
		return irq_num;
	if (irq_num < CALCULET_TOTAL_CHANNELS) {
		ret = -ENOSPC;
		goto err_vectors;
	}

	board->irq_vectors = irq_num;
	board->irq_handlers = 0;
	memset(board->irq_registered, 0, sizeof(board->irq_registered));

	for (i = 0; i < CALCULET_READ_CHANNELS; i++) {
		irq = pci_irq_vector(board->pdev, i);
		if (irq < 0) {
			ret = irq;
			goto err;
		}
		calculet_init_dma_channel(board, &ctrl->rd[i], i, i);
		ctrl->rd[i].irq = irq;
		ret = request_irq(irq, calculet_dma_irq, IRQF_SHARED,
				  CALCULET_DRV_NAME, &ctrl->rd[i]);
		if (ret)
			goto err;
		rd_irqs++;
		board->irq_handlers++;
		board->irq_registered[i] = true;
	}

	for (i = 0; i < CALCULET_WRITE_CHANNELS; i++) {
		unsigned int vector = CALCULET_READ_CHANNELS + i;

		irq = pci_irq_vector(board->pdev, vector);
		if (irq < 0) {
			ret = irq;
			goto err;
		}
		calculet_init_dma_channel(board, &ctrl->wr[i], i, vector);
		ctrl->wr[i].irq = irq;
		ret = request_irq(irq, calculet_dma_irq, IRQF_SHARED,
				  CALCULET_DRV_NAME, &ctrl->wr[i]);
		if (ret)
			goto err;
		wr_irqs++;
		board->irq_handlers++;
		board->irq_registered[vector] = true;
	}

	for (i = CALCULET_TOTAL_CHANNELS; i < irq_num; i++) {
		unsigned int index = i - CALCULET_TOTAL_CHANNELS;

		if (index >= ARRAY_SIZE(ctrl->ep))
			break;
		irq = pci_irq_vector(board->pdev, i);
		if (irq < 0) {
			ret = irq;
			goto err;
		}
		calculet_init_dma_channel(board, &ctrl->ep[index], index, i);
		ctrl->ep[index].irq = irq;
		ret = request_irq(irq, calculet_dma_irq, IRQF_SHARED,
				  CALCULET_DRV_NAME, &ctrl->ep[index]);
		if (ret)
			goto err;
		ep_irqs++;
		board->irq_handlers++;
		board->irq_registered[i] = true;
	}

	ret = calculet_program_msi(board);
	if (ret)
		goto err;

	board->interrupts_enabled = true;
	return 0;

err:
	while (ep_irqs-- > 0)
		free_irq(ctrl->ep[ep_irqs].irq, &ctrl->ep[ep_irqs]);
	while (wr_irqs-- > 0)
		free_irq(ctrl->wr[wr_irqs].irq, &ctrl->wr[wr_irqs]);
	while (rd_irqs-- > 0)
		free_irq(ctrl->rd[rd_irqs].irq, &ctrl->rd[rd_irqs]);
err_vectors:
	calculet_disable_engines(board);
	board->irq_handlers = 0;
	board->irq_vectors = 0;
	memset(board->irq_registered, 0, sizeof(board->irq_registered));
	pci_free_irq_vectors(board->pdev);
	return ret;
}

void calculet_disable_interrupts(struct calculet_board *board)
{
	struct calculet_dma_controller *ctrl = &board->dma;
	int i;

	if (!board || !board->pdev)
		return;

	if (poll_mode) {
		calculet_disable_engines(board);
		board->interrupts_enabled = false;
		return;
	}

	if (!board->interrupts_enabled && !board->irq_vectors)
		return;

	board->interrupts_enabled = false;
	calculet_disable_engines(board);

	for (i = 0; i < CALCULET_READ_CHANNELS; i++) {
		if (!board->irq_registered[i])
			continue;
		free_irq(ctrl->rd[i].irq, &ctrl->rd[i]);
		board->irq_registered[i] = false;
	}
	for (i = 0; i < CALCULET_WRITE_CHANNELS; i++) {
		unsigned int vector = CALCULET_READ_CHANNELS + i;

		if (!board->irq_registered[vector])
			continue;
		free_irq(ctrl->wr[i].irq, &ctrl->wr[i]);
		board->irq_registered[vector] = false;
	}
	for (i = 0; i < ARRAY_SIZE(ctrl->ep); i++) {
		unsigned int vector = CALCULET_TOTAL_CHANNELS + i;

		if (vector >= board->irq_vectors ||
		    !board->irq_registered[vector])
			continue;
		free_irq(ctrl->ep[i].irq, &ctrl->ep[i]);
		board->irq_registered[vector] = false;
	}

	pci_free_irq_vectors(board->pdev);
	board->irq_handlers = 0;
	board->irq_vectors = 0;
	memset(board->irq_registered, 0, sizeof(board->irq_registered));
}

int calculet_activate_board(struct calculet_board *board)
{
	u32 os_status, rt_status;
	int ret;

	ret = calculet_enable_interrupts(board);
	if (ret)
		return ret;

	os_status = calculet_reg_read32(&board->res.rt,
					board->profile->os_status_reg);
	rt_status = calculet_reg_read32(&board->res.rt,
					board->profile->rt_status_reg);
	calculet_info(board, "OS %s, RT %s\n",
		      os_status == 1 ? "active" : "inactive",
		      rt_status == 1 ? "active" : "inactive");

	return 0;
}

/* ---------------------------------------------------------------- */
/* Power management                                                 */
/* ---------------------------------------------------------------- */

static void calculet_pause_board(struct calculet_board *board)
{
	{
		guard(mutex)(&board->op_mutex);
		calculet_board_set_state(board, CALCULET_BOARD_SUSPENDED);
	}

	calculet_dma_stop(&board->dma);
	calculet_dma_wait(&board->dma);
	wait_event(board->op_wq, atomic_read(&board->active_ops) == 0);

	calculet_dma_stop_poll_thread(&board->dma);
	calculet_disable_interrupts(board);
	calculet_dma_deinit(&board->dma);
	WRITE_ONCE(board->res.valid, false);
}

static int calculet_restart_board(struct calculet_board *board)
{
	struct device *dev = &board->pdev->dev;
	int ret;

	WRITE_ONCE(board->res.valid, false);

	ret = calculet_dma_init(&board->dma, dev, poll_mode);
	if (ret)
		return ret;

	WRITE_ONCE(board->res.valid, true);

	ret = calculet_enable_interrupts(board);
	if (ret) {
		calculet_dma_deinit(&board->dma);
		WRITE_ONCE(board->res.valid, false);
		return ret;
	}

	calculet_info(board, "resumed, OS %s, RT %s\n",
		      calculet_reg_read32(&board->res.rt,
					  board->profile->os_status_reg) == 1 ?
			      "active" : "inactive",
		      calculet_reg_read32(&board->res.rt,
					  board->profile->rt_status_reg) == 1 ?
			      "active" : "inactive");

	return 0;
}

static int calculet_suspend(struct device *dev)
{
	struct calculet_board *board = pci_get_drvdata(to_pci_dev(dev));

	if (!board)
		return -ENODEV;
	if (board->suspended)
		return 0;

	calculet_pause_board(board);
	board->suspended = true;
	return 0;
}

static int calculet_resume(struct device *dev)
{
	struct calculet_board *board = pci_get_drvdata(to_pci_dev(dev));
	int ret;

	if (!board)
		return -ENODEV;
	if (READ_ONCE(board->fail_closed))
		return -EIO;
	if (!board->suspended)
		return 0;

	ret = calculet_restart_board(board);
	if (ret) {
		calculet_disable_interrupts(board);
		board->suspended = true;
		WRITE_ONCE(board->res.valid, false);
		calculet_board_set_state(board, CALCULET_BOARD_SUSPENDED);
		return ret;
	}

	board->suspended = false;
	calculet_board_set_state(board, CALCULET_BOARD_ACTIVE);
	return 0;
}

DEFINE_SIMPLE_DEV_PM_OPS(calculet_pm_ops, calculet_suspend, calculet_resume);

static void calculet_shutdown(struct pci_dev *pdev)
{
	struct calculet_board *board = pci_get_drvdata(pdev);

	if (board)
		calculet_disable_interrupts(board);
	pci_disable_device(pdev);
}

/* ---------------------------------------------------------------- */
/* PCI error handling                                               */
/* ---------------------------------------------------------------- */

static void calculet_fail_board(struct calculet_board *board)
{
	if (!board)
		return;

	{
		guard(mutex)(&board->op_mutex);
		board->fail_closed = true;
		calculet_board_set_state(board, CALCULET_BOARD_SUSPENDED);
	}

	{
		guard(mutex)(&board->res.dma.dma_lock);
		calculet_dma_fail(&board->dma, -EIO);
		WRITE_ONCE(board->res.valid, false);
	}

	atomic64_inc(&board->stats.errors);
}

static pci_ers_result_t calculet_error_detected(struct pci_dev *pdev,
						pci_channel_state_t state)
{
	struct calculet_board *board = pci_get_drvdata(pdev);

	if (board)
		calculet_err(board, "PCI error detected (state %d)\n", state);
	calculet_fail_board(board);
	return PCI_ERS_RESULT_DISCONNECT;
}

static pci_ers_result_t calculet_slot_reset(struct pci_dev *pdev)
{
	struct calculet_board *board = pci_get_drvdata(pdev);

	calculet_fail_board(board);
	return PCI_ERS_RESULT_DISCONNECT;
}

static void calculet_error_resume(struct pci_dev *pdev)
{
	struct calculet_board *board = pci_get_drvdata(pdev);

	if (board)
		calculet_err(board, "PCI error resume; rebind required\n");
}

/* ---------------------------------------------------------------- */
/* Sysfs                                                            */
/* ---------------------------------------------------------------- */

static ssize_t driver_version_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", CALCULET_DRV_VER);
}
static DEVICE_ATTR_RO(driver_version);

static ssize_t poll_mode_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", poll_mode);
}
static DEVICE_ATTR_RO(poll_mode);

static ssize_t board_state_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct calculet_board *board = dev_get_drvdata(dev);
	const char *state;

	if (!board)
		return sysfs_emit(buf, "unknown\n");

	switch (calculet_board_get_state(board)) {
	case CALCULET_BOARD_ACTIVE:
		state = "active";
		break;
	case CALCULET_BOARD_SUSPENDED:
		state = "suspended";
		break;
	case CALCULET_BOARD_REMOVING:
		state = "removing";
		break;
	default:
		state = "unknown";
		break;
	}

	return sysfs_emit(buf, "%s\n", state);
}
static DEVICE_ATTR_RO(board_state);

static ssize_t health_status_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct calculet_board *board = dev_get_drvdata(dev);
	int status;

	if (!board)
		return sysfs_emit(buf, "unknown\n");

	status = calculet_board_health_check(board);
	return sysfs_emit(buf, "%s (%d)\n", status ? "poor" : "good", status);
}
static DEVICE_ATTR_RO(health_status);

static struct attribute *calculet_attrs[] = {
	&dev_attr_driver_version.attr,
	&dev_attr_poll_mode.attr,
	&dev_attr_board_state.attr,
	&dev_attr_health_status.attr,
	NULL,
};
ATTRIBUTE_GROUPS(calculet);

/* ---------------------------------------------------------------- */
/* Probe and remove                                                 */
/* ---------------------------------------------------------------- */

static int calculet_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct calculet_board *board;
	bool published = false;
	int ret;

	board = devm_drm_dev_alloc(&pdev->dev, &calculet_drm_driver,
				   struct calculet_board, drm);
	if (IS_ERR(board))
		return PTR_ERR(board);

	board->pdev = pdev;
	board->profile = calculet_profile_from_id(id);
	if (board->profile->read_channels != CALCULET_READ_CHANNELS ||
	    board->profile->write_channels != CALCULET_WRITE_CHANNELS) {
		calculet_err(board, "profile %s is incompatible with the UAPI\n",
			     board->profile->name);
		return -EOPNOTSUPP;
	}

	calculet_board_init(board);
	pci_set_drvdata(pdev, board);

	ret = pci_enable_device(pdev);
	if (ret)
		goto err_drvdata;

	pci_set_master(pdev);

	ret = calculet_resources_init(pdev, &board->res);
	if (ret)
		goto err_device;

	board->dma.board = board;
	calculet_dma_set_timeout(&board->dma, dma_timeout_ms);

	ret = calculet_dma_init(&board->dma, &pdev->dev, poll_mode);
	if (ret)
		goto err_resources;

	ret = calculet_activate_board(board);
	if (ret)
		goto err_dma;

	ret = calculet_board_mgr_add_board(board);
	if (ret)
		goto err_hw;
	published = true;

	ret = drm_dev_register(&board->drm, 0);
	if (ret)
		goto err_hw;

	calculet_info(board, "registered with the accel subsystem\n");
	return 0;

err_hw:
	calculet_disable_interrupts(board);
	if (published)
		calculet_board_mgr_remove_board(board);
err_dma:
	calculet_dma_deinit(&board->dma);
err_resources:
	calculet_resources_release(pdev, &board->res);
err_device:
	pci_disable_device(pdev);
err_drvdata:
	pci_set_drvdata(pdev, NULL);
	return ret;
}

static void calculet_remove(struct pci_dev *pdev)
{
	struct calculet_board *board = pci_get_drvdata(pdev);

	if (!board)
		return;

	calculet_board_begin_remove(board);
	calculet_dma_stop(&board->dma);
	calculet_dma_wait(&board->dma);
	calculet_dma_stop_poll_thread(&board->dma);
	calculet_disable_interrupts(board);
	calculet_dma_deinit(&board->dma);

	drm_dev_unplug(&board->drm);
	calculet_resources_release(pdev, &board->res);
	pci_disable_device(pdev);

	WRITE_ONCE(board->pdev, NULL);
	pci_set_drvdata(pdev, NULL);
	calculet_board_mgr_remove_board(board);
}

/* ---------------------------------------------------------------- */
/* Driver and module                                                */
/* ---------------------------------------------------------------- */

static const struct pci_error_handlers calculet_err_handlers = {
	.error_detected	= calculet_error_detected,
	.slot_reset	= calculet_slot_reset,
	.resume		= calculet_error_resume,
};

static struct pci_device_id calculet_pci_ids[] = {
	{ PCI_DEVICE(CALCULET_PCI_VENDOR_ID, PCI_ANY_ID),
	  .driver_data = (kernel_ulong_t)&calculet_legacy_profile },
	{ }
};
MODULE_DEVICE_TABLE(pci, calculet_pci_ids);

static struct pci_driver calculet_pci_driver = {
	.name		= CALCULET_DRV_NAME,
	.id_table	= calculet_pci_ids,
	.probe		= calculet_probe,
	.remove		= calculet_remove,
	.shutdown	= calculet_shutdown,
	.err_handler	= &calculet_err_handlers,
	.dev_groups	= calculet_groups,
	.driver		= {
		.pm	= pm_sleep_ptr(&calculet_pm_ops),
	},
};

static int __init calculet_pci_init(void)
{
	int ret;

	ret = calculet_board_mgr_init();
	if (ret)
		return ret;

	ret = calculet_proc_init();
	if (ret)
		goto err_mgr;

	ret = pci_register_driver(&calculet_pci_driver);
	if (ret)
		goto err_proc;

	return 0;

err_proc:
	calculet_proc_cleanup();
err_mgr:
	calculet_board_mgr_cleanup();
	return ret;
}

static void __exit calculet_pci_exit(void)
{
	pci_unregister_driver(&calculet_pci_driver);
	calculet_proc_cleanup();
	calculet_board_mgr_cleanup();
}

module_init(calculet_pci_init);
module_exit(calculet_pci_exit);

MODULE_AUTHOR("Calculet Technologies Ltd.");
MODULE_DESCRIPTION("CALCULET KS1 PCIe accel driver");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.1.4");
