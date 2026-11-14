// SPDX-License-Identifier: GPL-2.0-only
/*
 * fops.c - DRM accel file operations and the driver-private ioctls.
 */
#include <linux/capability.h>
#include <linux/crc32.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <drm/drm_accel.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_ioctl.h>

#include "calculet_pci.h"

/*
 * The driver logs through _dev_printk() while the board is attached and
 * through printk() with a "[board detached]" prefix afterwards.
 */
static void calculet_board_log(struct calculet_pci_board *board,
			       const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;
	if (board && board->pdev)
		dev_err(&board->pdev->dev, "%pV", &vaf);
	else
		pr_err("%scalculet: [board detached] %pV", "", &vaf);
	va_end(args);
}

static int calculet_open(struct drm_device *drm, struct drm_file *file_priv)
{
	struct calculet_pci_board *board = calculet_board_from_dev(drm->dev);
	struct calculet_file_private *priv;
	int ret;

	if (!board || !calculet_board_is_operational(board))
		return -ENODEV;

	priv = kzalloc_obj(*priv);
	if (!priv)
		return -ENOMEM;

	if (!calculet_board_get(board)) {
		kfree(priv);
		return -ENODEV;
	}

	priv->board = board;
	priv->pid = task_pid_nr(current);
	priv->pid_ref = get_task_pid(current, PIDTYPE_PID);
	if (!priv->pid_ref) {
		calculet_board_put(board);
		kfree(priv);
		return -ESRCH;
	}

	ret = calculet_proc_add_process_ref(board, current, priv->pid_ref);
	if (ret)
		calculet_info("Failed to add process monitoring for PID %d: %d\n",
			      priv->pid, ret);
	else
		priv->access = true;

	file_priv->driver_priv = priv;
	return 0;
}

static void calculet_postclose(struct drm_device *drm,
			       struct drm_file *file_priv)
{
	struct calculet_file_private *priv = file_priv->driver_priv;

	if (!priv)
		return;

	if (priv->board && priv->access)
		calculet_proc_remove_process_ref(priv->board, priv->pid_ref,
						 priv->pid);
	if (priv->pid_ref)
		put_pid(priv->pid_ref);
	if (priv->board)
		calculet_board_put(priv->board);
	kfree(priv);
	file_priv->driver_priv = NULL;
}

static struct calculet_pci_board *
calculet_ioctl_begin(struct drm_file *file_priv, int *ret)
{
	struct calculet_file_private *priv = file_priv->driver_priv;
	struct calculet_pci_board *board;

	if (!priv || !priv->board) {
		*ret = -ENODEV;
		return NULL;
	}

	board = priv->board;
	if (!board->pdev) {
		*ret = -ENODEV;
		return NULL;
	}

	*ret = calculet_board_begin_op(board);
	if (*ret)
		return NULL;

	calculet_proc_update_process_access(board, priv->pid);
	return board;
}

/* ------------------------------------------------------------------ */
/* Board info                                                         */
/* ------------------------------------------------------------------ */

static u32 calculet_uuid_word(u32 crc, u32 salt)
{
	u32 v = (crc ^ salt) ^ ((crc ^ salt) >> 16);

	v *= 0x45d9f3b;
	v = (v ^ (v >> 16)) * 0x45d9f3b;
	return v ^ (v >> 16);
}

static int calculet_ioctl_get_sn(struct drm_device *drm, void *data,
				 struct drm_file *file_priv)
{
	struct calculet_sn_info *info = data;
	struct calculet_pci_board *board;
	int ret;

	board = calculet_ioctl_begin(file_priv, &ret);
	if (!board)
		return ret;

	if (info->slot >= 16) {
		ret = -EINVAL;
		goto out;
	}

	/* 32 bytes of the per-chip serial string. */
	memset(info->sn, 0, sizeof(info->sn));
	calculet_read_strings(info->sn, sizeof(info->sn),
			      &board->res[CALCULET_RES_DEVINFO],
			      CALCULET_DEVI_SERIAL(info->slot), 0x20);
out:
	calculet_board_end_op(board);
	return ret;
}

static int calculet_ioctl_board_info(struct drm_device *drm, void *data,
				     struct drm_file *file_priv)
{
	struct calculet_board_info *info = data;
	struct calculet_pci_board *board;
	struct pci_dev *pdev;
	char *product;
	u32 crc, words[4];
	u8 uuid[16];
	u16 lnksta = 0;
	int i, ret;

	board = calculet_ioctl_begin(file_priv, &ret);
	if (!board)
		return ret;

	pdev = board->pdev;
	memset(info, 0, sizeof(*info));

	calculet_read_strings(info->device_name, sizeof(info->device_name),
			      &board->res[CALCULET_RES_DEVINFO],
			      CALCULET_DEVI_DEVICE_NAME, 0x20);
	calculet_read_strings(info->product_name, sizeof(info->product_name),
			      &board->res[CALCULET_RES_DEVINFO],
			      CALCULET_DEVI_PRODUCT_NAME, 0x20);
	calculet_read_strings(info->board_name, sizeof(info->board_name),
			      &board->res[CALCULET_RES_DEVINFO],
			      CALCULET_DEVI_BOARD_NAME, 0x20);
	calculet_read_strings(info->mac_address, sizeof(info->mac_address),
			      &board->res[CALCULET_RES_DEVINFO],
			      CALCULET_DEVI_MAC, sizeof(info->mac_address));

	info->device_id = pdev->device;
	info->vendor_id = pdev->vendor;
	info->subsystem_id = pdev->subsystem_device;
	info->subsystem_vendor_id = pdev->subsystem_vendor;
	info->domain_number = pci_domain_nr(pdev->bus) & 0xff;
	info->bus_number = pdev->bus->number;
	info->device_number = PCI_SLOT(pdev->devfn);
	info->function_number = PCI_FUNC(pdev->devfn);

	ret = pcie_capability_read_word(pdev, PCI_EXP_LNKSTA, &lnksta);
	if (ret && calculet_log_level > 3)
		calculet_board_log(board,
				   "Failed to read PCIe link status: %d", ret);
	info->link_speed = lnksta & 0xf;
	info->link_width = (lnksta >> 4) & 0x3f;

	/* Deterministic UUID derived from the product string. */
	product = info->product_name;
	crc = crc32_le(0, product, strnlen(product, sizeof(info->product_name)));
	words[0] = calculet_uuid_word(crc, 0x12345678);
	words[1] = calculet_uuid_word(crc, 0x9abcdef0);
	words[2] = calculet_uuid_word(crc, 0xfedcba98);
	words[3] = calculet_uuid_word(crc, 0x76543210);
	for (i = 0; i < 4; i++) {
		uuid[i * 4 + 0] = words[i] >> 24;
		uuid[i * 4 + 1] = words[i] >> 16;
		uuid[i * 4 + 2] = words[i] >> 8;
		uuid[i * 4 + 3] = words[i];
	}
	uuid[6] = (uuid[6] & 0x0f) | 0x40;
	uuid[8] = (uuid[8] & 0x3f) | 0x80;
	snprintf(info->serial_uuid, sizeof(info->serial_uuid),
		 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
		 uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5],
		 uuid[6], uuid[7], uuid[8], uuid[9], uuid[10], uuid[11],
		 uuid[12], uuid[13], uuid[14], uuid[15]);

	info->api_version = 1;
	info->abi_version = 1;
	strscpy(info->driver_version, CALCULET_DRV_VERSION,
		sizeof(info->driver_version));

	info->memory_size = calculet_resource_read64(&board->res[CALCULET_RES_DEVINFO],
						     CALCULET_DEVI_MEM_SIZE);
	info->memory_bandwidth = calculet_resource_read64(&board->res[CALCULET_RES_DEVINFO],
							  CALCULET_DEVI_MEM_BW);
	info->ddr_frequency = calculet_resource_read32(&board->res[CALCULET_RES_DEVINFO],
						       CALCULET_DEVI_DDR_FREQ);
	info->power_limit = calculet_resource_read32(&board->res[CALCULET_RES_DEVINFO],
						     CALCULET_DEVI_POWER_LIMIT);
	info->temperature_limit = calculet_resource_read32(&board->res[CALCULET_RES_DEVINFO],
							   CALCULET_DEVI_TEMP_LIMIT);

	calculet_board_end_op(board);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Process list                                                       */
/* ------------------------------------------------------------------ */

static int calculet_ioctl_process_list(struct drm_device *drm, void *data,
				       struct drm_file *file_priv)
{
	struct calculet_process_info_list_user *out = data;
	struct calculet_pci_board *board;
	struct calculet_process_info *proc;
	int count = 0;
	int ret;

	board = calculet_ioctl_begin(file_priv, &ret);
	if (!board)
		return ret;

	memset(out, 0, sizeof(*out));
	out->board = board->index;

	mutex_lock(&board->proc_lock);
	list_for_each_entry(proc, &board->procs, list) {
		if (!proc->active || proc->refcount == 0)
			continue;
		if (count >= CALCULET_MAX_PROCESS_ENTRIES)
			break;
		out->procs[count].pid = proc->pid;
		out->procs[count].ppid = proc->ppid;
		out->procs[count].uid = __kuid_val(proc->uid);
		strscpy(out->procs[count].comm, proc->comm,
			sizeof(out->procs[count].comm));
		out->procs[count].active = proc->active;
		count++;
	}
	mutex_unlock(&board->proc_lock);
	out->count = count;

	calculet_board_end_op(board);
	return 0;
}

/* ------------------------------------------------------------------ */
/* EP MSI request                                                     */
/* ------------------------------------------------------------------ */

static int calculet_ioctl_ep_msi(struct drm_device *drm, void *data,
				 struct drm_file *file_priv)
{
	struct calculet_msi_req *req = data;
	struct calculet_dma_channel *ch;
	struct calculet_pci_board *board;
	u32 channel = req->channel;
	long left;
	int ret;

	board = calculet_ioctl_begin(file_priv, &ret);
	if (!board)
		return ret;

	if (calculet_restrict_privileged_io && !capable(CAP_SYS_ADMIN)) {
		ret = -EPERM;
		goto out;
	}
	if (channel >= 16) {
		ret = -EINVAL;
		goto out;
	}

	ch = &board->dma.ep[channel];
	if (!ch->irq_registered || ch->vector >= board->irq_vectors) {
		ret = -EOPNOTSUPP;
		goto out;
	}

	if (mutex_lock_interruptible(&ch->ioctl_lock)) {
		ret = -ERESTARTSYS;
		goto out;
	}

	WRITE_ONCE(ch->result, 0);
	left = wait_event_interruptible_timeout(ch->wq,
						READ_ONCE(ch->result) != 0,
						msecs_to_jiffies(30000));
	if (left <= 0) {
		if (left == 0)
			dev_err(&board->pdev->dev, "EP MSI request timed out");
		WRITE_ONCE(ch->result, 0);
		mutex_unlock(&ch->ioctl_lock);
		ret = left == 0 ? -ETIMEDOUT : -ERESTARTSYS;
		goto out;
	}

	channel = READ_ONCE(ch->result);
	WRITE_ONCE(ch->result, 0);
	mutex_unlock(&ch->ioctl_lock);
	ret = channel == 1 ? 0 : (channel < 0 ? (int)channel : -EIO);
out:
	calculet_board_end_op(board);
	return ret;
}

/* ------------------------------------------------------------------ */
/* Soft reset via PCIe rescan                                         */
/* ------------------------------------------------------------------ */

static int calculet_wait_endpoint_config(struct pci_bus *bus, u32 devfn,
					 u16 expect_vendor, u16 expect_device,
					 unsigned int timeout_ms,
					 u16 *last_vendor)
{
	unsigned int waited;
	u16 vendor = 0, device = 0;

	for (waited = 0; waited < timeout_ms; waited += 100) {
		if (last_vendor)
			*last_vendor = vendor;
		if (!pci_bus_read_config_word(bus, devfn, PCI_VENDOR_ID,
					      &vendor) &&
		    vendor != 0xffff && vendor != 0x0001 &&
		    !pci_bus_read_config_word(bus, devfn, PCI_DEVICE_ID,
					      &device) &&
		    (!expect_vendor || expect_vendor == vendor) &&
		    (!expect_device || expect_device == device))
			return 0;
		msleep(100);
	}
	if (last_vendor)
		*last_vendor = vendor;
	return -ETIMEDOUT;
}

static int calculet_rescan_bus(struct calculet_pci_board *board,
			       struct pci_bus *bus, u32 devfn,
			       u16 expect_vendor, u16 expect_device)
{
	int attempt;

	for (attempt = 0; attempt < 3; attempt++) {
		struct pci_dev *pdev;
		u16 vendor = 0, device = 0;
		int ret, i;

		if (attempt != 1) {
			pdev = pci_get_slot(bus, devfn);
			if (pdev) {
				if (!pci_get_drvdata(pdev) &&
				    calculet_log_level > 3)
					calculet_board_log(
						board,
						"Dropping unbound device at devfn 0x%x before rescan retry %d",
						devfn, attempt);
				pci_stop_and_remove_bus_device(pdev);
				pci_dev_put(pdev);
			}
			msleep(1000);

			if (pci_bus_read_config_word(bus, devfn, PCI_VENDOR_ID,
						     &vendor) ||
			    vendor == 0xffff || vendor == 0x0001 ||
			    pci_bus_read_config_word(bus, devfn, PCI_DEVICE_ID,
						     &device)) {
				if (calculet_log_level > 3)
					calculet_board_log(
						board,
						"Endpoint not reported ready before rescan retry %d; rescanning anyway",
						attempt);
			} else if ((expect_vendor && expect_vendor != vendor) ||
				   (expect_device && expect_device != device)) {
				if (calculet_log_level > 3)
					calculet_board_log(
						board,
						"Unexpected ID at devfn 0x%x: %04x:%04x (expect %04x:%04x)",
						devfn, vendor, device,
						expect_vendor, expect_device);
			}
		}

		pci_lock_rescan_remove();
		ret = pci_rescan_bus(bus);
		pci_unlock_rescan_remove();
		if (ret < 0) {
			if (calculet_log_level > 3)
				calculet_err("PCIe soft reset rescan failed for board %u (attempt %d): %d\n",
					     board->index, attempt, ret);
			continue;
		}

		for (i = 0; i < 20; i++) {
			pdev = pci_get_slot(bus, devfn);
			if (pdev && pci_get_drvdata(pdev)) {
				pci_dev_put(pdev);
				if (calculet_log_level > 5)
					calculet_board_log(
						board,
						"PCIe soft reset rescan bound device at devfn 0x%x (attempt %d)",
						devfn, attempt);
				return 0;
			}
			if (pdev)
				pci_dev_put(pdev);
			msleep(100);
		}
		calculet_err("PCIe soft reset rescan did not bind device at devfn 0x%x (attempt %d/%d)\n",
			     devfn, attempt + 1, 3);
	}
	return -ENODEV;
}

static int calculet_soft_reset(struct calculet_pci_board *board)
{
	struct pci_dev *pdev = board->pdev;
	struct pci_bus *bus;
	u32 devfn;
	u16 vendor, device, last_vendor = 0;
	int ret;

	if (!pdev) {
		calculet_err("Invalid board or device pointer\n");
		return -EINVAL;
	}
	bus = pdev->bus;
	devfn = pdev->devfn;
	vendor = pdev->vendor;
	device = pdev->device;

	ret = calculet_global_soft_reset_prepare(board);
	if (ret)
		return ret;

	mutex_lock(&board->state_lock);
	calculet_board_set_state(board, CALCULET_BOARD_REMOVING);
	mutex_unlock(&board->state_lock);

	wait_event(board->op_wq, atomic_read(&board->active_ops) <= 1);
	board->soft_reset_pending = true;
	calculet_board_reserve_slot(board);

	calculet_resource_write32(&board->res[CALCULET_RES_RT],
				  board->profile->soft_reset_reg, 1);
	msleep(2000);

	/* Wait until the endpoint stops answering config reads. */
	{
		unsigned int waited;
		u16 id = 0;

		for (waited = 0; waited < 3000; waited += 100) {
			if (pci_bus_read_config_word(bus, devfn, PCI_VENDOR_ID,
						     &id) || id == 0xffff ||
			    id == 0x0001)
				break;
			msleep(100);
		}
		if (waited >= 3000 && calculet_log_level > 3)
			calculet_board_log(
				board,
				"Endpoint still answers config reads %ums after reset trigger",
				3000);
	}

	pci_stop_and_remove_bus_device(pdev);
	ret = calculet_wait_endpoint_config(bus, devfn, vendor, device, 15000,
					    &last_vendor);
	if (ret) {
		if (calculet_log_level > 3) {
			calculet_board_log(
				board,
				"Endpoint not ready after %ums (vendor=0x%04x device=0x%04x)",
				15000, last_vendor, device);
			calculet_board_log(
				board,
				"Endpoint readiness poll timed out after %ums (last vendor=0x%04x); attempting rescan anyway",
				15000, last_vendor);
		}
	}

	ret = calculet_rescan_bus(board, bus, devfn, vendor, device);
	if (ret)
		calculet_err("PCIe soft reset failed for board %u after %d rescan attempt(s), ret=%d\n",
			     board->index, 3, ret);

	calculet_global_soft_reset_complete(board);
	return ret;
}

static int calculet_ioctl_soft_reset(struct drm_device *drm, void *data,
				     struct drm_file *file_priv)
{
	struct calculet_pci_board *board;
	int ret;

	board = calculet_ioctl_begin(file_priv, &ret);
	if (!board)
		return ret;

	if (calculet_restrict_privileged_io && !capable(CAP_SYS_ADMIN)) {
		ret = -EPERM;
		goto out;
	}

	ret = calculet_soft_reset(board);
out:
	calculet_board_end_op(board);
	return ret;
}

/* ------------------------------------------------------------------ */
/* Raw BAR access                                                     */
/* ------------------------------------------------------------------ */

static int calculet_ioctl_bar32_transfer(struct drm_device *drm, void *data,
					 struct drm_file *file_priv)
{
	struct calculet_bar32_transfer_params *p = data;
	struct calculet_pci_board *board;
	int ret;

	board = calculet_ioctl_begin(file_priv, &ret);
	if (!board)
		return ret;

	if (calculet_restrict_privileged_io && p->op == CALCULET_BAR_OP_WRITE &&
	    !capable(CAP_SYS_RAWIO)) {
		ret = -EPERM;
		goto out;
	}

	ret = calculet_pcie_bar32_transfer(board, p);
	if (ret < 0) {
		board->stat_errors++;
		calculet_board_log(board, "bar32 transfer failed %d", ret);
	}
out:
	calculet_board_end_op(board);
	return ret;
}

static int calculet_ioctl_bar64_transfer(struct drm_device *drm, void *data,
					 struct drm_file *file_priv)
{
	struct calculet_bar64_transfer_params *p = data;
	struct calculet_pci_board *board;
	int ret;

	board = calculet_ioctl_begin(file_priv, &ret);
	if (!board)
		return ret;

	if (calculet_restrict_privileged_io && p->op == CALCULET_BAR_OP_WRITE &&
	    !capable(CAP_SYS_RAWIO)) {
		ret = -EPERM;
		goto out;
	}

	ret = calculet_pcie_bar64_transfer(board, p);
	if (ret < 0) {
		board->stat_errors++;
		calculet_board_log(board, "bar64 transfer failed %d", ret);
	}
out:
	calculet_board_end_op(board);
	return ret;
}

/* ------------------------------------------------------------------ */
/* DMA                                                                */
/* ------------------------------------------------------------------ */

static int calculet_ioctl_dma_transfer(struct drm_device *drm, void *data,
				       struct drm_file *file_priv)
{
	struct calculet_dma_transfer_channels_params *p = data;
	struct calculet_pci_board *board;
	int ret;

	board = calculet_ioctl_begin(file_priv, &ret);
	if (!board)
		return ret;

	ret = calculet_dma_transfer_params(&board->res[CALCULET_RES_RT],
					   &board->dma, p);
	calculet_board_end_op(board);
	return ret;
}

static int calculet_ioctl_interrupt_poll(struct drm_device *drm, void *data,
					 struct drm_file *file_priv)
{
	struct calculet_pci_board *board;
	int ret;

	board = calculet_ioctl_begin(file_priv, &ret);
	if (!board)
		return ret;

	ret = calculet_dma_poll_once(&board->dma);
	calculet_board_end_op(board);
	return ret;
}

static const struct drm_ioctl_desc calculet_ioctls[] = {
	DRM_IOCTL_DEF_DRV(CALCULET_BOARD_INFO, calculet_ioctl_board_info, 0),
	DRM_IOCTL_DEF_DRV(CALCULET_BAR64_TRANSFER,
			  calculet_ioctl_bar64_transfer, 0),
	DRM_IOCTL_DEF_DRV(CALCULET_BAR32_TRANSFER,
			  calculet_ioctl_bar32_transfer, 0),
	DRM_IOCTL_DEF_DRV(CALCULET_GET_SN, calculet_ioctl_get_sn, 0),
	DRM_IOCTL_DEF_DRV(CALCULET_BLOCK_DMA_TRANSFER,
			  calculet_ioctl_dma_transfer, 0),
	DRM_IOCTL_DEF_DRV(CALCULET_INTERRUPT_POLL,
			  calculet_ioctl_interrupt_poll, 0),
	DRM_IOCTL_DEF_DRV(CALCULET_EP_MSI_REQ, calculet_ioctl_ep_msi, 0),
	DRM_IOCTL_DEF_DRV(CALCULET_SOFT_RESET, calculet_ioctl_soft_reset, 0),
	DRM_IOCTL_DEF_DRV(CALCULET_GET_PROCESS_LIST,
			  calculet_ioctl_process_list, 0),
};

static int calculet_mmap(struct file *file, struct vm_area_struct *vma)
{
	return -EOPNOTSUPP;
}

static const struct file_operations calculet_drm_fops = {
	.owner		= THIS_MODULE,
	.open		= accel_open,
	.release	= drm_release,
	.unlocked_ioctl	= drm_ioctl,
	.compat_ioctl	= drm_compat_ioctl,
	.mmap		= calculet_mmap,
	.llseek		= noop_llseek,
};

const struct drm_driver calculet_drm_driver = {
	.driver_features = DRIVER_COMPUTE_ACCEL,
	.fops = &calculet_drm_fops,
	.open = calculet_open,
	.postclose = calculet_postclose,
	.ioctls = calculet_ioctls,
	.num_ioctls = ARRAY_SIZE(calculet_ioctls),
	.name = CALCULET_DRV_NAME,
	.desc = "CALCULET KS1 PCIe accelerator",
	.major = CALCULET_DRV_VER_MAJOR,
	.minor = CALCULET_DRV_VER_MINOR,
	.patchlevel = CALCULET_DRV_VER_REVISION,
};
