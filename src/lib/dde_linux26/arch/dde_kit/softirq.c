/*
 * \brief   DDE Linux 2.6 softirqs
 * \author  Björn Döbel
 * \author  Christian Helmuth
 * \date    2008-11-11
 */

#include <linux/interrupt.h>

#include "local.h"

/* There are at most 32 softirqs in Linux, but only 6 are really used. */
#define NUM_SOFTIRQS 6

DECLARE_INITVAR(dde_linux26_softirq);

/* softirq wakeup semaphore */
static struct dde_kit_sem *softirq_sem;

/* struct tasklet_head is not defined in a header in Linux 2.6 */
struct tasklet_head
{
	struct tasklet_struct *list;
	struct dde_kit_lock   *lock;  /* list lock */
};

/* What to do if a softirq occurs. */
static struct softirq_action softirq_vec[32];

/* tasklet queues for each softirq thread */
struct tasklet_head tasklet_vec;
struct tasklet_head tasklet_hi_vec;

void open_softirq(int nr, void (*action)(struct softirq_action*), void *data)
{
	softirq_vec[nr].action = action;
	softirq_vec[nr].data   = data;
}

static void fastcall raise_softirq_irqoff_cpu(unsigned int nr, unsigned int cpu)
{
	CHECK_INITVAR(dde_linux26_softirq);

	/* mark softirq scheduled */
	__raise_softirq_irqoff(nr);
	/* wake softirq thread */
	dde_kit_sem_up(softirq_sem);
}

void fastcall raise_softirq_irqoff(unsigned int nr)
{
	raise_softirq_irqoff_cpu(nr, 0);
}

void fastcall raise_softirq(unsigned int nr)
{
	unsigned long flags;

	local_irq_save(flags);
	raise_softirq_irqoff(nr);
	local_irq_restore(flags);
}

/** Initialize tasklet.
 *
 * Unimplemented.
 */
void tasklet_init(struct tasklet_struct *t,
                  void (*func)(unsigned long), unsigned long data)
{
	t->next = NULL;
	t->state = 0;
	atomic_set(&t->count, 0);
	t->func = func;
	t->data = data;
}

void tasklet_kill(struct tasklet_struct *t)
{
	if (in_interrupt())
		printk("Attempt to kill tasklet from interrupt\n");

	while (test_and_set_bit(TASKLET_STATE_SCHED, &t->state)) {
		do
			yield();
		while (test_bit(TASKLET_STATE_SCHED, &t->state));
	}
	tasklet_unlock_wait(t);
	clear_bit(TASKLET_STATE_SCHED, &t->state);
}

EXPORT_SYMBOL(tasklet_kill);

/* enqueue tasklet */
static void __tasklet_enqueue(struct tasklet_struct *t, 
                              struct tasklet_head *listhead)
{
	dde_kit_lock_lock(listhead->lock);
	t->next = listhead->list;
	listhead->list = t;
	dde_kit_lock_unlock(listhead->lock);
}

void fastcall __tasklet_schedule(struct tasklet_struct *t)
{
	unsigned long flags;

	CHECK_INITVAR(dde_linux26_softirq);

	local_irq_save(flags);

	__tasklet_enqueue(t, &tasklet_vec);
	/* raise softirq */
	raise_softirq_irqoff_cpu(TASKLET_SOFTIRQ, 0);

	local_irq_restore(flags);
}

void fastcall __tasklet_hi_schedule(struct tasklet_struct *t)
{
	unsigned long flags;

	CHECK_INITVAR(dde_linux26_softirq);

	local_irq_save(flags);
	__tasklet_enqueue(t, &tasklet_hi_vec);
	raise_softirq_irqoff_cpu(HI_SOFTIRQ, 0);
	local_irq_restore(flags);
}

/* Execute tasklets */
static void tasklet_action(struct softirq_action *a)
{
	struct tasklet_struct *list;

	dde_kit_lock_lock(tasklet_vec.lock);
	list = tasklet_vec.list;
	tasklet_vec.list = NULL;
	dde_kit_lock_unlock(tasklet_vec.lock);

	while (list) {
		struct tasklet_struct *t = list;

		list = list->next;

		if (tasklet_trylock(t)) {
			if (!atomic_read(&t->count)) {
				if (!test_and_clear_bit(TASKLET_STATE_SCHED, &t->state))
					BUG();
				t->func(t->data);
				tasklet_unlock(t);
				continue;
			}
			tasklet_unlock(t);
		}

		dde_kit_lock_lock(tasklet_vec.lock);
		t->next = tasklet_vec.list;
		tasklet_vec.list = t;
		raise_softirq_irqoff_cpu(TASKLET_SOFTIRQ, 0);
		dde_kit_lock_unlock(tasklet_vec.lock);
	}
}


static void tasklet_hi_action(struct softirq_action *a)
{
	struct tasklet_struct *list;

	dde_kit_lock_lock(tasklet_hi_vec.lock);
	list = tasklet_hi_vec.list;
	tasklet_hi_vec.list = NULL;
	dde_kit_lock_unlock(tasklet_hi_vec.lock);

	while (list) {	
		struct tasklet_struct *t = list;

		list = list->next;

		if (tasklet_trylock(t)) {
			if (!atomic_read(&t->count)) {
				if (!test_and_clear_bit(TASKLET_STATE_SCHED, &t->state))
					BUG();
				t->func(t->data);
				tasklet_unlock(t);
				continue;
			}
			tasklet_unlock(t);
		}

		dde_kit_lock_lock(tasklet_hi_vec.lock);
		t->next = tasklet_hi_vec.list;
		tasklet_hi_vec.list = t;
		raise_softirq_irqoff_cpu(HI_SOFTIRQ, 0);
		dde_kit_lock_unlock(tasklet_hi_vec.lock);
	}
}


#define MAX_SOFTIRQ_RETRIES	10

/** Run softirq handlers 
 */
static void __do_softirq(void)
{
	int retries = MAX_SOFTIRQ_RETRIES;
	do {
		struct softirq_action *h = softirq_vec;
		unsigned long pending = local_softirq_pending();

		/* reset softirq count */
		set_softirq_pending(0);

		/* While we have a softirq pending... */
		while (pending) {
			/* need to execute current softirq? */
			if (pending & 1)
				h->action(h);
			/* try next softirq */
			h++;
			/* remove pending flag for last softirq */
			pending >>= 1;
		}

	/* Somebody might have scheduled another softirq in between
	 * (e.g., an IRQ thread or another tasklet). */
	} while (local_softirq_pending() && --retries);

}


asmlinkage void do_softirq(void)
{
	unsigned long flags;

	local_irq_save(flags);
	if (local_softirq_pending())
		__do_softirq();
	local_irq_restore(flags);
}


void dde_linux26_raise_softirq()
{
	dde_kit_sem_up(softirq_sem);
}


/** Softirq thread function.
 *
 * Once started, a softirq thread waits for tasklets to be scheduled
 * and executes them.
 *
 * \param arg	# of this softirq thread so that it grabs the correct lock
 *              if multiple softirq threads are running.
 */
void dde_linux26_softirq_thread(void *arg)
{
	printk("Softirq daemon starting\n");
	dde_linux26_process_add_worker("unused");

	/* This thread will always be in a softirq, so set the 
	 * corresponding flag right now.
	 */
	preempt_count() |= SOFTIRQ_MASK;

	while(1) {
		dde_kit_sem_down(softirq_sem);
		do_softirq();
	}
}

/** Initialize softirq subsystem.
 *
 * Start NUM_SOFTIRQ_THREADS threads executing the \ref dde_linux26_softirq_thread
 * function.
 */
void dde_linux26_softirq_init(void)
{
	static char name[20];

	softirq_sem = dde_kit_sem_init(0);

	set_softirq_pending(0);

	dde_kit_lock_init(&tasklet_vec.lock);
	dde_kit_lock_init(&tasklet_hi_vec.lock);

	snprintf(name, 20, ".softirqd");
	dde_kit_thread_create( dde_linux26_softirq_thread, NULL, name);

	open_softirq(TASKLET_SOFTIRQ, tasklet_action, NULL);
	open_softirq(HI_SOFTIRQ, tasklet_hi_action, NULL);

	INITIALIZE_INITVAR(dde_linux26_softirq);
}
