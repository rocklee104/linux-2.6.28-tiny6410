/*
 * mm/pdflush.c - worker threads for writing back filesystem data
 *
 * Copyright (C) 2002, Linus Torvalds.
 *
 * 09Apr2002	Andrew Morton
 *		Initial version
 * 29Feb2004	kaos@sgi.com
 *		Move worker thread creation to kthread to avoid chewing
 *		up stack space with nested calls to kernel_thread.
 */

#include <linux/sched.h>
#include <linux/list.h>
#include <linux/signal.h>
#include <linux/spinlock.h>
#include <linux/gfp.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>		/* Needed by writeback.h	  */
#include <linux/writeback.h>	/* Prototypes pdflush_operation() */
#include <linux/kthread.h>
#include <linux/cpuset.h>
#include <linux/freezer.h>


/*
 * Minimum and maximum number of pdflush instances
 */
#define MIN_PDFLUSH_THREADS	2
#define MAX_PDFLUSH_THREADS	8

static void start_one_pdflush_thread(void);


/*
 * The pdflush threads are worker threads for writing back dirty data.
 * Ideally, we'd like one thread per active disk spindle.  But the disk
 * topology is very hard to divine at this level.   Instead, we take
 * care in various places to prevent more than one pdflush thread from
 * performing writeback against a single filesystem.  pdflush threads
 * have the PF_FLUSHER flag set in current->flags to aid in this.
 */

/*
 * All the pdflush threads.  Protected by pdflush_lock
 */
/*
 * 链表头,用来group together多个空闲的struct pdflush_work *,
 * 睡眠最久的pdflush_work放在链表最末端.
 */
static LIST_HEAD(pdflush_list);
static DEFINE_SPINLOCK(pdflush_lock);

/*
 * The count of currently-running pdflush threads.  Protected
 * by pdflush_lock.
 *
 * Readable by sysctl, but not writable.  Published to userspace at
 * /proc/sys/vm/nr_pdflush_threads.
 */
//记录系统中有多少个pdflush线程
int nr_pdflush_threads = 0;

/*
 * The time at which the pdflush thread pool last went empty
 */
/* pdflush_list上一次变为空的时间 */
static unsigned long last_empty_jifs;

/*
 * The pdflush thread.
 *
 * Thread pool management algorithm:
 * 
 * - The minimum and maximum number of pdflush instances are bound
 *   by MIN_PDFLUSH_THREADS and MAX_PDFLUSH_THREADS.
 * 
 * - If there have been no idle pdflush instances for 1 second, create
 *   a new one.
 * 
 * - If the least-recently-went-to-sleep pdflush thread has been asleep
 *   for more than one second, terminate a thread.
 */

/*
 * A structure for passing work to a pdflush thread.  Also for passing
 * state information between pdflush threads.  Protected by pdflush_lock.
 */
struct pdflush_work {
	//指向所属pdflush线程
	struct task_struct *who;	/* The thread */
	void (*fn)(unsigned long);	/* A callback function */
	unsigned long arg0;		/* An argument to the callback */
	struct list_head list;		/* On pdflush_list, when idle */
	/* 保存上一次睡眠的时间,系统用此值判断并删除多于的pdflush */
	unsigned long when_i_went_to_sleep;
};

//一个pdflush对应了一个pdflush_work
static int __pdflush(struct pdflush_work *my_work)
{
	current->flags |= PF_FLUSHER | PF_SWAPWRITE;
	set_freezable();
	my_work->fn = NULL;
	my_work->who = current;
	INIT_LIST_HEAD(&my_work->list);

	spin_lock_irq(&pdflush_lock);
	nr_pdflush_threads++;
	for ( ; ; ) {
		struct pdflush_work *pdf;

		set_current_state(TASK_INTERRUPTIBLE);
		//将my_work从之前的链表中取出来,头插到pdflush_list链表中
		list_move(&my_work->list, &pdflush_list);
		//记录my_work开始睡眠的时间
		my_work->when_i_went_to_sleep = jiffies;
		spin_unlock_irq(&pdflush_lock);
		//pdflush在pdflush_list上睡眠
		schedule();
		try_to_freeze();
		spin_lock_irq(&pdflush_lock);
		if (!list_empty(&my_work->list)) {
			//有地方唤醒了本次pdflush的进程,但是没有将my_work从链表中取出来,本次唤醒无效
			/*
			 * Someone woke us up, but without removing our control
			 * structure from the global list.  swsusp will do this
			 * in try_to_freeze()->refrigerator().  Handle it.
			 */
			my_work->fn = NULL;
			continue;
		}
		//若pdflush被唤醒,但是my_work没有回调函数,那么本次唤醒也是无效的
		if (my_work->fn == NULL) {
			printk("pdflush: bogus wakeup\n");
			continue;
		}
		spin_unlock_irq(&pdflush_lock);

		//有效唤醒,执行回调函数
		(*my_work->fn)(my_work->arg0);

		/*
		 * Thread creation: For how long have there been zero
		 * available threads?
		 */
		//当前jiffies如果大于last_empty_jifs + 1 * HZ,表示距上次pdflush_list链表空闲的时间超过了1s.
		if (time_after(jiffies, last_empty_jifs + 1 * HZ)) {
			/* unlocked list_empty() test is OK here */
			if (list_empty(&pdflush_list)) {
				//没有空闲的pdflush work,创建一个新的线程
				/* unlocked test is OK here */
				if (nr_pdflush_threads < MAX_PDFLUSH_THREADS)
					start_one_pdflush_thread();
			}
		}

		spin_lock_irq(&pdflush_lock);
		my_work->fn = NULL;

		/*
		 * Thread destruction: For how long has the sleepiest
		 * thread slept?
		 */
		if (list_empty(&pdflush_list))
			continue;
		/* 系统中pdflush的数量没有超过最低界限 */
		if (nr_pdflush_threads <= MIN_PDFLUSH_THREADS)
			continue;
		//取pdflush_list睡眠时间最长的pdflush_work
		pdf = list_entry(pdflush_list.prev, struct pdflush_work, list);
		if (time_after(jiffies, pdf->when_i_went_to_sleep + 1 * HZ)) {
			//判断睡眠时间最长的pdflush_work睡眠已经超过1s,当前pdflush线程就会退出
			/* Limit exit rate */
			pdf->when_i_went_to_sleep = jiffies;
			break;					/* exeunt */
		}
	}
	nr_pdflush_threads--;
	spin_unlock_irq(&pdflush_lock);
	return 0;
}

/*
 * Of course, my_work wants to be just a local in __pdflush().  It is
 * separated out in this manner to hopefully prevent the compiler from
 * performing unfortunate optimisations against the auto variables.  Because
 * these are visible to other tasks and CPUs.  (No problem has actually
 * been observed.  This is just paranoia).
 */
static int pdflush(void *dummy)
{
	struct pdflush_work my_work;
	cpumask_t cpus_allowed;

	/*
	 * pdflush can spend a lot of time doing encryption via dm-crypt.  We
	 * don't want to do that at keventd's priority.
	 */
	set_user_nice(current, 0);

	/*
	 * Some configs put our parent kthread in a limited cpuset,
	 * which kthread() overrides, forcing cpus_allowed == CPU_MASK_ALL.
	 * Our needs are more modest - cut back to our cpusets cpus_allowed.
	 * This is needed as pdflush's are dynamically created and destroyed.
	 * The boottime pdflush's are easily placed w/o these 2 lines.
	 */
	cpuset_cpus_allowed(current, &cpus_allowed);
	set_cpus_allowed_ptr(current, &cpus_allowed);

	return __pdflush(&my_work);
}

/*
 * Attempt to wake up a pdflush thread, and get it to do some work for you.
 * Returns zero if it indeed managed to find a worker thread, and passed your
 * payload to it.
 */
//唤醒pdflush线程
int pdflush_operation(void (*fn)(unsigned long), unsigned long arg0)
{
	unsigned long flags;
	int ret = 0;

	BUG_ON(fn == NULL);	/* Hard to diagnose if it's deferred */

	spin_lock_irqsave(&pdflush_lock, flags);
	if (list_empty(&pdflush_list)) {
		//没有空闲的pdflush
		ret = -1;
	} else {
		struct pdflush_work *pdf;

		//获取最近加入的pdflush_work,将其从空闲链表中移除
		pdf = list_entry(pdflush_list.next, struct pdflush_work, list);
		list_del_init(&pdf->list);
		if (list_empty(&pdflush_list))
			/* 如果pdflush_work取出之后,pdflush_list为空,就需要更新last_empty_jifs */
			last_empty_jifs = jiffies;
		pdf->fn = fn;
		pdf->arg0 = arg0;
		//唤醒这个pdflush_work所属的pdflush线程
		wake_up_process(pdf->who);
	}
	spin_unlock_irqrestore(&pdflush_lock, flags);

	return ret;
}

static void start_one_pdflush_thread(void)
{
	kthread_run(pdflush, NULL, "pdflush");
}

static int __init pdflush_init(void)
{
	int i;

	for (i = 0; i < MIN_PDFLUSH_THREADS; i++)
		start_one_pdflush_thread();
	return 0;
}

module_init(pdflush_init);
