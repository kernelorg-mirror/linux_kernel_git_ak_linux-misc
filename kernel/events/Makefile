# SPDX-License-Identifier: GPL-2.0
ifdef CONFIG_FUNCTION_TRACER
ifneq ($(DISABLE_FTRACE),)
CFLAGS_core.o += $(DISABLE_FTRACE)
else
CFLAGS_REMOVE_core.o = $(CC_FLAGS_FTRACE)
endif
endif

obj-y := core.o ring_buffer.o callchain.o

obj-$(CONFIG_HAVE_HW_BREAKPOINT) += hw_breakpoint.o
obj-$(CONFIG_UPROBES) += uprobes.o

