// SPDX-License-Identifier: GPL-2.0-only
/*
 * OPLUS healthinfo, read-only part, for the 4.19 WALT kernel.
 *
 * Node names, modes and the cpu_loading / fg_uids text follow the OPPO
 * oplus_performance_5.10 healthinfo driver (sched/healthinfo/{healthinfo,cpuload}.c).
 * Everything is computed when a node is read: no hooks in sched, mm or block.
 *
 *   /proc/oplus_healthinfo/cpu_loading     0444  "cur_cpuloading: N" + ctrl/logon/trig lines
 *   /proc/oplus_healthinfo/kswapd_loading  0444  "kswapd_loading: N" (format not in any source we have)
 *   /proc/fg_info/fg_uids                  0666  "fg_uids: UID"; written by system_server on top-app change
 */
#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel_stat.h>
#include <linux/mmzone.h>
#include <linux/nodemask.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/seq_file.h>
#include <linux/tick.h>
#include <linux/uaccess.h>

#define OHM_SAMPLE_MS		25
#define OHM_PROC_DIR		"oplus_healthinfo"
#define OHM_FG_DIR		"fg_info"
#define OHM_FG_RW		0666

/* ---- cpu_loading ---- */

static u64 ohm_idle_time(int cpu)
{
	u64 idle_us = -1ULL;

	if (cpu_online(cpu))
		idle_us = get_cpu_idle_time_us(cpu, NULL);
	if (idle_us == -1ULL)
		return kcpustat_cpu(cpu).cpustat[CPUTIME_IDLE];
	return idle_us * NSEC_PER_USEC;
}

static u64 ohm_iowait_time(int cpu)
{
	u64 iowait_us = -1ULL;

	if (cpu_online(cpu))
		iowait_us = get_cpu_iowait_time_us(cpu, NULL);
	if (iowait_us == -1ULL)
		return kcpustat_cpu(cpu).cpustat[CPUTIME_IOWAIT];
	return iowait_us * NSEC_PER_USEC;
}

struct ohm_cpu_sample {
	u64 busy;	/* user + system + iowait + irq + softirq, as in OPPO's code */
	u64 total;	/* busy + idle */
};

static void ohm_cpu_sample_take(struct ohm_cpu_sample *s)
{
	int cpu;

	s->busy = 0;
	s->total = 0;
	for_each_online_cpu(cpu) {
		u64 *c = kcpustat_cpu(cpu).cpustat;
		u64 busy = c[CPUTIME_USER] + c[CPUTIME_SYSTEM] + ohm_iowait_time(cpu) +
			   c[CPUTIME_IRQ] + c[CPUTIME_SOFTIRQ];

		s->busy += busy;
		s->total += busy + ohm_idle_time(cpu);
	}
}

static int ohm_get_cur_cpuload(void)
{
	struct ohm_cpu_sample a, b;
	u64 dbusy, dtotal;

	ohm_cpu_sample_take(&a);
	msleep(OHM_SAMPLE_MS);
	ohm_cpu_sample_take(&b);

	if (b.total <= a.total || b.busy < a.busy)
		return 0;
	dbusy = b.busy - a.busy;
	dtotal = b.total - a.total;
	return (int)min_t(u64, 100, div64_u64(100 * dbusy, dtotal));
}

static int cpu_loading_show(struct seq_file *m, void *v)
{
	seq_printf(m, "cur_cpuloading: %d\ncur_cpu_ctrl: true\ncur_cpu_logon: false\ncur_cpu_trig: false\n",
		   ohm_get_cur_cpuload());
	return 0;
}

static int cpu_loading_open(struct inode *inode, struct file *file)
{
	return single_open(file, cpu_loading_show, NULL);
}

static const struct file_operations cpu_loading_fops = {
	.open		= cpu_loading_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

/* ---- kswapd_loading: all kswapd threads, percent of one CPU over the sample window ---- */

static u64 ohm_kswapd_runtime(void)
{
	u64 sum = 0;
	int nid, i;

	for_each_online_node(nid) {
		/* this tree (QCOM) has up to MAX_KSWAPD_THREADS kswapd tasks per node */
		for (i = 0; i < MAX_KSWAPD_THREADS; i++) {
			struct task_struct *t = READ_ONCE(NODE_DATA(nid)->kswapd[i]);

			if (t)
				sum += t->se.sum_exec_runtime;
		}
	}
	return sum;
}

static int kswapd_loading_show(struct seq_file *m, void *v)
{
	u64 a, b, win;
	ktime_t t0, t1;

	t0 = ktime_get();
	a = ohm_kswapd_runtime();
	msleep(OHM_SAMPLE_MS);
	b = ohm_kswapd_runtime();
	t1 = ktime_get();

	win = ktime_to_ns(ktime_sub(t1, t0));
	/* all kswapd threads summed, percent of one CPU; one node on this SoC */
	seq_printf(m, "kswapd_loading: %d\n",
		   (b > a && win) ? (int)min_t(u64, 100, div64_u64(100 * (b - a), win)) : 0);
	return 0;
}

static int kswapd_loading_open(struct inode *inode, struct file *file)
{
	return single_open(file, kswapd_loading_show, NULL);
}

static const struct file_operations kswapd_loading_fops = {
	.open		= kswapd_loading_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

/* ---- fg_info/fg_uids ---- */

static int fg_uid = -555;	/* same "none" value as the stock driver */

static int fg_uids_show(struct seq_file *m, void *v)
{
	seq_printf(m, "fg_uids: %d\n", READ_ONCE(fg_uid));
	return 0;
}

static int fg_uids_open(struct inode *inode, struct file *file)
{
	return single_open(file, fg_uids_show, NULL);
}

static ssize_t fg_uids_write(struct file *file, const char __user *buf,
			     size_t count, loff_t *ppos)
{
	char kbuf[32];
	long uid;

	if (!count || count >= sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;
	kbuf[count] = '\0';
	if (kstrtol(strim(kbuf), 0, &uid))
		return -EINVAL;
	WRITE_ONCE(fg_uid, (int)uid);
	return count;
}

static const struct file_operations fg_uids_fops = {
	.open		= fg_uids_open,
	.read		= seq_read,
	.write		= fg_uids_write,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int __init oplus_healthinfo_lite_init(void)
{
	struct proc_dir_entry *dir, *fgdir;

	/* runs before the late_initcall in mm/vmscan.c that adds swappiness_para to this dir */
	dir = proc_mkdir(OHM_PROC_DIR, NULL);
	if (dir) {
		proc_create("cpu_loading", 0444, dir, &cpu_loading_fops);
		proc_create("kswapd_loading", 0444, dir, &kswapd_loading_fops);
	} else {
		pr_warn("oplus_healthinfo: proc dir not created\n");
	}

	fgdir = proc_mkdir(OHM_FG_DIR, NULL);
	if (fgdir)
		proc_create("fg_uids", OHM_FG_RW, fgdir, &fg_uids_fops);
	else
		pr_warn("oplus_healthinfo: fg_info dir not created\n");
	return 0;
}
fs_initcall(oplus_healthinfo_lite_init);
