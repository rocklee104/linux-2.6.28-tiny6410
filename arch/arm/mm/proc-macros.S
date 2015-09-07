/*
 * We need constants.h for:
 *  VMA_VM_MM
 *  VMA_VM_FLAGS
 *  VM_EXEC
 */
#include <asm/asm-offsets.h>
#include <asm/thread_info.h>

/*
 * vma_vm_mm - get mm pointer from vma pointer (vma->vm_mm)
 */
	.macro	vma_vm_mm, rd, rn
	ldr	\rd, [\rn, #VMA_VM_MM]
	.endm

/*
 * vma_vm_flags - get vma->vm_flags
 */
	.macro	vma_vm_flags, rd, rn
	ldr	\rd, [\rn, #VMA_VM_FLAGS]
	.endm

	.macro	tsk_mm, rd, rn
	ldr	\rd, [\rn, #TI_TASK]
	ldr	\rd, [\rd, #TSK_ACTIVE_MM]
	.endm

/*
 * act_mm - get current->active_mm
 */
	.macro	act_mm, rd
	bic	\rd, sp, #8128
	bic	\rd, \rd, #63
	ldr	\rd, [\rd, #TI_TASK]
	ldr	\rd, [\rd, #TSK_ACTIVE_MM]
	.endm

/*
 * mmid - get context id from mm pointer (mm->context.id)
 */
	.macro	mmid, rd, rn
	ldr	\rd, [\rn, #MM_CONTEXT_ID]
	.endm

/*
 * mask_asid - mask the ASID from the context ID
 */
	.macro	asid, rd, rn
	and	\rd, \rn, #255
	.endm

	.macro	crval, clear, mmuset, ucset
#ifdef CONFIG_MMU
	.word	\clear
	.word	\mmuset
#else
	.word	\clear
	.word	\ucset
#endif
	.endm

/*
 * cache_line_size - get the cache line size from the CSIDR register
 * (available on ARMv7+). It assumes that the CSSR register was configured
 * to access the L1 data cache CSIDR.
 */
	.macro	dcache_line_size, reg, tmp
	mrc	p15, 1, \tmp, c0, c0, 0		@ read CSIDR
	and	\tmp, \tmp, #7			@ cache line size encoding
	mov	\reg, #16			@ size offset
	mov	\reg, \reg, lsl \tmp		@ actual cache line size
	.endm


/*
 * Sanity check the PTE configuration for the code below - which makes
 * certain assumptions about how these bits are layed out.
 */
#if L_PTE_SHARED != PTE_EXT_SHARED
#error PTE shared bit mismatch
#endif
#if L_PTE_BUFFERABLE != PTE_BUFFERABLE
#error PTE bufferable bit mismatch
#endif
#if L_PTE_CACHEABLE != PTE_CACHEABLE
#error PTE cacheable bit mismatch
#endif
#if (L_PTE_EXEC+L_PTE_USER+L_PTE_WRITE+L_PTE_DIRTY+L_PTE_YOUNG+\
     L_PTE_FILE+L_PTE_PRESENT) > L_PTE_SHARED
#error Invalid Linux PTE bit settings
#endif

/*
 * The ARMv6 and ARMv7 set_pte_ext translation function.
 *
 * Permission translation:
 *  YUWD  APX AP1 AP0	SVC	User
 *  0xxx   0   0   0	no acc	no acc
 *  100x   1   0   1	r/o	no acc
 *  10x0   1   0   1	r/o	no acc
 *  1011   0   0   1	r/w	no acc
 *  110x   0   1   0	r/w	r/o
 *  11x0   0   1   0	r/w	r/o
 *  1111   0   1   1	r/w	r/w
 */
	.macro	armv6_mt_table pfx
\pfx\()_mt_table:
	#       hw pte buffer/cache 		linux pte buffer/cache
	.long	0x00						@ L_PTE_MT_UNCACHED
	.long	PTE_EXT_TEX(1)					@ L_PTE_MT_BUFFERABLE
	.long	PTE_CACHEABLE					@ L_PTE_MT_WRITETHROUGH
	.long	PTE_CACHEABLE | PTE_BUFFERABLE			@ L_PTE_MT_WRITEBACK
	.long	PTE_BUFFERABLE					@ L_PTE_MT_DEV_SHARED
	.long	0x00						@ unused
	.long	0x00						@ L_PTE_MT_MINICACHE (not present)
	.long	PTE_EXT_TEX(1) | PTE_CACHEABLE | PTE_BUFFERABLE	@ L_PTE_MT_WRITEALLOC
	.long	0x00						@ unused
	.long	PTE_EXT_TEX(1)					@ L_PTE_MT_DEV_WC
	.long	0x00						@ unused
	.long	PTE_CACHEABLE | PTE_BUFFERABLE			@ L_PTE_MT_DEV_CACHED
	.long	PTE_EXT_TEX(2)					@ L_PTE_MT_DEV_NONSHARED
	.long	0x00						@ unused
	.long	0x00						@ unused
	.long	0x00						@ unused
	.endm

	#armv6_set_pte_ext(liux_pte_addr, value, type)
	#在给high vector生成pte时,linux_pte_addr=0xc06befc0,value=0x506bd34b, type=0.
	#pfn的值是0x506bd
	#r1:linux pte项
	#r2:buffer/cache特性
	#r3:hw pte项
	.macro	armv6_set_pte_ext pfx
	#将linux L2 页表项的值写入pte entry, r0-=2048,指向对应的hw pte地址0xc06bd37c0
	str	r1, [r0], #-2048		@ linux version

	#清除页表项中bit[0-9],页表项中的低10位是标志位,r3的值为0x506bd000
	bic	r3, r1, #0x000003fc
	bic	r3, r3, #PTE_TYPE_MASK
	#将页表项类型赋值给页表项
	orr	r3, r3, r2
	#页表项设置成small page,ap设置成特权模式可读写,普通模式禁止访问,r3=0x506bd012
	orr	r3, r3, #PTE_EXT_AP0 | 2

	#取\pfx\()_mt_table地址
	adr	ip, \pfx\()_mt_table
	#r2=8
	and	r2, r1, #L_PTE_MT_MASK
	#在上表中将对应的cache/buffer特性加载到r2中,即PTE_CACHEABLE,也即L_PTE_MT_WRITETHROUGH
	ldr	r2, [ip, r2]

	#测试页表项中是否置位L_PTE_WRITE
	tst	r1, #L_PTE_WRITE
	#如果置位L_PTE_WRITE,测试是否置位L_PTE_DIRTY
	tstne	r1, #L_PTE_DIRTY
	#在没有置位L_PTE_WRITE或置位L_PTE_WRITE然而未置位L_PTE_DIRTY时,置位PTE_EXT_APX
	#r3=0x506bd212
	orreq	r3, r3, #PTE_EXT_APX

	tst	r1, #L_PTE_USER
	#如果r1设置了L_PTE_USER,那么r3需要置位PTE_EXT_AP1,r3=0x506bd232
	orrne	r3, r3, #PTE_EXT_AP1
	#如果r1设置了L_PTE_USER,那么还需要测试r3是否设置了PTE_EXT_APX
	tstne	r3, #PTE_EXT_APX
	#如果r1设置了L_PTE_USER,并且r3置位PTE_EXT_APX,那么清除r3的PTE_EXT_APX及PTE_EXT_AP0
	#最终APX=0, AP[1:0]=10,svc模式可读写,user模式只读,r3=0x506bd022
	bicne	r3, r3, #PTE_EXT_APX | PTE_EXT_AP0

	#测试r1是否置位L_PTE_EXEC
	tst	r1, #L_PTE_EXEC
	#r1没有L_PTE_EXEC,则r3需要excute never,r3=0x506bd022
	orreq	r3, r3, #PTE_EXT_XN

	#给r3添加buffer/cache的特性,0x506bd02a
	orr	r3, r3, r2

	tst	r1, #L_PTE_YOUNG
	#如果r1置位L_PTE_YOUNG,测试L_PTE_PRESENT
	tstne	r1, #L_PTE_PRESENT
	#如果r1没有置位L_PTE_YOUNG,或者置位L_PTE_YOUNG但是没有置位L_PTE_PRESENT
	#说明页表项指向的page不能被访问
	moveq	r3, #0

	#写入hw pte
	str	r3, [r0]
	mcr	p15, 0, r0, c7, c10, 1		@ flush_pte
	.endm
