// SPDX-License-Identifier: GPL-2.0
/*
 * pipa-cos17 debug build only: if Android has not started system_server after pipa_bootdbg= seconds,
 * dump the stuck tasks and panic, so the console log survives in pstore (reboot=panic_warm) and can be
 * read from the next boot. pipa_hooks= switches the SukiSU/susfs inline hooks for bisecting:
 * bit 0 exec (sucompat), bit 1 stat, bit 2 faccessat, bit 3 exec also runs the ksud handler (second_stage, zygote).
 * If the workqueue cannot run (CPUs spinning with preemption off), an hrtimer 30 s later asks every CPU for its
 * stack by IPI and panics from interrupt context.
 */
#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/moduleparam.h>
#include <linux/nmi.h>
#include <linux/sched.h>
#include <linux/sched/debug.h>
#include <linux/sched/signal.h>
#include <linux/smp.h>
#include <linux/string.h>
#include <linux/workqueue.h>

int pipa_hooks = 15;
core_param(pipa_hooks, pipa_hooks, int, 0644);
static int pipa_bootdbg = 90;
core_param(pipa_bootdbg, pipa_bootdbg, int, 0644);
/* 1: panic at pipa_bootdbg seconds even if the boot went fine, to keep this boot's log in pstore */
static int pipa_bootdbg_force;
core_param(pipa_bootdbg_force, pipa_bootdbg_force, int, 0644);

static struct delayed_work pipa_bootdbg_work;
static struct hrtimer pipa_bootdbg_timer;
static bool pipa_bootdbg_done;
static DEFINE_PER_CPU(call_single_data_t, pipa_bootdbg_csd);

static bool pipa_task_seen(const char *comm)
{
	struct task_struct *p;
	bool found = false;

	rcu_read_lock();
	for_each_process(p) {
		if (!strcmp(p->comm, comm)) {
			found = true;
			break;
		}
	}
	rcu_read_unlock();
	return found;
}

static void pipa_bootdbg_fn(struct work_struct *work)
{
	struct task_struct *g, *p;

	WRITE_ONCE(pipa_bootdbg_done, true);
	if (pipa_bootdbg_force)
		panic("pipa_bootdbg: forced panic to keep the boot log");
	if (pipa_task_seen("system_server")) {
		pr_info("pipa_bootdbg: system_server is running, watchdog off\n");
		return;
	}
	pr_emerg("pipa_bootdbg: no system_server after %d s (pipa_hooks=%d), dumping tasks\n",
		 pipa_bootdbg, pipa_hooks);
	rcu_read_lock();
	for_each_process(p)
		pr_emerg("pipa_bootdbg: pid %d ppid %d comm %s state %ld threads %d\n", p->pid,
			 task_pid_nr(rcu_dereference(p->real_parent)), p->comm, p->state,
			 get_nr_threads(p));
	for_each_process_thread(g, p) {
		/* stuck (D), running, or anything in init: those are the stacks that explain a stall */
		if (p->state == TASK_UNINTERRUPTIBLE || p->state == TASK_RUNNING || g->pid == 1) {
			sched_show_task(p);
			touch_nmi_watchdog();
		}
	}
	rcu_read_unlock();
	show_workqueue_state();
	panic("pipa_bootdbg: boot stalled");
}

static void pipa_bootdbg_cpu_stack(void *info)
{
	pr_emerg("pipa_bootdbg: stack of CPU %d (comm %s pid %d)\n", smp_processor_id(), current->comm,
		 current->pid);
	dump_stack();
}

static enum hrtimer_restart pipa_bootdbg_timer_fn(struct hrtimer *t)
{
	int cpu, self = smp_processor_id();

	if (READ_ONCE(pipa_bootdbg_done))
		return HRTIMER_NORESTART;
	pr_emerg("pipa_bootdbg: the watchdog work never ran, CPUs are stuck; stacks by IPI\n");
	for_each_online_cpu(cpu) {
		call_single_data_t *csd = per_cpu_ptr(&pipa_bootdbg_csd, cpu);

		if (cpu == self)
			continue;
		csd->func = pipa_bootdbg_cpu_stack;
		csd->info = NULL;
		smp_call_function_single_async(cpu, csd);
	}
	mdelay(2000);
	pipa_bootdbg_cpu_stack(NULL);
	panic("pipa_bootdbg: CPUs stuck");
	return HRTIMER_NORESTART;
}

static int __init pipa_bootdbg_init(void)
{
	pr_info("pipa_bootdbg: armed, %d s, pipa_hooks=%d\n", pipa_bootdbg, pipa_hooks);
	if (pipa_bootdbg <= 0)
		return 0;
	INIT_DELAYED_WORK(&pipa_bootdbg_work, pipa_bootdbg_fn);
	schedule_delayed_work(&pipa_bootdbg_work, msecs_to_jiffies(pipa_bootdbg * 1000));
	hrtimer_init(&pipa_bootdbg_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	pipa_bootdbg_timer.function = pipa_bootdbg_timer_fn;
	hrtimer_start(&pipa_bootdbg_timer, ktime_set(pipa_bootdbg + 30, 0), HRTIMER_MODE_REL);
	return 0;
}
late_initcall(pipa_bootdbg_init);
