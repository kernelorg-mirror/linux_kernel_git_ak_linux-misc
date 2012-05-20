	.macro GET_THREAD_AND_SCHEDULE reg
	GET_THREAD_INFO(\reg)
#ifdef CONFIG_PREEMPT_VOLUNTARY
	testl $_TIF_NEED_RESCHED,TI_flags(\reg)
	jnz   1f
2:
	.section .fixup,"ax"
1:	call user_schedule
	jmp  2b
	.previous
#endif
	.endm
