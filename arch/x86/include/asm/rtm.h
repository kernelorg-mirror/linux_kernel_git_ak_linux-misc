#ifndef _RTM_OFFICIAL_H
#define _RTM_OFFICIAL_H 1

#include <linux/compiler.h>
#include <asm/alternative.h>
#include <asm/cpufeature.h>
#include <asm/nops.h>

/*
 * RTM -- restricted transactional memory ISA
 *
 * Official RTM intrinsics interface matching gcc/icc, but works
 * on older gcc compatible compilers and binutils.
 *
 * _xbegin() starts a transaction. When it returns a value different
 * from _XBEGIN_STARTED a non transactional fallback path
 * should be executed.
 *
 * This is a special kernel variant that supports binary patching.
 * When the CPU does not support RTM we always jump to the abort handler.
 * And _xtest() always returns 0.

 * This means these intrinsics can be used without checking cpu_has_rtm
 * first.
 *
 * This is the low level interface mapping directly to the instructions.
 * Usually kernel code will use a higher level abstraction instead (like locks)
 *
 * Note this can be implemented more efficiently on compilers that support
 * "asm goto". But we don't want to require this right now.
 */

#define _XBEGIN_STARTED		(~0u)
#define _XABORT_EXPLICIT	(1 << 0)
#define _XABORT_RETRY		(1 << 1)
#define _XABORT_CONFLICT	(1 << 2)
#define _XABORT_CAPACITY	(1 << 3)
#define _XABORT_DEBUG		(1 << 4)
#define _XABORT_NESTED		(1 << 5)
#define _XABORT_CODE(x)		(((x) >> 24) & 0xff)

#define _XABORT_SOFTWARE	0	/* non architectural */

static __always_inline int _xbegin(void)
{
	int ret;
	alternative_io("mov %[fallback],%[ret] ; " ASM_NOP6,
		       "mov %[started],%[ret] ; "
		       ".byte 0xc7,0xf8 ; .long 0 # XBEGIN 0",
		       X86_FEATURE_RTM,
		       [ret] "=a" (ret),
		       [fallback] "i" (_XABORT_SOFTWARE),
		       [started] "i" (_XBEGIN_STARTED) : "memory");
	return ret;
}

static __always_inline void _xend(void)
{
	/* Not patched because these should be not executed in fallback */
	asm volatile(".byte 0x0f,0x01,0xd5 # XEND" : : : "memory");
}

static __always_inline void _xabort(const unsigned int status)
{
	alternative_input(ASM_NOP3,
			  ".byte 0xc6,0xf8,%P0 # XABORT",
			  X86_FEATURE_RTM,
			  "i" (status) : "memory");
}

static __always_inline int _xtest(void)
{
	unsigned char out;
	alternative_io("xor %0,%0 ; " ASM_NOP4,
		       ".byte 0x0f,0x01,0xd6 ; setnz %0 # XTEST",
		       X86_FEATURE_RTM,
		       "=r" (out),
		       "i" (0) : "memory");
	return out;
}

#endif
