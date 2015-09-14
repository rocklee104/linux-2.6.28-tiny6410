/*
 * Fast batching percpu counters.
 */

#include <linux/percpu_counter.h>
#include <linux/notifier.h>
#include <linux/mutex.h>
#include <linux/init.h>
#include <linux/cpu.h>
#include <linux/module.h>

void percpu_counter_set(struct percpu_counter *fbc, s64 amount)
{
	int cpu;

	spin_lock(&fbc->lock);
	for_each_possible_cpu(cpu) {
		s32 *pcount = per_cpu_ptr(fbc->counters, cpu);
		*pcount = 0;
	}
	fbc->count = amount;
	spin_unlock(&fbc->lock);
}
EXPORT_SYMBOL(percpu_counter_set);

void __percpu_counter_add(struct percpu_counter *fbc, s64 amount, s32 batch)
{
	s64 count;
	s32 *pcount;
	int cpu = get_cpu();

	pcount = per_cpu_ptr(fbc->counters, cpu);
	count = *pcount + amount;
	if (count >= batch || count <= -batch) {
		spin_lock(&fbc->lock);
		fbc->count += count;
		*pcount = 0;
		spin_unlock(&fbc->lock);
	} else {
		*pcount = count;
	}
	put_cpu();
}
EXPORT_SYMBOL(__percpu_counter_add);

/*
 * Add up all the per-cpu counts, return the result.  This is a more accurate
 * but much slower version of percpu_counter_read_positive()
 */
s64 __percpu_counter_sum(struct percpu_counter *fbc)
{
	s64 ret;
	int cpu;

	spin_lock(&fbc->lock);
	ret = fbc->count;
	for_each_online_cpu(cpu) {
		s32 *pcount = per_cpu_ptr(fbc->counters, cpu);
		ret += *pcount;
	}
	spin_unlock(&fbc->lock);
	return ret;
}
EXPORT_SYMBOL(__percpu_counter_sum);

static struct lock_class_key percpu_counter_irqsafe;

int percpu_counter_init(struct percpu_counter *fbc, s64 amount)
{
	spin_lock_init(&fbc->lock);
	fbc->count = amount;
	fbc->counters = alloc_percpu(s32);
	if (!fbc->counters)
		return -ENOMEM;
	return 0;
}
EXPORT_SYMBOL(percpu_counter_init);

int percpu_counter_init_irq(struct percpu_counter *fbc, s64 amount)
{
	int err;

	err = percpu_counter_init(fbc, amount);
	if (!err)
		lockdep_set_class(&fbc->lock, &percpu_counter_irqsafe);
	return err;
}

void percpu_counter_destroy(struct percpu_counter *fbc)
{
	if (!fbc->counters)
		return;

	free_percpu(fbc->counters);
	fbc->counters = NULL;
}
EXPORT_SYMBOL(percpu_counter_destroy);
