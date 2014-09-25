#include <linux/module.h>
#include <linux/preempt.h>
#include <asm/msr.h>
#define CREATE_TRACE_POINTS
#include <trace/events/msr.h>

struct msr *msrs_alloc(void)
{
	struct msr *msrs = NULL;

	msrs = alloc_percpu(struct msr);
	if (!msrs) {
		pr_warn("%s: error allocating msrs\n", __func__);
		return NULL;
	}

	return msrs;
}
EXPORT_SYMBOL(msrs_alloc);

void msrs_free(struct msr *msrs)
{
	free_percpu(msrs);
}
EXPORT_SYMBOL(msrs_free);

/**
 * Read an MSR with error handling
 *
 * @msr: MSR to read
 * @m: value to read into
 *
 * It returns read data only on success, otherwise it doesn't change the output
 * argument @m.
 *
 */
int msr_read(u32 msr, struct msr *m)
{
	int err;
	u64 val;

	err = rdmsrl_safe(msr, &val);
	if (!err)
		m->q = val;

	return err;
}

/**
 * Write an MSR with error handling
 *
 * @msr: MSR to write
 * @m: value to write
 */
int msr_write(u32 msr, struct msr *m)
{
	return wrmsrl_safe(msr, m->q);
}

static inline int __flip_bit(u32 msr, u8 bit, bool set)
{
	struct msr m, m1;
	int err = -EINVAL;

	if (bit > 63)
		return err;

	err = msr_read(msr, &m);
	if (err)
		return err;

	m1 = m;
	if (set)
		m1.q |=  BIT_64(bit);
	else
		m1.q &= ~BIT_64(bit);

	if (m1.q == m.q)
		return 0;

	err = msr_write(msr, &m1);
	if (err)
		return err;

	return 1;
}

/**
 * Set @bit in a MSR @msr.
 *
 * Retval:
 * < 0: An error was encountered.
 * = 0: Bit was already set.
 * > 0: Hardware accepted the MSR write.
 */
int msr_set_bit(u32 msr, u8 bit)
{
	return __flip_bit(msr, bit, true);
}

/**
 * Clear @bit in a MSR @msr.
 *
 * Retval:
 * < 0: An error was encountered.
 * = 0: Bit was already cleared.
 * > 0: Hardware accepted the MSR write.
 */
int msr_clear_bit(u32 msr, u8 bit)
{
	return __flip_bit(msr, bit, false);
}

inline unsigned long long native_read_msr(unsigned int msr)
{
	unsigned long long lval;
	DECLARE_ARGS(val, low, high);

	asm volatile("rdmsr" : EAX_EDX_RET(val, low, high) : "c" (msr));
	lval = EAX_EDX_VAL(val, low, high);
	trace_read_msr(msr, lval, 0);
	return lval;
}
EXPORT_SYMBOL(native_read_msr);

inline unsigned long long native_read_msr_safe(unsigned int msr,
						      int *err)
{
	unsigned long long lval;
	DECLARE_ARGS(val, low, high);

	asm volatile("2: rdmsr ; xor %[err],%[err]\n"
		     "1:\n\t"
		     ".section .fixup,\"ax\"\n\t"
		     "3:  mov %[fault],%[err] ; jmp 1b\n\t"
		     ".previous\n\t"
		     _ASM_EXTABLE(2b, 3b)
		     : [err] "=r" (*err), EAX_EDX_RET(val, low, high)
		     : "c" (msr), [fault] "i" (-EIO));
	lval = EAX_EDX_VAL(val, low, high);
	trace_read_msr(msr, lval, *err);
	return lval;
}
EXPORT_SYMBOL(native_read_msr_safe);

inline void native_write_msr(unsigned int msr,
				    unsigned low, unsigned high)
{
	asm volatile("wrmsr" : : "c" (msr), "a"(low), "d" (high) : "memory");
	trace_write_msr(msr, ((u64)high << 32 | low), 0);
}
EXPORT_SYMBOL(native_write_msr);

/* Can be uninlined because referenced by paravirt */
notrace inline int native_write_msr_safe(unsigned int msr,
					unsigned low, unsigned high)
{
	int err;

	asm volatile("2: wrmsr ; xor %[err],%[err]\n"
		     "1:\n\t"
		     ".section .fixup,\"ax\"\n\t"
		     "3:  mov %[fault],%[err] ; jmp 1b\n\t"
		     ".previous\n\t"
		     _ASM_EXTABLE(2b, 3b)
		     : [err] "=a" (err)
		     : "c" (msr), "0" (low), "d" (high),
		       [fault] "i" (-EIO)
		     : "memory");
	trace_write_msr(msr, ((u64)high << 32 | low), err);
	return err;
}
EXPORT_SYMBOL(native_write_msr_safe);
