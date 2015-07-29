
#if __LINUX_ARM_ARCH__ >= 6 && defined(CONFIG_CPU_32v6K)
	.macro	bitop, instr
	mov	r2, #1
	and	r3, r0, #7		@ Get bit offset
	add	r1, r1, r0, lsr #3	@ Get byte offset
	mov	r3, r2, lsl r3
1:	ldrexb	r2, [r1]
	\instr	r2, r2, r3
	strexb	r0, r2, [r1]
	cmp	r0, #0
	bne	1b
	mov	pc, lr
	.endm

	.macro	testop, instr, store
	#r3保存要操作的这个bit未以byte对齐的部分
	and	r3, r0, #7		@ Get bit offset
	mov	r2, #1
	#将目标bitmap偏移要操作的这个bit的字节数
	add	r1, r1, r0, lsr #3	@ Get byte offset
	mov	r3, r2, lsl r3		@ create mask
	#原子地读取bitmap+偏移操作的这个bit的字节数的一个byte
1:	ldrexb	r2, [r1]
	#r0中保存目标bitmap中需要操作的bit的状态
	ands	r0, r2, r3		@ save old value of bit
	/*
	 * 对于_test_and_set_bit_le来说,instr就是orreq,如果bitmap中要操作的bit为0,
	 * 那么就设置这个bit为1.
	 */
	\instr	r2, r2, r3			@ toggle bit
	#将更改后的结果写入bitmap对应的位置,ip保存返回结果
	strexb	ip, r2, [r1]
	#判断写入操作是否成功
	cmp	ip, #0
	bne	1b
	#r0保存之前bitmap中需要操作bit的状态
	cmp	r0, #0
	movne	r0, #1
2:	mov	pc, lr
	.endm
#endif
