/* Copyright 2018 Intel Corporation
 * Author: Andi Kleen
 *
 * Support to maintain call chain depth counters for automatic
 * return buffer stuffing to avoid speculation attacks on Skylake
 * CPUs. We use ftrace hooks and custom return instrumentation
 * to maintain a dynamic call depth counter.
 *
 * SPDX-License-Identifier: GPL-2.0
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/init.h>
#include <linux/debugfs.h>
#include <linux/deepchain.h>
#include <asm/sections.h>
#include <asm/text-patching.h>
#include <asm/nospec-branch.h>

asm("nop5_insn:\n"
    "   " _ASM_MK_NOP(P6_NOP5) "\n"
    ASM_UNREACHABLE
    ".size nop5_insn,.-nop5_insn\n");

extern char nop5_insn[];

extern void __visible __return__(void);
extern void __visible calldepth_hook(void);

static void deepchain_patch(unsigned long *entries, unsigned num, void *func,
			    u8 opc)
{
	unsigned i;

	pr_debug("deep-chain: patching %u returns\n", num);
	for (i = 0; i < num; i++) {
		char *insnp = (char *)entries[i];
		char call[5];
		int offset;

		if (memcmp(insnp, nop5_insn, 5)) {
			pr_warn("Unexpected return entry at %p: %02x %02x %02x %02x %02x\n",
				insnp,
				insnp[0],
				insnp[1],
				insnp[2],
				insnp[3],
				insnp[4]);
			continue;
		}
		call[0] = opc;
		offset = (unsigned long)func - (unsigned long)insnp - 5;
		memcpy(call + 1, &offset, 4);
		text_poke_early_bp(insnp, call, 5, insnp + 5);
	}
}

void deepchain_return_patch(unsigned long *entries, unsigned num)
{
	/*
	 * Use CALL. Could in theory be 0xe9 (JMP) for return
	 * to save one RET, but that causes very mysterious
	 * random boot failures with specific kernel configs.
	 */
	deepchain_patch(entries, num, __return__, 0xe8);
}

void deepchain_entry_patch(unsigned long *entries, unsigned num)
{
	deepchain_patch(entries, num, calldepth_hook, 0xe8);
}

#ifdef CONFIG_DEBUG_FS
static int call_depth_show(struct seq_file *m, void *private)
{
	int cpu;

	for_each_possible_cpu (cpu)
		seq_printf(m, "%d: %d\n", cpu, per_cpu(__call_depth__, cpu));
	return 0;
}

static int call_depth_open(struct inode *inode, struct file *file)
{
	return single_open(file, call_depth_show, inode->i_private);
}

static const struct file_operations call_depth_fops = {
	.open = call_depth_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static __init int deepchain_debugfs_init(void)
{
	debugfs_create_file_unsafe("call_depth", 0644, arch_debugfs_dir, NULL,
				 &call_depth_fops);
	return 0;
}

fs_initcall(deepchain_debugfs_init);
#endif

__init void deepchain_init(void)
{
	deepchain_return_patch(__start_return_loc, __end_return_loc - __start_return_loc);
	deepchain_entry_patch(__start_entry_loc, __end_entry_loc - __start_entry_loc);
}
