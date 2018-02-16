#ifndef LINUX_DEEPCHAIN_H
#define LINUX_DEEPCHAIN_H 1

#ifdef CONFIG_DEEP_CHAIN
extern void deepchain_return_patch(unsigned long *entries, unsigned num);
extern void deepchain_init(void);
#else
#define deepchain_return_patch(entries, num) do {} while(0)
static inline void deepchain_init(void) {}
#endif

#endif
