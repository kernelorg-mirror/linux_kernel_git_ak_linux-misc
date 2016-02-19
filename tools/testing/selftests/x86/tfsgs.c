/* Test kernel RD/WR FS/GS BASE support
 * Run tfsgs 0 to run forever, otherwise iterations (default 10000)
 * For stress testing run many in parallel to test context switching too
 *
 * This program destroys TLS, which means most of normal glibc
 * doesn't work. So it uses its own libc replacement.
 *
 * It also breaks some versions of gdb
 * (workaround available in https://sourceware.org/bugzilla/show_bug.cgi?id=19684)
 */
#include <stdlib.h>
#include <assert.h>
#include <asm/prctl.h>
#include <asm/unistd.h>
#include <cpuid.h>
#include <sys/auxv.h>
#include <elf.h>

#ifndef __always_inline
#define __always_inline inline __attribute__((always_inline))
#endif

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

/* Custom assert because we can't access errno with changed fs */

int my_strlen(char *s)
{
	int len = 0;
	while (*s++)
		len++;
	return len;
}

int arch_prctl(int cmd, unsigned long arg)
{
	int ret;
	asm volatile("syscall" : "=a" (ret)
			: "0" (__NR_arch_prctl), "D" (cmd), "S" (arg)
			: "memory", "rcx", "r11");
	return ret;
}

__attribute__((noinline)) void my_assert(int flag, char *msg)
{
	if (!flag) {
		int ret;
		asm volatile("syscall"
				: "=a" (ret)
				: "0" (__NR_write),
				"D" (2), "S" (msg),
				"d" (my_strlen(msg))
				: "memory", "rcx", "r11");
		*(int *)0 = 0;
	}
}

long iter = 10000;

#ifndef bit_FSGSBASE
#define bit_FSGSBASE 1
#endif

/* Will be eventually in asm/hwcap.h */
#define HWCAP2_FSGSBASE        (1 << 0)

unsigned long nfs, ngs, x;

int main(int ac, char **av)
{
	long i;
	unsigned a, b, c, d;

	if (__get_cpuid_max(0, NULL) < 7)
		exit(0);
	__cpuid_count(7, 0, a, b, c, d);
	if (!(b & bit_FSGSBASE))
		exit(0);

	/* Kernel support? */
        if (!(getauxval(AT_HWCAP2) & HWCAP2_FSGSBASE))
		exit(0);

	if (av[1])
		iter = strtoul(av[1], NULL, 0);

	srandom(1);
	unsigned long count = random();
	unsigned long orig_fs = rdfsbase();
	for (i = 0; i < iter || iter == 0; i++) {
		unsigned long x = count++;
		x = ((long)(x << 16)) >> 16; /* sign extend 48->64 */

		wrgsbase(x);
		wrfsbase(x);

		int i;
		for (i = 0; i < 1000; i++)
			asm volatile("pause" ::: "memory");

		ngs = rdgsbase();
		nfs = rdfsbase();

		my_assert(ngs == x, "gs check 1 failed\n");
		my_assert(nfs == x, "fs check 1 failed\n");

		unsigned long n;
		const unsigned long MASK = 0x7fffffffffff;
		arch_prctl(ARCH_SET_FS, (x + 1) & MASK);
		arch_prctl(ARCH_SET_GS, (x - 1) & MASK);
		n = rdfsbase();
		my_assert(n == ((x + 1) & MASK), "fs check 2 failed\n");

		for (i = 0; i < 1000; i++)
			asm volatile("pause" ::: "memory");

		n = rdgsbase();
		my_assert(n == ((x - 1) & MASK), "gs check 2 failed\n");
	}
	wrfsbase(orig_fs);
}
