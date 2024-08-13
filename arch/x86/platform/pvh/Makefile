# SPDX-License-Identifier: GPL-2.0
OBJECT_FILES_NON_STANDARD_head.o := y

ifeq ($(CONFIG_X86_PIE),y)
	ccflags-remove-$(CONFIG_FUNCTION_TRACER) += $(CC_FLAGS_FTRACE)
endif

obj-$(CONFIG_PVH) += enlighten.o
obj-$(CONFIG_PVH) += head.o
