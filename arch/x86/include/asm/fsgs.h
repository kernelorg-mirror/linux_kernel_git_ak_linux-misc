#ifndef _ASM_FSGS_H
#define _ASM_FSGS_H 1

#ifndef __ASSEMBLY__

static __always_inline void swapgs(void)
{
	asm volatile("swapgs" ::: "memory");
}

/* Must be protected by X86_FEATURE_FSGSBASE check. */

static __always_inline unsigned long rdgsbase(void)
{
	unsigned long gs;
	asm volatile(".byte 0xf3,0x48,0x0f,0xae,0xc8 # rdgsbaseq %%rax"
			: "=a" (gs)
			:: "memory");
	return gs;
}

static __always_inline unsigned long rdfsbase(void)
{
	unsigned long fs;
	asm volatile(".byte 0xf3,0x48,0x0f,0xae,0xc0 # rdfsbaseq %%rax"
			: "=a" (fs)
			:: "memory");
	return fs;
}

static __always_inline void wrgsbase(unsigned long gs)
{
	asm volatile(".byte 0xf3,0x48,0x0f,0xae,0xd8 # wrgsbaseq %%rax"
			:: "a" (gs)
			: "memory");
}

static __always_inline void wrfsbase(unsigned long fs)
{
	asm volatile(".byte 0xf3,0x48,0x0f,0xae,0xd0 # wrfsbaseq %%rax"
			:: "a" (fs)
			: "memory");
}

#else

/* Handle old assemblers. */
#define RDGSBASE_R15 .byte 0xf3,0x49,0x0f,0xae,0xcf
#define WRGSBASE_RDI .byte 0xf3,0x48,0x0f,0xae,0xdf
#define WRGSBASE_R15 .byte 0xf3,0x49,0x0f,0xae,0xdf

#endif /* __ASSEMBLY__ */

#endif
