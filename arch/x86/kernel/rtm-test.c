#include <asm/rtm.h>
#include <linux/init.h>
#include <linux/kernel.h>

int x;

static __init int rtm_test(void)
{
	unsigned status;

	pr_info("simple rtm test\n");
	if ((status = _xbegin()) == _XBEGIN_STARTED) {
		x++;
		_xend();
		pr_info("transaction committed\n");
	} else {
		pr_info("transaction aborted %x\n", status);
	}
	return 0;
}

device_initcall(rtm_test);
