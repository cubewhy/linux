// SPDX-License-Identifier: GPL-2.0-only
/*
 * DRM accel file operations and driver-private ioctls.
 */

#include <linux/capability.h>
#include <linux/crc32.h>
#include <linux/cred.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/pci_regs.h>
#include <linux/pid.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <drm/drm_accel.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_ioctl.h>

#include "calculet_pci.h"

#define CALCULET_RESET_POLL_MS		100
#define CALCULET_RESET_GONE_MS		3000
#define CALCULET_RESET_BACK_MS		15000
#define CALCULET_RESET_RESCAN_MAX	3
#define CALCULET_RESET_RESCAN_GAP_MS	1000
#define CALCULET_RESET_BIND_GRACE_MS	2000

struct calculet_file_priv {
	struct pid *pid_ref;
	pid_t pid;
	bool registered;
};

static u32 calculet_mix(u32 val, u32 salt)
{
	val ^= salt;
	val = ((val >> 16) ^ val) * 0x45d9f3b;
	val = ((val >> 16) ^ val) * 0x45d9f3b;
	return (val >> 16) ^ val;
}

static void calculet_uuid(const char *serial, char *out)
{
	u32 base = crc32(0, serial, strlen(serial));
	u32 seeds[4] = {
		calculet_mix(base, 0x12345678),
		calculet_mix(base, 0x9abcdef0),
		calculet_mix(base, 0xfedcba98),
		calculet_mix(base, 0x76543210),
	};
	u8 bytes[16];
	int i;

	for (i = 0; i < 4; i++) {
		bytes[i * 4] = seeds[i] >> 24;
		bytes[i * 4 + 1] = seeds[i] >> 16;
		bytes[i * 4 + 2] = seeds[i] >> 8;
		bytes[i * 4 + 3] = seeds[i];
	}

	bytes[6] = (bytes[6] & 0x0f) | 0x40;
	bytes[8] = (bytes[8] & 0x3f) | 0x80;

	snprintf(out, 37,
		 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
		 bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5],
		 bytes[6], bytes[7], bytes[8], bytes[9], bytes[10], bytes[11],
		 bytes[12], bytes[13], bytes[14], bytes[15]);
}

static struct calculet_resource *
calculet_bar_resource(struct calculet_board *board, u32 bar)
{
	switch (bar) {
	case CALCULET_BAR_RT:
		return &board->res.rt;
	case CALCULET_BAR_SMI:
		return &board->res.smi;
	default:
		return NULL;
	}
}

static int calculet_ioctl_board_info(struct drm_device *dev, void *data,
				     struct drm_file *file)
{
	struct calculet_board *board = container_of(dev, struct calculet_board,
						    drm);
	struct calculet_board_info *info = data;
	struct calculet_resource *smi = &board->res.smi;
	u16 lnksta = 0;
	int ret;

	ret = calculet_board_begin_op(board);
	if (ret)
		return ret;

	memset(info, 0, sizeof(*info));

	if (pcie_capability_read_word(board->pdev, PCI_EXP_LNKSTA, &lnksta))
		calculet_warn(board, "failed to read PCIe link status\n");

	info->link_speed = lnksta & PCI_EXP_LNKSTA_CLS;
	info->link_width = (lnksta & PCI_EXP_LNKSTA_NLW) >>
			   PCI_EXP_LNKSTA_NLW_SHIFT;
	info->vendor_id = board->pdev->vendor;
	info->device_id = board->pdev->device;
	info->subsystem_vendor = board->pdev->subsystem_vendor;
	info->subsystem_device = board->pdev->subsystem_device;
	info->domain = pci_domain_nr(board->pdev->bus);
	info->bus = board->pdev->bus->number;
	info->device = PCI_SLOT(board->pdev->devfn);
	info->function = PCI_FUNC(board->pdev->devfn);

	calculet_read_strings(info->product_name, sizeof(info->product_name),
			      smi, CALCULET_SMI_PRODUCT_NAME, 32);
	calculet_read_strings(info->serial_number, sizeof(info->serial_number),
			      smi, CALCULET_SMI_SERIAL_NUMBER, 32);
	calculet_read_strings(info->product_number, sizeof(info->product_number),
			      smi, CALCULET_SMI_PRODUCT_NUMBER, 32);
	calculet_read_strings(info->firmware_version,
			      sizeof(info->firmware_version), smi,
			      CALCULET_SMI_FIRMWARE_VERSION, 12);
	calculet_uuid(info->serial_number, info->uuid);

	info->total_memory = calculet_reg_read64(smi, CALCULET_SMI_TOTAL_MEMORY);
	info->memory_bandwidth = calculet_reg_read64(
		smi, CALCULET_SMI_MEMORY_BANDWIDTH);
	info->ddr_frequency = calculet_reg_read32(
		smi, CALCULET_SMI_DDR_FREQUENCY);
	info->max_power_limit = calculet_reg_read32(
		smi, CALCULET_SMI_MAX_POWER_LIMIT);
	info->temp_limit = calculet_reg_read32(smi, CALCULET_SMI_TEMP_LIMIT);

	info->driver_version_major = CALCULET_DRV_VER_MAJOR;
	info->driver_version_minor = CALCULET_DRV_VER_MINOR;
	strscpy(info->driver_version, CALCULET_DRV_VER,
		sizeof(info->driver_version));

	calculet_board_end_op(board);
	return 0;
}

static int calculet_ioctl_bar32(struct drm_device *dev, void *data,
				struct drm_file *file)
{
	struct calculet_board *board = container_of(dev, struct calculet_board,
						    drm);
	struct calculet_bar32_transfer_params *args = data;
	struct calculet_resource *res;
	int ret;

	ret = calculet_board_begin_op(board);
	if (ret)
		return ret;

	if (args->direction != CALCULET_TRANSFER_C2H &&
	    args->direction != CALCULET_TRANSFER_H2C) {
		ret = -EINVAL;
		goto out_err;
	}
	res = calculet_bar_resource(board, args->bar);
	if (!res || (args->offset & 3) || args->offset > res->size ||
	    sizeof(u32) > res->size - args->offset) {
		ret = -EINVAL;
		goto out_err;
	}

	if (args->direction == CALCULET_TRANSFER_H2C) {
		if (calculet_restrict_privileged_io &&
		    !capable(CAP_SYS_RAWIO)) {
			ret = -EPERM;
			goto out;
		}
		calculet_reg_write32(res, args->offset, args->value);
	} else {
		args->value = calculet_reg_read32(res, args->offset);
	}
	ret = 0;

out:
	calculet_board_end_op(board);
	return ret;

out_err:
	atomic64_inc(&board->stats.errors);
	calculet_board_end_op(board);
	return ret;
}

static int calculet_ioctl_bar64(struct drm_device *dev, void *data,
				struct drm_file *file)
{
	struct calculet_board *board = container_of(dev, struct calculet_board,
						    drm);
	struct calculet_bar64_transfer_params *args = data;
	struct calculet_resource *res;
	int ret;

	ret = calculet_board_begin_op(board);
	if (ret)
		return ret;

	if (args->direction != CALCULET_TRANSFER_C2H &&
	    args->direction != CALCULET_TRANSFER_H2C) {
		ret = -EINVAL;
		goto out_err;
	}
	res = calculet_bar_resource(board, args->bar);
	if (!res || (args->offset & 3) || args->offset > res->size ||
	    sizeof(u64) > res->size - args->offset) {
		ret = -EINVAL;
		goto out_err;
	}

	if (args->direction == CALCULET_TRANSFER_H2C) {
		if (calculet_restrict_privileged_io &&
		    !capable(CAP_SYS_RAWIO)) {
			ret = -EPERM;
			goto out;
		}
		calculet_reg_write64(res, args->offset, args->value);
	} else {
		args->value = calculet_reg_read64(res, args->offset);
	}
	ret = 0;

out:
	calculet_board_end_op(board);
	return ret;

out_err:
	atomic64_inc(&board->stats.errors);
	calculet_board_end_op(board);
	return ret;
}

static int calculet_ioctl_get_sn(struct drm_device *dev, void *data,
				 struct drm_file *file)
{
	struct calculet_board *board = container_of(dev, struct calculet_board,
						    drm);
	struct calculet_sn_info *sn = data;
	u32 chip = sn->chip_id;
	int ret;

	ret = calculet_board_begin_op(board);
	if (ret)
		return ret;

	if (chip >= CALCULET_MAX_CHIPS) {
		ret = -EINVAL;
		goto out;
	}

	memset(sn, 0, sizeof(*sn));
	sn->chip_id = chip;
	calculet_read_strings(sn->sn, sizeof(sn->sn), &board->res.smi,
			      CALCULET_SMI_CHIP_SN +
			      chip * CALCULET_SMI_CHIP_STRIDE, 32);

out:
	calculet_board_end_op(board);
	return ret;
}

static int calculet_ioctl_dma(struct drm_device *dev, void *data,
			      struct drm_file *file)
{
	struct calculet_board *board = container_of(dev, struct calculet_board,
						    drm);
	int ret;

	ret = calculet_board_begin_op(board);
	if (ret)
		return ret;

	ret = calculet_dma_transfer(&board->dma, &board->res.dma, data);

	calculet_board_end_op(board);
	return ret;
}

static int calculet_ioctl_poll(struct drm_device *dev, void *data,
			       struct drm_file *file)
{
	struct calculet_board *board = container_of(dev, struct calculet_board,
						    drm);
	int ret;

	ret = calculet_board_begin_op(board);
	if (ret)
		return ret;

	ret = calculet_dma_poll(&board->dma);

	calculet_board_end_op(board);
	return ret;
}

static int calculet_ioctl_msi(struct drm_device *dev, void *data,
			      struct drm_file *file)
{
	struct calculet_board *board = container_of(dev, struct calculet_board,
						    drm);
	struct calculet_msi_request *req = data;
	int ret;

	ret = calculet_board_begin_op(board);
	if (ret)
		return ret;

	if (calculet_restrict_privileged_io && !capable(CAP_SYS_ADMIN)) {
		ret = -EPERM;
		goto out;
	}

	ret = calculet_dma_msi_request(&board->dma, req->channel);

out:
	calculet_board_end_op(board);
	return ret;
}

enum calculet_ep_state {
	CALCULET_EP_ABSENT = 0,
	CALCULET_EP_PRESENT,
	CALCULET_EP_MISMATCH,
};

static enum calculet_ep_state
calculet_ep_state(struct pci_bus *bus, unsigned int devfn, u16 vendor,
		  u16 device, u16 *got_vendor, u16 *got_device)
{
	u16 v = 0, d = 0;

	if (got_vendor)
		*got_vendor = 0;
	if (got_device)
		*got_device = 0;

	if (pci_bus_read_config_word(bus, devfn, PCI_VENDOR_ID, &v))
		return CALCULET_EP_ABSENT;
	if (got_vendor)
		*got_vendor = v;
	if (v == 0xffff || v == 0x0001)
		return CALCULET_EP_ABSENT;

	if (pci_bus_read_config_word(bus, devfn, PCI_DEVICE_ID, &d))
		return CALCULET_EP_ABSENT;
	if (got_device)
		*got_device = d;

	if ((vendor && v != vendor) || (device && d != device))
		return CALCULET_EP_MISMATCH;

	return CALCULET_EP_PRESENT;
}

static bool calculet_ep_absent(struct pci_bus *bus, unsigned int devfn)
{
	u16 vendor = 0;

	if (pci_bus_read_config_word(bus, devfn, PCI_VENDOR_ID, &vendor))
		return true;

	return vendor == 0xffff || vendor == 0x0001;
}

static struct pci_dev *calculet_get_bound_dev(struct pci_bus *bus,
					      unsigned int devfn)
{
	struct pci_dev *pdev;
	int waited;

	for (waited = 0; waited < CALCULET_RESET_BIND_GRACE_MS;
	     waited += CALCULET_RESET_POLL_MS) {
		pdev = pci_get_slot(bus, devfn);
		if (!pdev)
			return NULL;
		if (pdev->driver && pci_get_drvdata(pdev))
			return pdev;
		pci_dev_put(pdev);
		msleep(CALCULET_RESET_POLL_MS);
	}

	return NULL;
}

static int calculet_soft_reset(struct calculet_board *board)
{
	struct pci_dev *pdev = board->pdev;
	struct pci_bus *bus, *rescan_bus;
	unsigned int devfn;
	u16 vendor, device;
	int attempt, waited, ret;

	if (!pdev)
		return -EINVAL;
	if (calculet_restrict_privileged_io && !capable(CAP_SYS_ADMIN))
		return -EPERM;

	bus = pdev->bus;
	if (!bus)
		return -ENODEV;

	devfn = pdev->devfn;
	vendor = pdev->vendor;
	device = pdev->device;
	rescan_bus = bus->parent ? bus->parent : bus;

	{
		guard(mutex)(&board->op_mutex);
		if (!calculet_board_is_operational(board))
			return -ENODEV;
		ret = calculet_global_soft_reset_prepare(board);
		if (ret)
			return ret;
		calculet_board_set_state(board, CALCULET_BOARD_REMOVING);
	}

	wait_event(board->op_wq, atomic_read(&board->active_ops) <= 1);

	calculet_board_reserve_slot(board);

	calculet_reg_write32(&board->res.rt, board->profile->soft_reset_reg, 1);
	msleep(2000);

	for (waited = 0; waited < CALCULET_RESET_GONE_MS;
	     waited += CALCULET_RESET_POLL_MS) {
		if (calculet_ep_absent(bus, devfn))
			break;
		msleep(CALCULET_RESET_POLL_MS);
	}

	pci_stop_and_remove_bus_device(pdev);

	for (waited = 0; waited < CALCULET_RESET_BACK_MS;
	     waited += CALCULET_RESET_POLL_MS) {
		enum calculet_ep_state state;
		u16 v = 0, d = 0;

		state = calculet_ep_state(bus, devfn, vendor, device, &v, &d);
		if (state == CALCULET_EP_PRESENT)
			break;
		if (state == CALCULET_EP_MISMATCH)
			calculet_warn(board,
				      "unexpected ID %04x:%04x at devfn 0x%x\n",
				      v, d, devfn);
		msleep(CALCULET_RESET_POLL_MS);
	}

	ret = -ENODEV;
	for (attempt = 0; attempt < CALCULET_RESET_RESCAN_MAX; attempt++) {
		struct pci_dev *stale, *new_dev;
		int rescan;

		if (attempt) {
			stale = pci_get_slot(bus, devfn);
			if (stale) {
				if (!stale->driver)
					pci_stop_and_remove_bus_device(stale);
				pci_dev_put(stale);
			}
			msleep(CALCULET_RESET_RESCAN_GAP_MS);
		}

		pci_lock_rescan_remove();
		rescan = pci_rescan_bus(rescan_bus);
		pci_unlock_rescan_remove();

		if (rescan < 0) {
			ret = rescan;
			continue;
		}

		new_dev = calculet_get_bound_dev(bus, devfn);
		if (!new_dev) {
			ret = -ENODEV;
			continue;
		}
		pci_dev_put(new_dev);
		ret = 0;
		break;
	}

	calculet_global_soft_reset_complete(board);
	return ret;
}

static int calculet_ioctl_soft_reset(struct drm_device *dev, void *data,
				     struct drm_file *file)
{
	struct calculet_board *board = container_of(dev, struct calculet_board,
						    drm);
	int ret;

	ret = calculet_board_begin_op(board);
	if (ret)
		return ret;

	ret = calculet_soft_reset(board);

	calculet_board_end_op(board);
	return ret;
}

static int calculet_ioctl_process_list(struct drm_device *dev, void *data,
				       struct drm_file *file)
{
	struct calculet_board *board = container_of(dev, struct calculet_board,
						    drm);
	struct calculet_process_list *result = data;
	struct calculet_process *proc;
	unsigned int count = 0;
	int ret;

	ret = calculet_board_begin_op(board);
	if (ret)
		return ret;

	memset(result, 0, sizeof(*result));
	result->board_index = board->index;

	{
		guard(mutex)(&board->proc_lock);
		list_for_each_entry(proc, &board->procs, list) {
			struct calculet_process_info *out;

			if (!proc->active ||
			    count >= CALCULET_MAX_PROCESS_ENTRIES)
				continue;
			out = &result->process[count];
			out->pid = proc->pid;
			out->ppid = proc->ppid;
			out->uid = from_kuid(&init_user_ns, proc->uid);
			strscpy(out->comm, proc->comm, sizeof(out->comm));
			out->active = proc->active ? 1 : 0;
			count++;
		}
	}
	result->count = count;

	calculet_board_end_op(board);
	return 0;
}

int calculet_open(struct drm_device *dev, struct drm_file *file)
{
	struct calculet_board *board = container_of(dev, struct calculet_board,
						    drm);
	struct calculet_file_priv *priv;
	struct pid *pid_ref;
	int ret;

	priv = kzalloc_obj(*priv);
	if (!priv)
		return -ENOMEM;

	pid_ref = get_task_pid(current, PIDTYPE_TGID);
	if (!pid_ref) {
		kfree(priv);
		return -ESRCH;
	}
	priv->pid_ref = pid_ref;
	priv->pid = pid_nr(pid_ref);

	{
		guard(mutex)(&board->op_mutex);
		if (!calculet_board_is_operational(board)) {
			ret = -ENODEV;
			goto err;
		}
		ret = calculet_proc_add_process_ref(board, current, pid_ref);
	}
	if (!ret)
		priv->registered = true;

	file->driver_priv = priv;
	return 0;

err:
	put_pid(pid_ref);
	kfree(priv);
	return ret;
}

void calculet_postclose(struct drm_device *dev, struct drm_file *file)
{
	struct calculet_board *board = container_of(dev, struct calculet_board,
						    drm);
	struct calculet_file_priv *priv = file->driver_priv;

	if (!priv)
		return;

	if (priv->registered)
		calculet_proc_remove_process_ref(board, priv->pid_ref,
						 priv->pid);
	put_pid(priv->pid_ref);
	kfree(priv);
}

static const struct drm_ioctl_desc calculet_ioctls[] = {
	DRM_IOCTL_DEF_DRV(CALCULET_BOARD_INFO, calculet_ioctl_board_info, 0),
	DRM_IOCTL_DEF_DRV(CALCULET_BAR64_TRANSFER, calculet_ioctl_bar64, 0),
	DRM_IOCTL_DEF_DRV(CALCULET_BAR32_TRANSFER, calculet_ioctl_bar32, 0),
	DRM_IOCTL_DEF_DRV(CALCULET_GET_SN, calculet_ioctl_get_sn, 0),
	DRM_IOCTL_DEF_DRV(CALCULET_BLOCK_DMA_TRANSFER, calculet_ioctl_dma, 0),
	DRM_IOCTL_DEF_DRV(CALCULET_INTERRUPT_POLL, calculet_ioctl_poll, 0),
	DRM_IOCTL_DEF_DRV(CALCULET_EP_MSI_REQ, calculet_ioctl_msi, 0),
	DRM_IOCTL_DEF_DRV(CALCULET_SOFT_RESET, calculet_ioctl_soft_reset, 0),
	DRM_IOCTL_DEF_DRV(CALCULET_GET_PROCESS_LIST,
			  calculet_ioctl_process_list, 0),
};

DEFINE_DRM_ACCEL_FOPS(calculet_fops);

const struct drm_driver calculet_drm_driver = {
	.driver_features	= DRIVER_COMPUTE_ACCEL | DRIVER_GEM,
	.open			= calculet_open,
	.postclose		= calculet_postclose,
	.ioctls			= calculet_ioctls,
	.num_ioctls		= ARRAY_SIZE(calculet_ioctls),
	.fops			= &calculet_fops,
	.name			= "calculet",
	.desc			= "CALCULET KS1 PCIe accelerator",
	.major			= 1,
	.minor			= 0,
};
