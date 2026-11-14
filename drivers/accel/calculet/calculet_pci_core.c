// SPDX-License-Identifier: GPL-2.0-only
/*
 * calculet_pci_core.c - PCI probe/remove, BAR mapping, interrupts, power
 * management and DRM accel device registration.
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <drm/drm_accel.h>
#include <drm/drm_drv.h>

#include "calculet_pci.h"

int calculet_log_level = CALCULET_DEFAULT_LOG_LEVEL;
unsigned int dma_timeout_ms = CALCULET_DEFAULT_DMA_TIMEOUT_MS;
unsigned int max_boards = CALCULET_DEFAULT_MAX_BOARDS;
unsigned int poll_mode;
bool calculet_restrict_privileged_io;

module_param_named(restrict_privileged_io, calculet_restrict_privileged_io,
		   bool, 0644);
MODULE_PARM_DESC(restrict_privileged_io,
		 "Require CAP_SYS_RAWIO/CAP_SYS_ADMIN for raw BAR, EP-MSI and reset ioctls");
module_param(dma_timeout_ms, uint, 0444);
MODULE_PARM_DESC(dma_timeout_ms, "DMA transfer timeout in milliseconds");
module_param(max_boards, uint, 0444);
MODULE_PARM_DESC(max_boards, "Maximum number of boards to support");
module_param(poll_mode, uint, 0444);
MODULE_PARM_DESC(poll_mode, "Set 1 for hw polling, default is 0 (interrupts)");
module_param(calculet_log_level, int, 0644);
MODULE_PARM_DESC(calculet_log_level,
		 "Debug level (0=EMERG, 1=ALERT, 2=CRIT, 3=ERR, 4=WARN, 5=NOTICE, 6=INFO, 7=DEBUG)");

static const struct calculet_hw_profile calculet_legacy_profile = {
	.name		= "legacy-ks1",
	.os_bar		= 0,
	.rt_bar		= 2,
	.dma_bar	= 4,
	.rd_channels	= CALCULET_READ_CHANNELS,
	.wr_channels	= CALCULET_WRITE_CHANNELS,
	.os_status_reg	= CALCULET_RT_OS_STATUS,
	.rt_status_reg	= CALCULET_RT_RT_STATUS,
	.soft_reset_reg	= CALCULET_RT_SOFT_RESET,
};

struct calculet_drm_device {
	struct drm_device drm;
};

struct calculet_pci_board *calculet_board_from_dev(struct device *dev)
{
	return dev_get_drvdata(dev);
}

/* ------------------------------------------------------------------ */
/* BAR mapping and register access                                    */
/* ------------------------------------------------------------------ */

static int calculet_bar_iomap(struct pci_dev *pdev, int bar,
			      struct calculet_pci_res *res)
{
	resource_size_t start, end, len;
	unsigned long flags;

	if (!pdev || !res)
		return -EINVAL;

	memset(res, 0, sizeof(*res));
	mutex_init(&res->lock);

	if ((unsigned int)bar >= PCI_STD_NUM_BARS)
		return -EINVAL;

	flags = pci_resource_flags(pdev, bar);
	if (!(flags & IORESOURCE_MEM)) {
		calculet_err("PCI BAR %d is not a memory resource (flags=0x%lx)\n",
			     bar, flags);
		return -EINVAL;
	}

	start = pci_resource_start(pdev, bar);
	end = pci_resource_end(pdev, bar);
	if (!end)
		return -ENODEV;

	len = end - start + 1;
	if (!len)
		return -ENODEV;

	res->phys = start;
	res->size = len;
	res->io = pci_iomap(pdev, bar, len);
	if (!res->io) {
		calculet_err("Probing: Invalid PCIe BAR %d\n", bar);
		return -EINVAL;
	}

	if (flags & IORESOURCE_MEM_64)
		calculet_info("BAR %d is 64-bit\n", bar);
	else
		calculet_info("BAR %d is 32-bit\n", bar);
	calculet_info("BAR %d mapped: phys=0x%llx, size=%zu\n", bar,
		      (u64)res->phys, (size_t)res->size);
	return 0;
}

int pcie_resources_init(struct pci_dev *pdev, void *resources)
{
	struct calculet_pci_board *board;
	struct calculet_pci_res *res = resources;
	const struct calculet_hw_profile *profile;
	resource_size_t rt_need;
	int ret;

	if (!pdev || !res)
		return -EINVAL;

	board = pci_get_drvdata(pdev);
	profile = board && board->profile ? board->profile
					  : &calculet_legacy_profile;

	memset(res, 0, sizeof(*res) * 3);

	ret = pci_request_regions(pdev, CALCULET_DRV_NAME);
	if (ret < 0) {
		calculet_err("Probing: Error allocating bars %d\n", ret);
		return ret;
	}
	board->regions_requested = true;

	ret = calculet_bar_iomap(pdev, profile->os_bar, &res[CALCULET_RES_OS]);
	if (ret)
		goto err;
	ret = calculet_bar_iomap(pdev, profile->rt_bar, &res[CALCULET_RES_RT]);
	if (ret)
		goto err;
	ret = calculet_bar_iomap(pdev, profile->dma_bar,
				 &res[CALCULET_RES_DEVINFO]);
	if (ret)
		goto err;

	rt_need = max_t(resource_size_t,
			(resource_size_t)profile->rd_channels * 0x200 + 0x11c,
			(resource_size_t)profile->wr_channels * 0x200 + 0x1c);
	if (res[CALCULET_RES_OS].size <
	    max3((u32)profile->os_status_reg, (u32)profile->rt_status_reg,
		 (u32)profile->soft_reset_reg) + 4) {
		calculet_err("Profile %s RT register layout exceeds BAR %u (size=%zu)\n",
			     profile->name, profile->rt_bar,
			     (size_t)res[CALCULET_RES_RT].size);
		ret = -ENODEV;
		goto err;
	}
	if (res[CALCULET_RES_RT].size < rt_need) {
		calculet_err("Profile %s DMA register layout exceeds BAR %u (size=%zu)\n",
			     profile->name, profile->os_bar,
			     (size_t)res[CALCULET_RES_OS].size);
		ret = -ENODEV;
		goto err;
	}

	board->operational = true;
	return 0;

err:
	pcie_resources_release(pdev, res);
	return ret;
}

void pcie_resources_release(struct pci_dev *pdev, void *resources)
{
	struct calculet_pci_res *res = resources;
	struct calculet_pci_board *board = pci_get_drvdata(pdev);
	int i;

	if (!pdev || !res)
		return;

	board->operational = false;
	for (i = 0; i < 3; i++) {
		if (!res[i].io)
			continue;
		pci_iounmap(pdev, res[i].io);
		res[i].io = NULL;
		res[i].phys = 0;
		res[i].size = 0;
	}
	if (board->regions_requested) {
		pci_release_regions(pdev);
		board->regions_requested = false;
	}
}

u32 calculet_resource_read32(struct calculet_pci_res *res, unsigned long off)
{
	unsigned long flags;
	u32 val;

	if (!res || !res->io || (off & 3) || off > res->size ||
	    res->size - off < 4)
		return 0xffffffff;

	spin_lock_irqsave(&res->reg_lock, flags);
	val = readl(res->io + off);
	spin_unlock_irqrestore(&res->reg_lock, flags);
	return val;
}

u64 calculet_resource_read64(struct calculet_pci_res *res, unsigned long off)
{
	unsigned long flags;
	u64 val;

	if (!res || !res->io || (off & 3) || off > res->size ||
	    res->size - off < 8)
		return 0xffffffffffffffffULL;

	spin_lock_irqsave(&res->reg_lock, flags);
	val = readl(res->io + off);
	val |= (u64)readl(res->io + off + 4) << 32;
	spin_unlock_irqrestore(&res->reg_lock, flags);
	return val;
}

void calculet_resource_write32(struct calculet_pci_res *res, unsigned long off,
			       u32 val)
{
	unsigned long flags;

	if (!res || !res->io || (off & 3) || off > res->size ||
	    res->size - off < 4)
		return;

	spin_lock_irqsave(&res->reg_lock, flags);
	writel(val, res->io + off);
	spin_unlock_irqrestore(&res->reg_lock, flags);
}

void calculet_resource_write64(struct calculet_pci_res *res, unsigned long off,
			       u64 val)
{
	unsigned long flags;

	if (!res || !res->io || (off & 3) || off > res->size ||
	    res->size - off < 8)
		return;

	spin_lock_irqsave(&res->reg_lock, flags);
	writel(lower_32_bits(val), res->io + off);
	writel(upper_32_bits(val), res->io + off + 4);
	spin_unlock_irqrestore(&res->reg_lock, flags);
}

void calculet_read_strings(void *dst, size_t dst_size,
			   struct calculet_pci_res *res, unsigned long off,
			   size_t len)
{
	unsigned long flags;
	size_t n;

	if (!dst || !dst_size || !res || !res->io || !len)
		return;
	if (off > res->size || res->size - off < len)
		return;

	n = min(dst_size, len);
	spin_lock_irqsave(&res->reg_lock, flags);
	memcpy_fromio(dst, res->io + off, n);
	spin_unlock_irqrestore(&res->reg_lock, flags);
	if (n < dst_size)
		((char *)dst)[n] = '\0';
}

static int calculet_bar32_transfer(struct calculet_pci_res *res,
				   struct calculet_bar32_transfer_params *p)
{
	if (!res || !p || (p->address & 3) || p->address > res->size ||
	    res->size - p->address < 4)
		return -EINVAL;

	switch (p->op) {
	case CALCULET_BAR_OP_READ:
		p->value = calculet_resource_read32(res, p->address);
		return 0;
	case CALCULET_BAR_OP_WRITE:
		calculet_resource_write32(res, p->address, p->value);
		return 0;
	default:
		return -EINVAL;
	}
}

static int calculet_bar64_transfer(struct calculet_pci_res *res,
				   struct calculet_bar64_transfer_params *p)
{
	if (!res || !p || (p->address & 7) || p->address > res->size ||
	    res->size - p->address < 8)
		return -EINVAL;

	switch (p->op) {
	case CALCULET_BAR_OP_READ:
		p->value = calculet_resource_read64(res, p->address);
		return 0;
	case CALCULET_BAR_OP_WRITE:
		calculet_resource_write64(res, p->address, p->value);
		return 0;
	default:
		return -EINVAL;
	}
}

int calculet_pcie_bar32_transfer(struct calculet_pci_board *board,
				 struct calculet_bar32_transfer_params *p)
{
	if (!board || !p)
		return -EINVAL;
	if (!board->operational)
		return -ESRCH;

	switch (p->bar) {
	case CALCULET_BAR_RT:
		return calculet_bar32_transfer(&board->res[CALCULET_RES_RT], p);
	case CALCULET_BAR_DEVINFO:
		/* BAR4 is write-only for the driver; reads use DEVICE_INFO. */
		return calculet_bar32_transfer(&board->res[CALCULET_RES_DEVINFO],
					       p);
	default:
		return -EINVAL;
	}
}

int calculet_pcie_bar64_transfer(struct calculet_pci_board *board,
				 struct calculet_bar64_transfer_params *p)
{
	if (!board || !p)
		return -EINVAL;
	if (!board->operational)
		return -ESRCH;

	switch (p->bar) {
	case CALCULET_BAR_RT:
		return calculet_bar64_transfer(&board->res[CALCULET_RES_RT], p);
	case CALCULET_BAR_DEVINFO:
		return calculet_bar64_transfer(&board->res[CALCULET_RES_DEVINFO],
					       p);
	default:
		return -EINVAL;
	}
}

/* ------------------------------------------------------------------ */
/* MSI programming and IRQ handling                                   */
/* ------------------------------------------------------------------ */

static int calculet_pcie_enable_interrupts(struct pci_dev *pdev,
					   struct calculet_pci_res *res)
{
	struct calculet_pci_board *board = pci_get_drvdata(pdev);
	u16 control = 0;
	u32 addr_lo = 0, addr_hi = 0, data = 0;
	int cap, i, ret;

	if (!pdev || !res || !board || !board->operational)
		return -EINVAL;

	cap = pci_find_capability(pdev, PCI_CAP_ID_MSI);
	if (!cap) {
		calculet_err("Device does not support MSI\n");
		return -ESRCH;
	}

	ret = pci_read_config_word(pdev, cap + PCI_MSI_FLAGS, &control);
	if (ret) {
		calculet_err("Failed to read MSI configuration\n");
		return -EIO;
	}
	ret = pci_read_config_dword(pdev, cap + PCI_MSI_ADDRESS_LO, &addr_lo);
	if (ret) {
		calculet_err("Failed to read MSI configuration\n");
		return -EIO;
	}

	if (control & PCI_MSI_FLAGS_64BIT) {
		ret = pci_read_config_dword(pdev, cap + PCI_MSI_ADDRESS_HI,
					    &addr_hi);
		if (ret)
			goto err_64;
		ret = pci_read_config_dword(pdev, cap + PCI_MSI_DATA_64, &data);
		if (ret)
			goto err_64;
	} else {
		ret = pci_read_config_dword(pdev, cap + PCI_MSI_DATA_32, &data);
		if (ret)
			goto err_32;
	}

	if (!addr_lo) {
		calculet_err("MSI address is 0, MSI may not be properly enabled\n");
		return -EINVAL;
	}

	calculet_info("MSI: control=0x%x, addr_l=0x%x, addr_h=0x%x, data=0x%x\n",
		      control, addr_lo, addr_hi, data);

	calculet_resource_write32(res, CALCULET_RD_MSI_ADDR_LO, addr_lo);
	calculet_resource_write32(res, CALCULET_WR0_MSI_ADDR_LO, addr_lo);
	if (control & PCI_MSI_FLAGS_64BIT) {
		calculet_resource_write32(res, CALCULET_RD_MSI_ADDR_HI, addr_hi);
		calculet_resource_write32(res, CALCULET_WR0_MSI_ADDR_HI, addr_hi);
	}
	calculet_resource_write32(res, CALCULET_WR1_MSI_ADDR_LO, addr_lo);
	calculet_resource_write32(res, CALCULET_WR2_MSI_ADDR_LO, addr_lo);
	if (control & PCI_MSI_FLAGS_64BIT) {
		calculet_resource_write32(res, CALCULET_WR1_MSI_ADDR_HI, addr_hi);
		calculet_resource_write32(res, CALCULET_WR2_MSI_ADDR_HI, addr_hi);
	}

	for (i = 0; i < 4; i++) {
		data = ((data + 1) << 16) | (data & 0xffff);
		calculet_resource_write32(res, CALCULET_WR_MSI_DATA0 + i * 4,
					  data);
		data += 2;
	}
	for (i = 0; i < 4; i++) {
		data = ((data + 1) << 16) | (data & 0xffff);
		calculet_resource_write32(res, CALCULET_RD_MSI_DATA0 + i * 4,
					  data);
		data += 2;
	}

	calculet_resource_write32(res, CALCULET_OS_IRQ_EN, 1);
	calculet_resource_write32(res, CALCULET_RT_IRQ_EN, 1);
	return 0;

err_64:
	calculet_err("Failed to read 64-bit MSI configuration\n");
	return -EIO;
err_32:
	calculet_err("Failed to read 32-bit MSI configuration\n");
	return -EIO;
}

static void calculet_pcie_disable_interrupts(struct pci_dev *pdev,
					     struct calculet_pci_res *res)
{
	struct calculet_pci_board *board = pci_get_drvdata(pdev);

	if (!pdev || !res || !board || !board->operational)
		return;

	calculet_resource_write32(res, CALCULET_OS_IRQ_EN, 0);
	calculet_resource_write32(res, CALCULET_RT_IRQ_EN, 0);
}

static void calculet_dma_channel_init(struct calculet_dma_channel *ch,
				      struct calculet_dma_controller *ctrl,
				      struct calculet_pci_board *board,
				      u8 id, u8 vector, bool is_write,
				      int irq)
{
	memset(ch, 0, sizeof(*ch));
	ch->id = id;
	ch->vector = vector;
	ch->is_write = is_write;
	ch->irq = irq;
	ch->ctrl = ctrl;
	ch->board = board;
	ch->os_base = board->res[CALCULET_RES_OS].io;
	spin_lock_init(&ch->lock);
	init_waitqueue_head(&ch->wq);
	mutex_init(&ch->buf_lock);
	mutex_init(&ch->ioctl_lock);
	calculet_dma_channel_set_state(ch, CALCULET_DMA_IDLE);
}

int calculet_enable_interrupts(struct calculet_pci_board *board)
{
	struct pci_dev *pdev;
	struct calculet_pci_res *res;
	int vectors, ret, i;

	if (!board || !board->pdev) {
		calculet_err("Invalid parameters for interrupt setup\n");
		return -EINVAL;
	}
	pdev = board->pdev;
	res = &board->res[CALCULET_RES_OS];

	if (!board->operational)
		return -ESRCH;

	if (poll_mode) {
		if (board->irq_enabled)
			return -EBUSY;
		ret = calculet_pcie_enable_poll(pdev, res);
		if (ret) {
			calculet_err("Failed to configure poll-mode DMA interrupts: %d\n",
				     ret);
			return ret;
		}
		board->irq_enabled = true;
		return 0;
	}

	if (board->irq_enabled) {
		calculet_err("Failed enabling interrupts (already enabled)\n");
		return -EBUSY;
	}

	vectors = pci_alloc_irq_vectors(pdev, 16, CALCULET_MAX_IRQ_VECTORS,
					PCI_IRQ_MSIX);
	if (vectors < 0) {
		calculet_err("Failed to pci_alloc_irq_vectors %d\n", vectors);
		return vectors;
	}
	if (vectors < 16) {
		calculet_err("PCI allocated only %d IRQ vectors, need %d for DMA\n",
			     vectors, 16);
		ret = -ENOSPC;
		goto err_vectors;
	}

	board->irq_vectors = vectors;
	board->irq_count = 0;
	memset(board->irq_used, 0, sizeof(board->irq_used));

	for (i = 0; i < 8; i++) {
		struct calculet_dma_channel *ch = &board->dma.rd[i];

		ret = pci_irq_vector(pdev, i);
		if (ret < 0)
			goto err_irqs;
		calculet_dma_channel_init(ch, &board->dma, board, i, i, false,
					  ret);
		ret = request_threaded_irq(ch->irq, calculet_irqhandler, NULL,
					   IRQF_ONESHOT | IRQF_SHARED,
					   CALCULET_IRQ_NAME, ch);
		if (ret) {
			calculet_err("Failed to request IRQ for read channel %d\n",
				     i);
			goto err_irqs;
		}
		ch->irq_registered = true;
		board->irq_used[i] = true;
		board->irq_count++;
	}

	for (i = 0; i < 8; i++) {
		struct calculet_dma_channel *ch = &board->dma.wr[i];
		u8 vector = 8 + i;

		ret = pci_irq_vector(pdev, vector);
		if (ret < 0)
			goto err_irqs;
		calculet_dma_channel_init(ch, &board->dma, board, i, vector,
					  true, ret);
		ret = request_threaded_irq(ch->irq, calculet_irqhandler, NULL,
					   IRQF_ONESHOT | IRQF_SHARED,
					   CALCULET_IRQ_NAME, ch);
		if (ret) {
			calculet_err("Failed to request IRQ for write channel %d\n",
				     i);
			goto err_irqs;
		}
		ch->irq_registered = true;
		board->irq_used[vector] = true;
		board->irq_count++;
	}

	for (i = 0; i < vectors - 16 && i < 16; i++) {
		struct calculet_dma_channel *ch = &board->dma.ep[i];
		u8 vector = 16 + i;

		ret = pci_irq_vector(pdev, vector);
		if (ret < 0)
			goto err_irqs;
		calculet_dma_channel_init(ch, &board->dma, board, i, vector,
					  false, ret);
		ret = request_threaded_irq(ch->irq, calculet_irqhandler, NULL,
					   IRQF_ONESHOT | IRQF_SHARED,
					   CALCULET_IRQ_NAME, ch);
		if (ret) {
			calculet_err("Failed to request IRQ for ep msi req %d\n",
				     i);
			goto err_irqs;
		}
		ch->irq_registered = true;
		board->irq_used[vector] = true;
		board->irq_count++;
	}

	ret = calculet_pcie_enable_interrupts(pdev, res);
	if (ret) {
		calculet_err("Failed to configure MSI DMA interrupts: %d\n",
			     ret);
		goto err_irqs;
	}

	board->irq_enabled = true;
	calculet_info("Interrupts enabled successfully (%d vectors)\n",
		      vectors);
	return 0;

err_irqs:
	for (i = 0; i < CALCULET_MAX_IRQ_VECTORS; i++) {
		struct calculet_dma_channel *ch;

		if (!board->irq_used[i])
			continue;
		if (i < 8)
			ch = &board->dma.rd[i];
		else if (i < 16)
			ch = &board->dma.wr[i - 8];
		else if (i < 32)
			ch = &board->dma.ep[i - 16];
		else
			continue;
		free_irq(ch->irq, ch);
		board->irq_used[i] = false;
	}
	calculet_pcie_disable_interrupts(pdev, res);
	board->irq_vectors = 0;
	board->irq_count = 0;
err_vectors:
	pci_free_irq_vectors(pdev);
	return ret;
}

void calculet_disable_interrupts(struct calculet_pci_board *board)
{
	struct pci_dev *pdev;
	struct calculet_pci_res *res;
	int i;

	if (!board || !board->pdev) {
		calculet_err("Failed to access board or device\n");
		return;
	}
	pdev = board->pdev;
	res = &board->res[CALCULET_RES_OS];

	if (poll_mode) {
		calculet_pcie_disable_interrupts(pdev, res);
		board->irq_enabled = false;
		return;
	}

	if (!board->irq_enabled && !board->irq_vectors)
		return;

	board->irq_enabled = false;
	calculet_pcie_disable_interrupts(pdev, res);

	for (i = 0; i < CALCULET_MAX_IRQ_VECTORS; i++) {
		struct calculet_dma_channel *ch;

		if (i >= board->irq_vectors)
			break;
		if (!board->irq_used[i])
			continue;
		if (i < 8)
			ch = &board->dma.rd[i];
		else if (i < 16)
			ch = &board->dma.wr[i - 8];
		else
			ch = &board->dma.ep[i - 16];
		free_irq(ch->irq, ch);
		board->irq_used[i] = false;
	}

	pci_free_irq_vectors(pdev);
	board->irq_vectors = 0;
	board->irq_count = 0;
	calculet_info("Interrupts disabled\n");
}

irqreturn_t calculet_irqhandler(int irq, void *data)
{
	struct calculet_dma_channel *ch = data;
	struct calculet_pci_board *board;
	u8 done = 0, error = 0;
	int ret;

	if (!ch || !ch->board || !ch->ctrl)
		return IRQ_NONE;

	board = ch->board;
	if (!ch->ctrl->ready)
		return IRQ_NONE;

	if (ch->vector >= 16) {
		ch->result = 1;
		wake_up(&ch->wq);
		board->stat_irqs++;
		return IRQ_HANDLED;
	}

	ret = calculet_dma_read_profile_status(ch, ch->vector >= 8, &done,
					       &error);
	if (ret) {
		ch->result = -EIO;
	} else if (done) {
		if (board->profile && board->profile->dma_event)
			board->profile->dma_event(board, ch->vector >= 8,
						  ch->id);
		ch->result = error ? -EIO : 1;
	} else {
		ch->result = -EIO;
	}

	if (ch->armed) {
		ch->armed = false;
		wake_up(&ch->wq);
	}
	board->stat_irqs++;
	return IRQ_HANDLED;
}

int calculet_activate_board(struct calculet_pci_board *board)
{
	int ret, os_status, rt_status;

	if (!board)
		return -EINVAL;

	ret = calculet_enable_interrupts(board);
	if (ret < 0) {
		calculet_err("Failed enabling interrupts %d\n", ret);
		return ret;
	}

	os_status = calculet_resource_read32(&board->res[CALCULET_RES_RT],
					     board->profile->os_status_reg);
	if (os_status == 1)
		calculet_info("Board OS is active\n");
	else
		calculet_err("Board OS is not active\n");

	rt_status = calculet_resource_read32(&board->res[CALCULET_RES_RT],
					     board->profile->rt_status_reg);
	if (rt_status == 1)
		calculet_info("Device RT is active\n");
	else
		calculet_err("Device RT is not active\n");

	calculet_info("Board activated successfully\n");
	return 0;
}

/* ------------------------------------------------------------------ */
/* sysfs                                                              */
/* ------------------------------------------------------------------ */

static ssize_t health_status_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct calculet_pci_board *board = calculet_board_from_dev(dev);
	int ret;
	const char *status;

	if (!board)
		return scnprintf(buf, PAGE_SIZE, "unknown\n");

	ret = calculet_board_health_check(board);
	status = ret == 0 ? "good" : "poor";
	return scnprintf(buf, PAGE_SIZE, "%s (%d)\n", status, ret);
}
static DEVICE_ATTR_RO(health_status);

static ssize_t board_state_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct calculet_pci_board *board = calculet_board_from_dev(dev);
	static const char *const states[] = { "ACTIVE", "SUSPENDED",
					     "REMOVING" };
	int state;

	if (!board)
		return scnprintf(buf, PAGE_SIZE, "unknown\n");

	state = calculet_board_get_state(board);
	if (state < CALCULET_BOARD_ACTIVE || state > CALCULET_BOARD_REMOVING)
		return scnprintf(buf, PAGE_SIZE, "unknown\n");
	return scnprintf(buf, PAGE_SIZE, "%s\n", states[state - 1]);
}
static DEVICE_ATTR_RO(board_state);

static ssize_t poll_mode_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", poll_mode);
}
static DEVICE_ATTR_RO(poll_mode);

static ssize_t driver_version_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n", CALCULET_DRV_VERSION);
}
static DEVICE_ATTR_RO(driver_version);

static struct attribute *calculet_dev_attrs[] = {
	&dev_attr_health_status.attr,
	&dev_attr_board_state.attr,
	&dev_attr_poll_mode.attr,
	&dev_attr_driver_version.attr,
	NULL,
};
ATTRIBUTE_GROUPS(calculet_dev);

/* ------------------------------------------------------------------ */
/* PCI driver                                                         */
/* ------------------------------------------------------------------ */

static const struct pci_device_id calculet_pcie_id_table[] = {
	{ PCI_DEVICE(CALCULET_PCI_VENDOR_ID, PCI_ANY_ID),
	  .driver_data = (kernel_ulong_t)&calculet_legacy_profile },
	{ }
};
MODULE_DEVICE_TABLE(pci, calculet_pcie_id_table);

static int calculet_pcie_probe(struct pci_dev *pdev,
			       const struct pci_device_id *id)
{
	const struct calculet_hw_profile *profile = &calculet_legacy_profile;
	struct calculet_pci_board *board;
	struct calculet_drm_device *cdrm;
	int ret;

	if (id && id->driver_data)
		profile = (const void *)id->driver_data;

	if (profile->rd_channels != CALCULET_READ_CHANNELS ||
	    profile->wr_channels != CALCULET_WRITE_CHANNELS) {
		calculet_err("Profile %s channel layout (%u/%u) is incompatible with legacy ABI (%u/%u)\n",
			     profile->name, profile->rd_channels,
			     profile->wr_channels, CALCULET_READ_CHANNELS,
			     CALCULET_WRITE_CHANNELS);
		return -EOPNOTSUPP;
	}

	board = kzalloc_obj(*board);
	if (!board) {
		calculet_err("Failed to allocate memory for board structure\n");
		return -ENOMEM;
	}

	board->pdev = pdev;
	board->profile = profile;
	calculet_board_init(board);
	pci_set_drvdata(pdev, board);

	ret = pci_enable_device(pdev);
	if (ret) {
		calculet_err("Failed to enable PCI device: %d\n", ret);
		goto err_free;
	}
	pci_set_master(pdev);

	ret = pcie_resources_init(pdev, board->res);
	if (ret) {
		calculet_err("Failed to initialize PCIe resources: %d\n", ret);
		goto err_disable;
	}

	ret = calculet_pcie_dma_controller_init(board);
	if (ret < 0) {
		calculet_err("Failed to initialize DMA controller: %d\n", ret);
		goto err_release;
	}

	ret = calculet_activate_board(board);
	if (ret) {
		calculet_err("Failed to activate board: %d\n", ret);
		goto err_dma;
	}

	ret = calculet_board_mgr_add_board(board);
	if (ret < 0) {
		calculet_err("Failed to register board: %d\n", ret);
		goto err_dma;
	}

	cdrm = devm_drm_dev_alloc(&pdev->dev, &calculet_drm_driver,
				  struct calculet_drm_device, drm);
	if (IS_ERR(cdrm)) {
		ret = PTR_ERR(cdrm);
		calculet_err("Failed to allocate DRM device: %d\n", ret);
		goto err_mgr;
	}
	board->drm = &cdrm->drm;

	ret = drm_dev_register(board->drm, 0);
	if (ret) {
		calculet_err("Failed to register DRM accel device: %d\n", ret);
		board->drm = NULL;
		goto err_mgr;
	}

	calculet_info("Board %u registered with the DRM accel subsystem\n",
		      board->index);
	return 0;

err_mgr:
	calculet_disable_interrupts(board);
	calculet_dma_controller_deinit(&board->dma, poll_mode != 0);
	pcie_resources_release(pdev, board->res);
	pci_disable_device(pdev);
	pci_set_drvdata(pdev, NULL);
	calculet_board_mgr_remove_board(board);
	return ret;

err_dma:
	calculet_disable_interrupts(board);
	calculet_dma_controller_deinit(&board->dma, poll_mode != 0);
err_release:
	pcie_resources_release(pdev, board->res);
err_disable:
	pci_disable_device(pdev);
err_free:
	pci_set_drvdata(pdev, NULL);
	kfree(board);
	return ret;
}

static void calculet_pcie_remove(struct pci_dev *pdev)
{
	struct calculet_pci_board *board = pci_get_drvdata(pdev);

	if (!board) {
		calculet_warn("No board data found during removal\n");
		return;
	}

	calculet_board_begin_remove(board);

	if (board->drm) {
		drm_dev_unplug(board->drm);
		board->drm = NULL;
	}

	calculet_dma_stop_new_transfers(&board->dma);
	calculet_dma_wait_for_transfers(&board->dma);
	calculet_dma_stop_poll_thread(&board->dma, poll_mode != 0);
	calculet_disable_interrupts(board);
	calculet_dma_controller_deinit(&board->dma, poll_mode != 0);

	pcie_resources_release(pdev, board->res);
	pci_disable_device(pdev);
	board->pdev = NULL;
	pci_set_drvdata(pdev, NULL);
	calculet_board_mgr_remove_board(board);
}

static int calculet_pcie_suspend(struct pci_dev *pdev, pm_message_t state)
{
	struct calculet_pci_board *board = pci_get_drvdata(pdev);
	int ret;

	if (!board) {
		calculet_err("suspend: no board data\n");
		return -ESRCH;
	}
	if (board->suspended) {
		calculet_warn("Board %u already suspended\n", board->index);
		return 0;
	}

	calculet_info("Suspending board %u (event=%d)\n", board->index,
		      state.event);

	ret = pci_save_state(pdev);
	if (ret) {
		calculet_err("suspend: pci_save_state failed: %d\n", ret);
		return ret;
	}

	if (board->pdev) {
		calculet_board_set_state(board, CALCULET_BOARD_SUSPENDED);
		calculet_dma_stop_new_transfers(&board->dma);
		calculet_dma_wait_for_transfers(&board->dma);
		wait_event(board->op_wq, atomic_read(&board->active_ops) == 0);
		calculet_dma_stop_poll_thread(&board->dma, poll_mode != 0);
		calculet_disable_interrupts(board);
		calculet_dma_controller_deinit(&board->dma, poll_mode != 0);
		board->operational = false;
	}

	pci_disable_device(pdev);
	ret = pci_set_power_state(pdev, pci_choose_state(pdev, state));
	if (ret)
		calculet_err("suspend: failed to enter PCI power state %d: %d\n",
			     pci_choose_state(pdev, state), ret);

	board->suspended = true;
	calculet_info("Board %u suspend finish\n", board->index);
	return 0;
}

static int calculet_pcie_resume(struct pci_dev *pdev)
{
	struct calculet_pci_board *board = pci_get_drvdata(pdev);
	int ret;

	if (!board) {
		calculet_err("resume: no board data\n");
		return -ESRCH;
	}
	if (board->fail_closed) {
		calculet_err("resume: board %u is fail-closed after PCI error; rebind required\n",
			     board->index);
		return -EIO;
	}
	if (!board->suspended) {
		calculet_warn("Board %u is not suspended\n", board->index);
		return 0;
	}

	calculet_info("Resuming board %u\n", board->index);

	ret = pci_set_power_state(pdev, PCI_D0);
	if (ret) {
		calculet_err("resume: failed to enter PCI D0: %d\n", ret);
		return ret;
	}
	pci_restore_state(pdev);
	ret = pci_enable_device(pdev);
	if (ret) {
		calculet_err("resume: pci_enable_device failed: %d\n", ret);
		return ret;
	}
	pci_set_master(pdev);

	board->operational = false;
	ret = calculet_pcie_dma_controller_init(board);
	if (ret < 0) {
		calculet_err("Failed to re-init DMA controller on resume: %d\n",
			     ret);
		goto err;
	}
	board->operational = true;
	ret = calculet_enable_interrupts(board);
	if (ret < 0) {
		calculet_err("Failed to re-enable interrupts on resume: %d\n",
			     ret);
		calculet_dma_controller_deinit(&board->dma, poll_mode != 0);
		board->operational = false;
		goto err;
	}

	calculet_info("Board %u post-resume status: OS=%s, RT=%s\n",
		      board->index,
		      calculet_resource_read32(&board->res[CALCULET_RES_RT],
					       board->profile->os_status_reg) == 1
			      ? "active" : "inactive",
		      calculet_resource_read32(&board->res[CALCULET_RES_RT],
					       board->profile->rt_status_reg) == 1
			      ? "active" : "inactive");

	board->operational = true;
	board->suspended = false;
	calculet_board_set_state(board, CALCULET_BOARD_ACTIVE);
	calculet_info("Board %u resume finish\n", board->index);
	return 0;

err:
	calculet_err("Failed to restart board %u after resume: %d\n",
		     board->index, ret);
	calculet_disable_interrupts(board);
	pci_clear_master(pdev);
	pci_disable_device(pdev);
	board->suspended = true;
	board->operational = false;
	calculet_board_set_state(board, CALCULET_BOARD_SUSPENDED);
	return ret;
}

static void calculet_pcie_shutdown(struct pci_dev *pdev)
{
	struct calculet_pci_board *board = pci_get_drvdata(pdev);

	calculet_info("System shutdown: stopping board\n");
	if (board)
		calculet_disable_interrupts(board);
	pci_disable_device(pdev);
}

static void calculet_pcie_fail_aer(struct calculet_pci_board *board)
{
	mutex_lock(&board->state_lock);
	board->fail_closed = true;
	calculet_board_set_state(board, CALCULET_BOARD_SUSPENDED);
	mutex_unlock(&board->state_lock);

	if (board->res[CALCULET_RES_OS].io) {
		mutex_lock(&board->res[CALCULET_RES_OS].lock);
		calculet_dma_fail_active_transfers(&board->dma, -EIO);
		board->operational = false;
		mutex_unlock(&board->res[CALCULET_RES_OS].lock);
	} else {
		calculet_dma_fail_active_transfers(&board->dma, -EIO);
		board->operational = false;
	}
	board->stat_errors++;
}

static pci_ers_result_t calculet_error_detected(struct pci_dev *pdev,
						pci_channel_state_t state)
{
	struct calculet_pci_board *board = pci_get_drvdata(pdev);

	calculet_err("PCI error detected, state: %d\n", state);
	if (board)
		calculet_pcie_fail_aer(board);
	return PCI_ERS_RESULT_RECOVERED;
}

static pci_ers_result_t calculet_slot_reset(struct pci_dev *pdev)
{
	struct calculet_pci_board *board = pci_get_drvdata(pdev);

	calculet_err("PCI slot reset requested, but DMA recovery is not defined\n");
	if (board)
		calculet_pcie_fail_aer(board);
	return PCI_ERS_RESULT_RECOVERED;
}

static void calculet_error_resume(struct pci_dev *pdev)
{
	calculet_err("PCI error resume: board remains fail-closed; rebind driver after hardware validation\n");
}

static const struct pci_error_handlers calculet_pcie_err_handlers = {
	.error_detected	= calculet_error_detected,
	.slot_reset	= calculet_slot_reset,
	.resume		= calculet_error_resume,
};

static struct pci_driver calculet_pci_driver = {
	.name		= CALCULET_MOD_NAME,
	.id_table	= calculet_pcie_id_table,
	.probe		= calculet_pcie_probe,
	.remove		= calculet_pcie_remove,
	.suspend	= calculet_pcie_suspend,
	.resume		= calculet_pcie_resume,
	.shutdown	= calculet_pcie_shutdown,
	.err_handler	= &calculet_pcie_err_handlers,
	.dev_groups	= calculet_dev_groups,
};

static int __init calculet_pcie_module_init(void)
{
	int ret;

	calculet_info("Initializing module, version %s\n",
		      CALCULET_DRV_VERSION);

	ret = calculet_board_mgr_init();
	if (ret < 0) {
		calculet_err("Failed to initialize board manager: %d\n", ret);
		return ret;
	}

	ret = calculet_proc_init();
	if (ret < 0) {
		calculet_err("Failed to initialize proc filesystem: %d\n", ret);
		goto err_mgr;
	}

	ret = pci_register_driver(&calculet_pci_driver);
	if (ret) {
		calculet_err("Failed to register PCI driver: %d\n", ret);
		goto err_proc;
	}

	calculet_info("Module initialized successfully\n");
	return 0;

err_proc:
	calculet_proc_cleanup();
err_mgr:
	calculet_board_mgr_cleanup();
	return ret;
}

static void __exit calculet_pcie_module_exit(void)
{
	pci_unregister_driver(&calculet_pci_driver);
	calculet_proc_cleanup();
	calculet_board_mgr_cleanup();
	calculet_info("Module exited successfully\n");
}

module_init(calculet_pcie_module_init);
module_exit(calculet_pcie_module_exit);

MODULE_AUTHOR("Calculet Electronics Co., Ltd. Zhenyu Niu");
MODULE_DESCRIPTION("CALCULET KS1 PCIe accel driver");
MODULE_LICENSE("GPL v2");
MODULE_VERSION(CALCULET_DRV_VERSION);
