# SPDX-License-Identifier: GPL-2.0
CC		= $(CROSS_COMPILE)gcc
BUILD_OUTPUT    := $(CURDIR)
PREFIX		:= /usr
DESTDIR		:=

ifeq ("$(origin O)", "command line")
	BUILD_OUTPUT := $(O)
endif

x86_energy_perf_policy : x86_energy_perf_policy.c
override CFLAGS +=	-O2 -Wall -I../../../include
override CFLAGS +=	-DMSRHEADER='"../../../../arch/x86/include/asm/msr-index.h"'
override CFLAGS +=	

%: %.c
	@mkdir -p $(BUILD_OUTPUT)
	$(CC) $(CFLAGS) $< -o $(BUILD_OUTPUT)/$@ $(LDFLAGS)

.PHONY : clean
clean :
	@rm -f $(BUILD_OUTPUT)/x86_energy_perf_policy

install : x86_energy_perf_policy
	install -d  $(DESTDIR)$(PREFIX)/bin
	install $(BUILD_OUTPUT)/x86_energy_perf_policy $(DESTDIR)$(PREFIX)/bin/x86_energy_perf_policy
	install -d  $(DESTDIR)$(PREFIX)/share/man/man8
	install -m 644 x86_energy_perf_policy.8 $(DESTDIR)$(PREFIX)/share/man/man8

