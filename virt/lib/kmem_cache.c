// SPDX-License-Identifier: GPL-2.0-only

#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <linux/delay.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("KVM PAGE_SIZE cache module");

unsigned int max_num;
EXPORT_SYMBOL_GPL(max_num);
module_param(max_num, uint, 0444);

unsigned int missed_num;
EXPORT_SYMBOL_GPL(missed_num);
module_param(missed_num, uint, 0644);

unsigned int sleep_ms = 10;
EXPORT_SYMBOL_GPL(sleep_ms);
module_param(sleep_ms, uint, 0444);

unsigned int max_wait_ms;
EXPORT_SYMBOL_GPL(max_wait_ms);
module_param(max_wait_ms, uint, 0444);

static struct task_struct *topup_thread;
static DECLARE_WAIT_QUEUE_HEAD(topup_wait);
static DEFINE_MUTEX(thread_mutex);
static unsigned int usr_topup_sleep_ms;
static unsigned int pages_num;
static unsigned int alloc_cpu;
struct kvm_pages {
	/* protect nobjs inc/dec */
	raw_spinlock_t lock;
	unsigned long flags;
	gfp_t gfp;
	unsigned int capacity;
	unsigned int nobjs;
	void **objects;
};

static DEFINE_PER_CPU(struct kvm_pages, kvm_pages);
static DEFINE_PER_CPU(struct kvm_pages, kvm_zero_pages);

static inline void lock_pages(struct kvm_pages *pages)
{
	u64 time;

	preempt_disable();
	time = get_jiffies_64();
	raw_spin_lock_irqsave(&pages->lock, pages->flags);
	time = jiffies_delta_to_msecs(get_jiffies_64() - time);
	if (time > max_wait_ms)
		max_wait_ms = time;
}

static inline void unlock_pages(struct kvm_pages *pages)
{
	lockdep_assert_held(&pages->lock);

	raw_spin_unlock_irqrestore(&pages->lock, pages->flags);
	preempt_enable();
}

static inline struct kvm_pages *get_cpu_pages(gfp_t gfp_flags, int cpu)
{
	if (gfp_flags == GFP_KERNEL_ACCOUNT)
		return per_cpu_ptr(&kvm_pages, cpu);

	if (gfp_flags == (GFP_KERNEL_ACCOUNT | __GFP_ZERO))
		return per_cpu_ptr(&kvm_zero_pages, cpu);

	return NULL;
}

static inline void init_pages(struct kvm_pages *pages, gfp_t gfp_flags)
{
	if (!pages)
		return;

	pages->gfp = gfp_flags;
	pages->capacity = 0;
	pages->nobjs = 0;
	pages->objects = NULL;

	pages->flags = 0;
	raw_spin_lock_init(&pages->lock);
}

static void init_cpu_pages(void)
{
	unsigned int cpu;

	for_each_possible_cpu(cpu) {
		init_pages(per_cpu_ptr(&kvm_pages, cpu), GFP_KERNEL_ACCOUNT);
		init_pages(per_cpu_ptr(&kvm_zero_pages, cpu),
			   GFP_KERNEL_ACCOUNT | __GFP_ZERO);
	}
}

static inline void *get_one_free_page(gfp_t gfp_flags)
{
	if (preemptible())
		gfp_flags &= ~GFP_ATOMIC;
	else
		gfp_flags |= GFP_ATOMIC;

	return (void *)__get_free_page(gfp_flags);
}

static int __topup_pages(struct kvm_pages *pages, int capacity, int min)
{
	void *obj;

	if (pages->nobjs >= min)
		return 0;

	if (unlikely(!pages->objects)) {
		if (WARN_ON_ONCE(!capacity))
			return -EIO;

		pages->objects =
			kvmalloc_array(capacity, sizeof(void *), pages->gfp);
		if (!pages->objects)
			return -ENOMEM;

		pages->capacity = capacity;
	}

	if (WARN_ON_ONCE(pages->capacity != capacity))
		return -EIO;

	while (pages->nobjs < pages->capacity) {
		obj = get_one_free_page(pages->gfp);
		if (obj) {
			lock_pages(pages);
			pages->objects[pages->nobjs++] = obj;
			unlock_pages(pages);
		}
	}

	return 0;
}

static inline void _topup_pages(struct kvm_pages *pages)
{
	if (pages && pages->nobjs < max_num)
		__topup_pages(pages, max_num, max_num);
}

static int topup_pages(void)
{
	unsigned int cpu;

	for_each_possible_cpu(cpu) {
		_topup_pages(get_cpu_pages(GFP_KERNEL_ACCOUNT, cpu));
		_topup_pages(get_cpu_pages(GFP_KERNEL_ACCOUNT | __GFP_ZERO, cpu));
	}

	return 0;
}

static void _free_pages(struct kvm_pages *pages)
{
	while (pages->nobjs)
		free_page((unsigned long)pages->objects[--pages->nobjs]);

	kvfree(pages->objects);

	pages->objects = NULL;
	pages->capacity = 0;
}

static int topup_kvm_pages_thread(void *nothing)
{
	unsigned int new_sleep_ms, topup_cost;
	unsigned int pages;

	set_freezable();

	while (!kthread_should_stop()) {
		mutex_lock(&thread_mutex);

		if (usr_topup_sleep_ms) {
			pages = READ_ONCE(pages_num);

			if (pages) {
				new_sleep_ms =
					min(10, 1 + (sleep_ms / 2));
			} else {
				new_sleep_ms =
					min(usr_topup_sleep_ms, sleep_ms + 10);
			}
		}

		jiffies = get_jiffies_64();
		topup_pages();
		topup_cost = jiffies_delta_to_msecs(get_jiffies_64() - jiffies);

		WRITE_ONCE(pages_num, 0);

		if (!usr_topup_sleep_ms || topup_cost > new_sleep_ms) {
			mutex_unlock(&thread_mutex);
			continue;
		}

		mutex_unlock(&thread_mutex);

		try_to_freeze();

		sleep_ms = new_sleep_ms;
		wait_event_interruptible_timeout(topup_wait, false,
						 msecs_to_jiffies(sleep_ms - topup_cost));
	}

	return 0;
}

static int __init kmem_cache_init(void)
{
	if (!max_num && !sleep_ms)
		return 0;

	if (!max_num)
		max_num = 32000;

	init_cpu_pages();

	topup_pages();

	usr_topup_sleep_ms = sleep_ms;

	topup_thread =
		kthread_run(topup_kvm_pages_thread, NULL, "topup_pages");
	if (IS_ERR(topup_thread)) {
		pr_err("kvm: creating topup_pages kthread failed\n");
		return PTR_ERR(topup_thread);
	}

	return 0;
}
module_init(kmem_cache_init);

static void __exit kmem_cache_exit(void)
{
	unsigned int cpu;

	if (!max_num && !sleep_ms)
		return;

	if (topup_thread)
		kthread_stop(topup_thread);

	for_each_possible_cpu(cpu) {
		_free_pages(get_cpu_pages(GFP_KERNEL_ACCOUNT, cpu));
		_free_pages(get_cpu_pages(GFP_KERNEL_ACCOUNT | __GFP_ZERO, cpu));
	}
}
module_exit(kmem_cache_exit);

static inline void *alloc_one_page(struct kvm_pages *pages)
{
	void *p;

	if (!pages->nobjs) {
		WRITE_ONCE(missed_num, missed_num + 1);
		p = get_one_free_page(pages->gfp);
	} else {
		lock_pages(pages);
		p = pages->objects[--pages->nobjs];
		unlock_pages(pages);
	}

	return p;
}

void *kmem_cache_get_free_page(gfp_t gfp_flags)
{
	unsigned int cpu = (alloc_cpu++) % num_possible_cpus();
	struct kvm_pages *kvm_pages = NULL;
	void *page = NULL;

	if (!max_num && !sleep_ms) {
		WRITE_ONCE(missed_num, missed_num + 1);
		return get_one_free_page(gfp_flags);
	}

	if (!try_module_get(THIS_MODULE))
		return page;

	kvm_pages = get_cpu_pages(gfp_flags, cpu);

	if (kvm_pages && kvm_pages->nobjs) {
		page = alloc_one_page(kvm_pages);
	} else {
		WRITE_ONCE(missed_num, missed_num + 1);
		page = get_one_free_page(gfp_flags);
	}

	WRITE_ONCE(pages_num, pages_num + 1);

	module_put(THIS_MODULE);

	return page;
}
EXPORT_SYMBOL_GPL(kmem_cache_get_free_page);
