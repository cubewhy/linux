// SPDX-License-Identifier: GPL-2.0-only
/*
 * proc.c - /proc/calculet_monitor and per-board process accounting.
 *
 * Reconstructed from calculet_pci.ko 1.1.4 (proc.c strings at .rodata 0x793).
 */
#include <linux/cred.h>
#include <linux/pid.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "calculet_pci.h"

static struct proc_dir_entry *g_proc_dir;
static struct proc_dir_entry *g_proc_summary;
static struct proc_dir_entry *g_proc_processes;
static bool g_proc_initialized;

static const char *calculet_state_name(int state)
{
	static const char *const names[] = { "ACTIVE", "SUSPENDED",
					     "REMOVING" };

	if (state < CALCULET_BOARD_ACTIVE || state > CALCULET_BOARD_REMOVING)
		return "UNKNOWN";
	return names[state - 1];
}

static int calculet_proc_summary_show(struct seq_file *m, void *v)
{
	struct calculet_pci_board *boards[CALCULET_MAX_BOARDS];
	int count, i, total = 0;

	count = calculet_board_mgr_get_all_boards(boards, CALCULET_MAX_BOARDS);

	seq_puts(m, "CALCULET PCIe Driver Status\n");
	seq_puts(m, "===========================\n");
	seq_printf(m, "Driver Version: %s\n", CALCULET_DRV_VERSION);
	seq_printf(m, "Active Boards: %d\n", count);

	if (!count) {
		seq_puts(m, "No active boards found.\n");
		return 0;
	}

	seq_puts(m, "Board Summary:\n");
	seq_puts(m, "Board  State    Active Procs\n");
	seq_puts(m, "----------------------------\n");

	for (i = 0; i < count; i++) {
		struct calculet_pci_board *board = boards[i];
		struct calculet_process_info *proc;
		int active = 0;

		mutex_lock(&board->proc_lock);
		list_for_each_entry(proc, &board->procs, list) {
			if (proc->active && proc->refcount)
				active++;
		}
		mutex_unlock(&board->proc_lock);
		total += active;
		seq_printf(m, "%-5u  %-7s  %-11d\n", board->index,
			   calculet_state_name(board->state), active);
		calculet_board_mgr_put_board(board);
	}

	seq_puts(m, "----------------------------\n");
	seq_printf(m, "Total Active Processes: %d\n", total);
	return 0;
}

static int calculet_proc_processes_show(struct seq_file *m, void *v)
{
	struct calculet_pci_board *boards[CALCULET_MAX_BOARDS];
	int count, i, total = 0;

	count = calculet_board_mgr_get_all_boards(boards, CALCULET_MAX_BOARDS);

	seq_puts(m, "CALCULET Active Process Monitor\n");
	seq_puts(m, "===============================\n\n");

	if (!count) {
		seq_puts(m, "No active boards found.\n");
		return 0;
	}

	seq_puts(m, "BOARD  PID     PPID    STATUS\n");
	seq_puts(m, "------------------------------\n");

	for (i = 0; i < count; i++) {
		struct calculet_pci_board *board = boards[i];
		struct calculet_process_info *proc;

		mutex_lock(&board->proc_lock);
		list_for_each_entry(proc, &board->procs, list) {
			if (!proc->active || !proc->refcount)
				continue;
			total++;
			seq_printf(m, "%-5u  %-7d %-7d %s\n", board->index,
				   proc->pid, proc->ppid, "OPEN");
		}
		mutex_unlock(&board->proc_lock);
		calculet_board_mgr_put_board(board);
	}

	seq_puts(m, "---------------------------------------------------\n");
	seq_printf(m, "Total active processes: %d\n", total);
	return 0;
}

static int calculet_proc_device_show(struct seq_file *m, void *v)
{
	struct calculet_pci_board *board = m->private;
	struct calculet_process_info *proc;
	int active = 0;

	if (!board) {
		seq_puts(m, "Error: Invalid board data\n");
		return 0;
	}

	seq_printf(m, "CALCULET Board %u Process Monitor\n", board->index);
	seq_printf(m, "STATE: %s\n", calculet_state_name(board->state));

	mutex_lock(&board->proc_lock);
	list_for_each_entry(proc, &board->procs, list) {
		if (proc->active && proc->refcount)
			active++;
	}
	seq_puts(m, "PID     PPID    STATUS\n");
	seq_puts(m, "----------------------\n");
	list_for_each_entry(proc, &board->procs, list) {
		if (!proc->active || !proc->refcount)
			continue;
		seq_printf(m, "%-7d %-7d %s\n", proc->pid, proc->ppid, "OPEN");
	}
	seq_printf(m, "Active Processes: %d\n", active);
	mutex_unlock(&board->proc_lock);
	return 0;
}

static int calculet_proc_summary_open(struct inode *inode, struct file *file)
{
	return single_open(file, calculet_proc_summary_show, NULL);
}

static int calculet_proc_processes_open(struct inode *inode, struct file *file)
{
	return single_open(file, calculet_proc_processes_show, NULL);
}

static int calculet_proc_device_open(struct inode *inode, struct file *file)
{
	struct calculet_pci_board *board = pde_data(inode);
	int ret;

	if (!board)
		return -ESRCH;

	if (!calculet_board_get(board))
		return -ESRCH;

	ret = single_open(file, calculet_proc_device_show, board);
	if (ret)
		calculet_board_mgr_put_board(board);
	return ret;
}

static int calculet_proc_device_release(struct inode *inode, struct file *file)
{
	struct seq_file *m = file->private_data;
	struct calculet_pci_board *board = m ? m->private : NULL;
	int ret;

	ret = single_release(inode, file);
	if (board)
		calculet_board_mgr_put_board(board);
	return ret;
}

static const struct proc_ops calculet_proc_summary_ops = {
	.proc_open	= calculet_proc_summary_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static const struct proc_ops calculet_proc_processes_ops = {
	.proc_open	= calculet_proc_processes_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static const struct proc_ops calculet_proc_device_ops = {
	.proc_open	= calculet_proc_device_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= calculet_proc_device_release,
};

int calculet_proc_init(void)
{
	if (g_proc_initialized)
		return 0;

	g_proc_dir = proc_mkdir("calculet_monitor", NULL);
	if (!g_proc_dir) {
		calculet_err("Failed to create proc directory %s\n",
			     "calculet_monitor");
		return -ENOMEM;
	}

	g_proc_summary = proc_create("summary", 0444, g_proc_dir,
				     &calculet_proc_summary_ops);
	if (!g_proc_summary) {
		calculet_err("Failed to create proc summary file\n");
		proc_remove(g_proc_dir);
		g_proc_dir = NULL;
		return -ENOMEM;
	}

	g_proc_processes = proc_create("processes", 0444, g_proc_dir,
				       &calculet_proc_processes_ops);
	if (!g_proc_processes) {
		calculet_err("Failed to create proc processes file\n");
		proc_remove(g_proc_summary);
		proc_remove(g_proc_dir);
		g_proc_summary = NULL;
		g_proc_dir = NULL;
		return -ENOMEM;
	}

	g_proc_initialized = true;
	calculet_info("Proc filesystem initialized\n");
	return 0;
}

void calculet_proc_cleanup(void)
{
	if (!g_proc_initialized)
		return;

	if (g_proc_processes) {
		proc_remove(g_proc_processes);
		g_proc_processes = NULL;
	}
	if (g_proc_summary) {
		proc_remove(g_proc_summary);
		g_proc_summary = NULL;
	}
	if (g_proc_dir) {
		proc_remove(g_proc_dir);
		g_proc_dir = NULL;
	}
	g_proc_initialized = false;
	calculet_info("Proc filesystem cleaned up\n");
}

int calculet_proc_add_device(struct calculet_pci_board *board)
{
	if (!board || !g_proc_initialized)
		return -EINVAL;

	calculet_board_get(board);
	board->proc_entry = proc_create_data(board->proc_name, 0444, g_proc_dir,
					     &calculet_proc_device_ops, board);
	if (!board->proc_entry) {
		calculet_board_mgr_put_board(board);
		calculet_err("Failed to create proc entry for device %u\n",
			     board->index);
		return -ENOMEM;
	}
	return 0;
}

void calculet_proc_remove_device(struct calculet_pci_board *board)
{
	if (!board || !board->proc_entry)
		return;
	proc_remove(board->proc_entry);
	board->proc_entry = NULL;
	calculet_board_mgr_put_board(board);
}

static void calculet_process_info_free(struct calculet_pci_board *board,
				       struct calculet_process_info *proc)
{
	list_del(&proc->list);
	board->proc_count--;
	proc->active = false;
	put_pid(proc->pid_ref);
	kfree(proc);
}

int calculet_proc_add_process_ref(struct calculet_pci_board *board,
				  struct task_struct *task,
				  struct pid *pid)
{
	struct calculet_process_info *proc;
	u32 pid_nr;

	if (!board || !task || !pid)
		return -EINVAL;

	mutex_lock(&board->proc_lock);
	if (!calculet_board_is_operational(board)) {
		mutex_unlock(&board->proc_lock);
		return -ESRCH;
	}

	list_for_each_entry(proc, &board->procs, list) {
		if (proc->pid_ref == pid) {
			proc->refcount++;
			mutex_unlock(&board->proc_lock);
			return 0;
		}
	}

	proc = kzalloc_obj(*proc);
	if (!proc) {
		mutex_unlock(&board->proc_lock);
		return -ENOMEM;
	}

	get_pid(pid);
	pid_nr = pid_vnr(pid);
	proc->pid = pid_nr;
	proc->ppid = task_pid_nr_ns(task->real_parent, &init_pid_ns);
	proc->pid_ref = pid;
	proc->refcount = 1;
	proc->uid = task_uid(task);
	get_task_comm(proc->comm, task);

	list_add_tail(&proc->list, &board->procs);
	board->proc_count++;
	proc->active = true;
	mutex_unlock(&board->proc_lock);
	return 0;
}

int calculet_proc_add_process(struct calculet_pci_board *board,
			      struct task_struct *task)
{
	struct pid *pid;
	int ret;

	if (!task)
		return -EINVAL;

	pid = get_task_pid(task, PIDTYPE_PID);
	if (!pid)
		return -ESRCH;

	ret = calculet_proc_add_process_ref(board, task, pid);
	put_pid(pid);
	return ret;
}

void calculet_proc_remove_process_ref(struct calculet_pci_board *board,
				      struct pid *pid_ref, u32 pid_nr)
{
	struct calculet_process_info *proc;

	if (!board)
		return;

	mutex_lock(&board->proc_lock);
	list_for_each_entry(proc, &board->procs, list) {
		if (pid_ref ? proc->pid_ref == pid_ref : proc->pid == pid_nr) {
			if (proc->refcount > 0 && --proc->refcount == 0)
				calculet_process_info_free(board, proc);
			break;
		}
	}
	mutex_unlock(&board->proc_lock);
}

void calculet_proc_remove_process(struct calculet_pci_board *board, u32 pid)
{
	if (!board)
		return;
	calculet_proc_remove_process_ref(board, NULL, pid);
}

void calculet_proc_cleanup_processes(struct calculet_pci_board *board)
{
	struct calculet_process_info *proc, *tmp;

	if (!board)
		return;

	mutex_lock(&board->proc_lock);
	list_for_each_entry_safe(proc, tmp, &board->procs, list) {
		if (proc->refcount == 0)
			calculet_process_info_free(board, proc);
	}
	mutex_unlock(&board->proc_lock);
}

/* The vendor driver keeps this as an empty hook; access is tracked by open(). */
void calculet_proc_update_process_access(struct calculet_pci_board *board,
					 u32 pid)
{
}
