#ifndef _LINUX_IRQ_H
#define _LINUX_IRQ_H

/*
 * Please do not include this file in generic code.  There is currently
 * no requirement for any architecture to implement anything held
 * within this file.
 *
 * Thanks. --rmk
 */

#include <linux/smp.h>

#ifndef CONFIG_S390

#include <linux/linkage.h>
#include <linux/cache.h>
#include <linux/spinlock.h>
#include <linux/cpumask.h>
#include <linux/irqreturn.h>
#include <linux/irqnr.h>
#include <linux/errno.h>

#include <asm/irq.h>
#include <asm/ptrace.h>
#include <asm/irq_regs.h>

struct irq_desc;
typedef	void (*irq_flow_handler_t)(unsigned int irq,
					    struct irq_desc *desc);


/*
 * IRQ line status.
 *
 * Bits 0-7 are reserved for the IRQF_* bits in linux/interrupt.h
 *
 * IRQ types
 */
#define IRQ_TYPE_NONE		0x00000000	/* Default, unspecified type */
#define IRQ_TYPE_EDGE_RISING	0x00000001	/* Edge rising type */
#define IRQ_TYPE_EDGE_FALLING	0x00000002	/* Edge falling type */
#define IRQ_TYPE_EDGE_BOTH (IRQ_TYPE_EDGE_FALLING | IRQ_TYPE_EDGE_RISING)
#define IRQ_TYPE_LEVEL_HIGH	0x00000004	/* Level high type */
#define IRQ_TYPE_LEVEL_LOW	0x00000008	/* Level low type */
#define IRQ_TYPE_SENSE_MASK	0x0000000f	/* Mask of the above */
#define IRQ_TYPE_PROBE		0x00000010	/* Probing in progress */

/* Internal flags */
#define IRQ_INPROGRESS		0x00000100	/* IRQ handler active - do not enter! */
#define IRQ_DISABLED		0x00000200	/* IRQ disabled - do not enter! */
#define IRQ_PENDING		0x00000400	/* IRQ pending - replay on enable */
#define IRQ_REPLAY		0x00000800	/* IRQ has been replayed but not acked yet */
#define IRQ_AUTODETECT		0x00001000	/* IRQ is being autodetected */
#define IRQ_WAITING		0x00002000	/* IRQ not yet seen - for autodetection */
#define IRQ_LEVEL		0x00004000	/* IRQ level triggered */
#define IRQ_MASKED		0x00008000	/* IRQ masked - shouldn't be seen again */
#define IRQ_PER_CPU		0x00010000	/* IRQ is per CPU */
#define IRQ_NOPROBE		0x00020000	/* IRQ is not valid for probing */
#define IRQ_NOREQUEST		0x00040000	/* IRQ cannot be requested */
#define IRQ_NOAUTOEN		0x00080000	/* IRQ will not be enabled on request irq */
#define IRQ_WAKEUP		0x00100000	/* IRQ triggers system wakeup */
#define IRQ_MOVE_PENDING	0x00200000	/* need to re-target IRQ destination */
#define IRQ_NO_BALANCING	0x00400000	/* IRQ is excluded from balancing */
#define IRQ_SPURIOUS_DISABLED	0x00800000	/* IRQ was disabled by the spurious trap */
#define IRQ_MOVE_PCNTXT		0x01000000	/* IRQ migration from process context */
#define IRQ_AFFINITY_SET	0x02000000	/* IRQ affinity was set from userspace*/

#ifdef CONFIG_IRQ_PER_CPU
# define CHECK_IRQ_PER_CPU(var) ((var) & IRQ_PER_CPU)
# define IRQ_NO_BALANCING_MASK	(IRQ_PER_CPU | IRQ_NO_BALANCING)
#else
# define CHECK_IRQ_PER_CPU(var) 0
# define IRQ_NO_BALANCING_MASK	IRQ_NO_BALANCING
#endif

struct proc_dir_entry;
struct msi_desc;

/**
 * struct irq_chip - hardware interrupt chip descriptor
 *
 * @name:		name for /proc/interrupts
 * @startup:		start up the interrupt (defaults to ->enable if NULL)
 * @shutdown:		shut down the interrupt (defaults to ->disable if NULL)
 * @enable:		enable the interrupt (defaults to chip->unmask if NULL)
 * @disable:		disable the interrupt (defaults to chip->mask if NULL)
 * @ack:		start of a new interrupt
 * @mask:		mask an interrupt source
 * @mask_ack:		ack and mask an interrupt source
 * @unmask:		unmask an interrupt source
 * @eoi:		end of interrupt - chip level
 * @end:		end of interrupt - flow level
 * @set_affinity:	set the CPU affinity on SMP machines
 * @retrigger:		resend an IRQ to the CPU
 * @set_type:		set the flow type (IRQ_TYPE_LEVEL/etc.) of an IRQ
 * @set_wake:		enable/disable power-management wake-on of an IRQ
 *
 * @release:		release function solely used by UML
 * @typename:		obsoleted by name, kept as migration helper
 */
/* 中断芯片抽象 */
struct irq_chip {
	/* 在/proc/interrupts显示的名称 */
	const char	*name;
	/*
	 * startup指向一个函数,用于第一次使能参数指定的irq中断号对应的硬件寄存器.如果不提供,
	 * 则系统直接安装默认函数default_startup,它直接调用enable函数指针指向的函数
	 */
	unsigned int	(*startup)(unsigned int irq);
	void		(*shutdown)(unsigned int irq);
	/* 禁止/使能中断,也即控制中断使能禁止寄存器 */
	void		(*enable)(unsigned int irq);
	void		(*disable)(unsigned int irq);

	/* ack用于CPU对中断控制器的确认,通常直接调用mask函数禁止该中断即可 */
	void		(*ack)(unsigned int irq);
	/* mask用于设置屏蔽位,如果硬件没有屏蔽寄存器,那么就直接操作禁止寄存器 */
	void		(*mask)(unsigned int irq);
	void		(*mask_ack)(unsigned int irq);
	/* 用于清除屏蔽位,如果硬件没有屏蔽寄存器,那么就直接操作禁止寄存器 */
	void		(*unmask)(unsigned int irq);
	void		(*eoi)(unsigned int irq);

	void		(*end)(unsigned int irq);
	void		(*set_affinity)(unsigned int irq, cpumask_t dest);
	int		(*retrigger)(unsigned int irq);
	/* 
	 * set_type设置IRQ的电流类型.该方法主要在ARM,PowerPC等使用.通常只有irq_desc在注册action时,
	 * 标记有IRQF_TRIGGER_MASK才会调用该函数重新设置触发类型 
	 */
	int		(*set_type)(unsigned int irq, unsigned int flow_type);
	int		(*set_wake)(unsigned int irq, unsigned int on);

	/* Currently used only by UML, might disappear one day.*/
#ifdef CONFIG_IRQ_RELEASE_METHOD
	void		(*release)(unsigned int irq, void *dev_id);
#endif
	/*
	 * For compatibility, ->typename is copied into ->name.
	 * Will disappear.
	 */
	const char	*typename;
};

/**
 * struct irq_desc - interrupt descriptor
 * @irq:		interrupt number for this descriptor
 * @handle_irq:		highlevel irq-events handler [if NULL, __do_IRQ()]
 * @chip:		low level interrupt hardware access
 * @msi_desc:		MSI descriptor
 * @handler_data:	per-IRQ data for the irq_chip methods
 * @chip_data:		platform-specific per-chip private data for the chip
 *			methods, to allow shared chip implementations
 * @action:		the irq action chain
 * @status:		status information
 * @depth:		disable-depth, for nested irq_disable() calls
 * @wake_depth:		enable depth, for multiple set_irq_wake() callers
 * @irq_count:		stats field to detect stalled irqs
 * @irqs_unhandled:	stats field for spurious unhandled interrupts
 * @last_unhandled:	aging timer for unhandled count
 * @lock:		locking for SMP
 * @affinity:		IRQ affinity on SMP
 * @cpu:		cpu index useful for balancing
 * @pending_mask:	pending rebalanced interrupts
 * @dir:		/proc/irq/ procfs entry
 * @name:		flow handler name for /proc/interrupts output
 */
struct irq_desc {
	/* 中断号 */
	unsigned int		irq;
	/* 它用来实现中断处理器的电流处理,电流处理分为边沿跳变处理和电平处理 */
	irq_flow_handler_t	handle_irq;
	/* chip是对中断处理器芯片功能的封装:中断使能函数,屏蔽函数,确认函数等 */
	struct irq_chip		*chip;
	struct msi_desc		*msi_desc;
	/* 指向ISR处理程序所需的任意数据结构体 */
	void			*handler_data;
	/* 指向与chip操作相关的任意结构体,比如中断处理器控制寄存器的基地址 */
	void			*chip_data;
	/* action提供了ISR需要操作的一个函数链表,中断发生的目的就是要执行这些动作链表中的函数 */
	struct irqaction	*action;	/* IRQ action list */
	/* 中断的状态:使能,禁止,设备共享等 */
	unsigned int		status;		/* IRQ status */

	/*
	 * depth是一个内核中断指示计数器,被初始化为1,当内核调用setup_irq启用中断时减1,
	 * 而调用disable_irq时被加1.通过该值处理中断禁用嵌套 
	 */
	unsigned int		depth;		/* nested irq disables */
	/*
	 * wake_depth是和电源管理中的wake up source相关.通过irq_set_irq_wake接口可以enable
	 * 或者disable一个IRQ中断是否可以把系统从suspend状态唤醒,wake_depth是描述嵌套深度的信息
	 */
	unsigned int		wake_depth;	/* nested wake enables */
	/*
	 * irq_count、last_unhandled和irqs_unhandled用于处理broken IRQ 的处理.所谓broken IRQ就是
	 * 由于种种原因(例如错误firmware),IRQ handler没有定向到指定的IRQ上,当一个IRQ没有被处理的时候,
	 * kernel可以为这个没有被处理的handler启动scan过程,让系统中所有的handler来认领该IRQ.
	 */
	unsigned int		irq_count;	/* For detecting broken IRQs */
	unsigned int		irqs_unhandled;
	unsigned long		last_unhandled;	/* Aging timer for unhandled count */
	/* lock用来在SMP上进行对当前irq_desc的锁定 */
	spinlock_t		lock;
#ifdef CONFIG_SMP
	cpumask_t		affinity;
	unsigned int		cpu;
#endif
#ifdef CONFIG_GENERIC_PENDING_IRQ
	cpumask_t		pending_mask;
#endif
#ifdef CONFIG_PROC_FS
	struct proc_dir_entry	*dir;
#endif
	/*
	 * 安装handle_irq时传递给__set_irq_handler的最后一个参数,可以为NULL.
	 * 通过/proc/interrupts查看它.用来描述电流处理的名称 
	 */
	const char		*name;
} ____cacheline_internodealigned_in_smp;


extern struct irq_desc irq_desc[NR_IRQS];

/* 通过irq获取irq描述符 */
static inline struct irq_desc *irq_to_desc(unsigned int irq)
{
	return (irq < nr_irqs) ? irq_desc + irq : NULL;
}

/*
 * Migration helpers for obsolete names, they will go away:
 */
#define hw_interrupt_type	irq_chip
typedef struct irq_chip		hw_irq_controller;
#define no_irq_type		no_irq_chip
typedef struct irq_desc		irq_desc_t;

/*
 * Pick up the arch-dependent methods:
 */
#include <asm/hw_irq.h>

extern int setup_irq(unsigned int irq, struct irqaction *new);

#ifdef CONFIG_GENERIC_HARDIRQS

#ifdef CONFIG_SMP

#ifdef CONFIG_GENERIC_PENDING_IRQ

void move_native_irq(int irq);
void move_masked_irq(int irq);

#else /* CONFIG_GENERIC_PENDING_IRQ */

static inline void move_irq(int irq)
{
}

static inline void move_native_irq(int irq)
{
}

static inline void move_masked_irq(int irq)
{
}

#endif /* CONFIG_GENERIC_PENDING_IRQ */

#else /* CONFIG_SMP */

#define move_native_irq(x)
#define move_masked_irq(x)

#endif /* CONFIG_SMP */

extern int no_irq_affinity;

static inline int irq_balancing_disabled(unsigned int irq)
{
	struct irq_desc *desc;

	desc = irq_to_desc(irq);
	return desc->status & IRQ_NO_BALANCING_MASK;
}

/* Handle irq action chains: */
extern int handle_IRQ_event(unsigned int irq, struct irqaction *action);

/*
 * Built-in IRQ handlers for various IRQ types,
 * callable via desc->chip->handle_irq()
 */
extern void handle_level_irq(unsigned int irq, struct irq_desc *desc);
extern void handle_fasteoi_irq(unsigned int irq, struct irq_desc *desc);
extern void handle_edge_irq(unsigned int irq, struct irq_desc *desc);
extern void handle_simple_irq(unsigned int irq, struct irq_desc *desc);
extern void handle_percpu_irq(unsigned int irq, struct irq_desc *desc);
extern void handle_bad_irq(unsigned int irq, struct irq_desc *desc);

/*
 * Monolithic do_IRQ implementation.
 */
#ifndef CONFIG_GENERIC_HARDIRQS_NO__DO_IRQ
extern unsigned int __do_IRQ(unsigned int irq);
#endif

/*
 * Architectures call this to let the generic IRQ layer
 * handle an interrupt. If the descriptor is attached to an
 * irqchip-style controller then we call the ->handle_irq() handler,
 * and it calls __do_IRQ() if it's attached to an irqtype-style controller.
 */
static inline void generic_handle_irq_desc(unsigned int irq, struct irq_desc *desc)
{
#ifdef CONFIG_GENERIC_HARDIRQS_NO__DO_IRQ
	desc->handle_irq(irq, desc);
#else
	if (likely(desc->handle_irq))
		desc->handle_irq(irq, desc);
	else
		__do_IRQ(irq);
#endif
}

static inline void generic_handle_irq(unsigned int irq)
{
	generic_handle_irq_desc(irq, irq_to_desc(irq));
}

/* Handling of unhandled and spurious interrupts: */
extern void note_interrupt(unsigned int irq, struct irq_desc *desc,
			   int action_ret);

/* Resending of interrupts :*/
void check_irq_resend(struct irq_desc *desc, unsigned int irq);

/* Enable/disable irq debugging output: */
extern int noirqdebug_setup(char *str);

/* Checks whether the interrupt can be requested by request_irq(): */
extern int can_request_irq(unsigned int irq, unsigned long irqflags);

/* Dummy irq-chip implementations: */
extern struct irq_chip no_irq_chip;
extern struct irq_chip dummy_irq_chip;

extern void
set_irq_chip_and_handler(unsigned int irq, struct irq_chip *chip,
			 irq_flow_handler_t handle);
extern void
set_irq_chip_and_handler_name(unsigned int irq, struct irq_chip *chip,
			      irq_flow_handler_t handle, const char *name);

extern void
__set_irq_handler(unsigned int irq, irq_flow_handler_t handle, int is_chained,
		  const char *name);

/* caller has locked the irq_desc and both params are valid */
static inline void __set_irq_handler_unlocked(int irq,
					      irq_flow_handler_t handler)
{
	struct irq_desc *desc;

	desc = irq_to_desc(irq);
	desc->handle_irq = handler;
}

/*
 * Set a highlevel flow handler for a given IRQ:
 */
static inline void
set_irq_handler(unsigned int irq, irq_flow_handler_t handle)
{
	__set_irq_handler(irq, handle, 0, NULL);
}

/*
 * Set a highlevel chained flow handler for a given IRQ.
 * (a chained handler is automatically enabled and set to
 *  IRQ_NOREQUEST and IRQ_NOPROBE)
 */
static inline void
set_irq_chained_handler(unsigned int irq,
			irq_flow_handler_t handle)
{
	__set_irq_handler(irq, handle, 1, NULL);
}

extern void set_irq_noprobe(unsigned int irq);
extern void set_irq_probe(unsigned int irq);

/* Handle dynamic irq creation and destruction */
extern unsigned int create_irq_nr(unsigned int irq_want);
extern int create_irq(void);
extern void destroy_irq(unsigned int irq);

/* Test to see if a driver has successfully requested an irq */
static inline int irq_has_action(unsigned int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);
	return desc->action != NULL;
}

/* Dynamic irq helper functions */
extern void dynamic_irq_init(unsigned int irq);
extern void dynamic_irq_cleanup(unsigned int irq);

/* Set/get chip/data for an IRQ: */
extern int set_irq_chip(unsigned int irq, struct irq_chip *chip);
extern int set_irq_data(unsigned int irq, void *data);
extern int set_irq_chip_data(unsigned int irq, void *data);
extern int set_irq_type(unsigned int irq, unsigned int type);
extern int set_irq_msi(unsigned int irq, struct msi_desc *entry);

#define get_irq_chip(irq)	(irq_to_desc(irq)->chip)
#define get_irq_chip_data(irq)	(irq_to_desc(irq)->chip_data)
#define get_irq_data(irq)	(irq_to_desc(irq)->handler_data)
#define get_irq_msi(irq)	(irq_to_desc(irq)->msi_desc)

#endif /* CONFIG_GENERIC_HARDIRQS */

#endif /* !CONFIG_S390 */

#endif /* _LINUX_IRQ_H */
