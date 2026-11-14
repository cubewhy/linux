// SPDX-License-Identifier: GPL-2.0-only
/*
 * Process accounting exported through /proc/calculet_monitor.
 */

#include <linux/cred.h>
#include <linux/pid.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/slab.h>

#include "calculet_pci.h"

#define CALCULET_PROC_DIR_NAME	"calculet_monitor"

static struct calculet_proc_dir {
	struct proc_dir_entry *root;
	struct proc_dir_entry *summary;
	struct proc_dir_entry *processes;
	bool initialized;
} calculet_proc;

static int calculet_proc_summary_show(struct seq_file *m, void *v);
static int calculet_proc_summary_open(struct inode *inode, struct file *file);
static int calculet_proc_device_show(struct seq_file *m, void *v);
static int calculet_proc_device_open(struct inode *inode, struct file *file);
static int calculet_proc_device_release(struct inode *inode,
					struct file *file);
static int calculet_proc_processes_show(struct seq_file *m, void *v);
static int calculet_proc_processes_open(struct inode *inode, struct file *file);

static const struct proc_ops calculet_summary_ops = {
	.proc_open	= calculet_proc_summary_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static const struct proc_ops calculet_device_ops = {
	.proc_open	= calculet_proc_device_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= calculet_proc_device_release,
};

static const struct proc_ops calculet_processes_ops = {
	.proc_open	= calculet_proc_processes_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static const char *calculet_state_name(struct calculet_board *board)
{
	switch (calculet_board_get_state(board)) {
	case CALCULET_BOARD_ACTIVE:
		return "ACTIVE";
	case CALCULET_BOARD_SUSPENDED:
		return "SUSPEND";
	case CALCULET_BOARD_REMOVING:
		return "REMOVE";
	default:
		return "UNKNOWN";
	}
}

static const char *calculet_device_state_name(struct calculet_board *board)
{
	switch (calculet_board_get_state(board)) {
	case CALCULET_BOARD_ACTIVE:
		return "ACTIVE";
	case CALCULET_BOARD_SUSPENDED:
		return "SUSPENDED";
	case CALCULET_BOARD_REMOVING:
		return "REMOVING";
	default:
		return "UNKNOWN";
	}
}

int calculet_proc_init(void)
{
	if (calculet_proc.initialized)
		return 0;

	calculet_proc.root = proc_mkdir(CALCULET_PROC_DIR_NAME, NULL);
	if (!calculet_proc.root)
		return -ENOMEM;

	calculet_proc.summary = proc_create("summary", 0444,
					    calculet_proc.root,
					    &calculet_summary_ops);
	calculet_proc.processes = proc_create("processes", 0444,
					      calculet_proc.root,
					      &calculet_processes_ops);
	if (!calculet_proc.summary || !calculet_proc.processes)
		goto err;

	calculet_proc.initialized = true;
	return 0;

err:
	proc_remove(calculet_proc.processes);
	proc_remove(calculet_proc.summary);
	proc_remove(calculet_proc.root);
	memset(&calculet_proc, 0, sizeof(calculet_proc));
	return -ENOMEM;
}

void calculet_proc_cleanup(void)
{
	if (!calculet_proc.initialized)
		return;

	proc_remove(calculet_proc.processes);
	proc_remove(calculet_proc.summary);
	proc_remove(calculet_proc.root);
	memset(&calculet_proc, 0, sizeof(calculet_proc));
}

int calculet_proc_add_device(struct calculet_board *board)
{
	if (!calculet_proc.initialized || !board)
		return -EINVAL;

	board->proc_entry = proc_create_data(board->proc_name, 0444,
					     calculet_proc.root,
					     &calculet_device_ops,
					     (void *)(uintptr_t)(board->index + 1));
	if (!board->proc_entry)
		return -ENOMEM;

	return 0;
}

void calculet_proc_remove_device(struct calculet_board *board)
{
	if (board && board->proc_entry) {
		proc_remove(board->proc_entry);
		board->proc_entry = NULL;
	}
}

int calculet_proc_add_process_ref(struct calculet_board *board,
				  struct task_struct *task,
				  struct pid *pid_ref)
{
	struct calculet_process *proc;
	pid_t pid;

	if (!board || !task || !pid_ref)
		return -EINVAL;

	pid = pid_nr(pid_ref);

	guard(mutex)(&board->proc_lock);
	if (!calculet_board_is_operational(board))
		return -ENODEV;

	list_for_each_entry(proc, &board->procs, list) {
		if (proc->pid_ref == pid_ref) {
			proc->open_count++;
			return 0;
		}
	}

	proc = kzalloc_obj(*proc);
	if (!proc)
		return -ENOMEM;

	proc->pid_ref = get_pid(pid_ref);
	proc->pid = pid;
	proc->ppid = task_ppid_nr(task);
	proc->open_count = 1;
	proc->uid = task_uid(task);
	get_task_comm(proc->comm, task);
	proc->active = true;
	list_add_tail(&proc->list, &board->procs);

	return 0;
}

void calculet_proc_remove_process_ref(struct calculet_board *board,
				      struct pid *pid_ref, pid_t pid)
{
	struct calculet_process *proc, *tmp;

	if (!board)
		return;

	guard(mutex)(&board->proc_lock);
	list_for_each_entry_safe(proc, tmp, &board->procs, list) {
		if ((pid_ref && proc->pid_ref == pid_ref) ||
		    (!pid_ref && proc->pid == pid)) {
			if (proc->open_count)
				proc->open_count--;
			if (!proc->open_count) {
				list_del(&proc->list);
				proc->active = false;
				put_pid(proc->pid_ref);
				kfree(proc);
			}
			break;
		}
	}
}

void calculet_proc_cleanup_processes(struct calculet_board *board)
{
	struct calculet_process *proc, *tmp;

	if (!board)
		return;

	guard(mutex)(&board->proc_lock);
	list_for_each_entry_safe(proc, tmp, &board->procs, list) {
		if (proc->open_count)
			continue;
		list_del(&proc->list);
		proc->active = false;
		put_pid(proc->pid_ref);
		kfree(proc);
	}
}

static int calculet_proc_summary_show(struct seq_file *m, void *v)
{
	struct calculet_board *boards[CALCULET_MAX_BOARDS];
	int count, i, total = 0;

	count = calculet_board_mgr_get_all_boards(boards,
						  ARRAY_SIZE(boards));

	seq_puts(m, "CALCULET PCIe Driver Status\n");
	seq_puts(m, "===========================\n");
	seq_printf(m, "Driver Version: %s\n", CALCULET_DRV_VER);
	seq_printf(m, "Active Boards: %d\n\n", count);

	if (!count) {
		seq_puts(m, "No active boards found.\n");
		return 0;
	}

	seq_puts(m, "Board Summary:\n");
	seq_puts(m, "Board  State    Active Procs\n");
	seq_puts(m, "----------------------------\n");

	for (i = 0; i < count; i++) {
		struct calculet_board *board = boards[i];
		struct calculet_process *proc;
		int active = 0;

		{
			guard(mutex)(&board->proc_lock);
			list_for_each_entry(proc, &board->procs, list) {
				if (proc->active && proc->open_count)
					active++;
			}
		}

		seq_printf(m, "%-5u  %-7s  %-11d\n", board->index,
			   calculet_state_name(board), active);
		total += active;
		calculet_board_put(board);
	}

	seq_puts(m, "----------------------------\n");
	seq_printf(m, "Total Active Processes: %d\n", total);
	return 0;
}

static int calculet_proc_summary_open(struct inode *inode, struct file *file)
{
	return single_open(file, calculet_proc_summary_show, NULL);
}

static int calculet_proc_device_show(struct seq_file *m, void *v)
{
	struct calculet_board *board = m->private;
	struct calculet_process *proc;
	int active = 0;

	seq_printf(m, "CALCULET Board %u Process Monitor\n", board->index);
	seq_printf(m, "STATE: %s\n", calculet_device_state_name(board));

	{
		guard(mutex)(&board->proc_lock);
		list_for_each_entry(proc, &board->procs, list) {
			if (proc->active && proc->open_count)
				active++;
		}

		seq_puts(m, "PID     PPID    STATUS\n");
		seq_puts(m, "----------------------\n");
		list_for_each_entry(proc, &board->procs, list) {
			if (proc->active && proc->open_count)
				seq_printf(m, "%-7d %-7d OPEN\n", proc->pid,
					   proc->ppid);
		}
		seq_printf(m, "Active Processes: %d\n", active);
	}

	return 0;
}

static int calculet_proc_device_open(struct inode *inode, struct file *file)
{
	struct calculet_board *board;
	unsigned long id = (unsigned long)pde_data(inode);
	int ret;

	if (!id)
		return -ENODEV;

	board = calculet_board_mgr_lookup(id - 1);
	if (!board)
		return -ENODEV;

	ret = single_open(file, calculet_proc_device_show, board);
	if (ret)
		calculet_board_put(board);

	return ret;
}

static int calculet_proc_device_release(struct inode *inode, struct file *file)
{
	struct seq_file *m = file->private_data;
	struct calculet_board *board = m ? m->private : NULL;
	int ret;

	ret = single_release(inode, file);
	calculet_board_put(board);

	return ret;
}

static int calculet_proc_processes_show(struct seq_file *m, void *v)
{
	struct calculet_board *boards[CALCULET_MAX_BOARDS];
	int count, i, total = 0;

	count = calculet_board_mgr_get_all_boards(boards,
						  ARRAY_SIZE(boards));

	seq_puts(m, "CALCULET Active Process Monitor\n");
	seq_puts(m, "===============================\n\n");

	if (!count) {
		seq_puts(m, "No active boards found.\n");
		return 0;
	}

	seq_puts(m, "BOARD  PID     PPID    STATUS\n");
	seq_puts(m, "------------------------------\n");

	for (i = 0; i < count; i++) {
		struct calculet_board *board = boards[i];
		struct calculet_process *proc;

		{
			guard(mutex)(&board->proc_lock);
			list_for_each_entry(proc, &board->procs, list) {
				if (!proc->active || !proc->open_count)
					continue;
				seq_printf(m, "%-5u  %-7d %-7d OPEN\n",
					   board->index, proc->pid,
					   proc->ppid);
				total++;
			}
		}

		calculet_board_put(board);
	}

	seq_puts(m, "---------------------------------------------------\n");
	seq_printf(m, "Total active processes: %d\n", total);
	return 0;
}

static int calculet_proc_processes_open(struct inode *inode, struct file *file)
{
	return single_open(file, calculet_proc_processes_show, NULL);
}
