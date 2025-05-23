# SPDX-License-Identifier: GPL-2.0
OUTPUT := .output
abs_out := $(abspath $(OUTPUT))

CLANG ?= clang
LLC ?= llc
LLVM_STRIP ?= llvm-strip

TOOLS_PATH := $(abspath ../../../../tools)
BPFTOOL_SRC := $(TOOLS_PATH)/bpf/bpftool
BPFTOOL_OUTPUT := $(abs_out)/bpftool
DEFAULT_BPFTOOL := $(BPFTOOL_OUTPUT)/bootstrap/bpftool
BPFTOOL ?= $(DEFAULT_BPFTOOL)

LIBBPF_SRC := $(TOOLS_PATH)/lib/bpf
LIBBPF_OUTPUT := $(abs_out)/libbpf
LIBBPF_DESTDIR := $(LIBBPF_OUTPUT)
LIBBPF_INCLUDE := $(LIBBPF_DESTDIR)/include
BPFOBJ := $(LIBBPF_OUTPUT)/libbpf.a

INCLUDES := -I$(OUTPUT) -I$(LIBBPF_INCLUDE) -I$(TOOLS_PATH)/include/uapi
CFLAGS := -g -Wall

VMLINUX_BTF_PATHS ?= $(if $(O),$(O)/vmlinux)				\
		     $(if $(KBUILD_OUTPUT),$(KBUILD_OUTPUT)/vmlinux)	\
		     ../../../../vmlinux				\
		     /sys/kernel/btf/vmlinux				\
		     /boot/vmlinux-$(shell uname -r)
VMLINUX_BTF ?= $(abspath $(firstword $(wildcard $(VMLINUX_BTF_PATHS))))
ifeq ($(VMLINUX_BTF),)
$(error Cannot find a vmlinux for VMLINUX_BTF at any of "$(VMLINUX_BTF_PATHS)")
endif

ifeq ($(V),1)
Q =
msg =
else
Q = @
msg = @printf '  %-8s %s%s\n' "$(1)" "$(notdir $(2))" "$(if $(3), $(3))";
MAKEFLAGS += --no-print-directory
submake_extras := feature_display=0
endif

.DELETE_ON_ERROR:

.PHONY: all clean

SOURCES = $(wildcard *.bpf.c)
TARGETS = $(SOURCES:.bpf.c=.bpf.o)

all: $(TARGETS)

clean:
	$(call msg,CLEAN)
	$(Q)rm -rf $(OUTPUT) $(TARGETS)

%.bpf.o: %.bpf.c vmlinux.h $(BPFOBJ) | $(OUTPUT)
	$(call msg,BPF,$@)
	$(Q)$(CLANG) -g $(OPT_LVL) --target=bpf -Wall -Werror $(INCLUDES)	      \
		 -c $(filter %.c,$^) -o $@ &&				      \
	$(LLVM_STRIP) -g $@

vmlinux.h: $(VMLINUX_BTF) $(BPFTOOL) | $(INCLUDE_DIR)
ifeq ($(VMLINUX_H),)
	$(call msg,GEN,,$@)
	$(Q)$(BPFTOOL) btf dump file $(VMLINUX_BTF) format c > $@
else
	$(call msg,CP,,$@)
	$(Q)cp "$(VMLINUX_H)" $@
endif

$(OUTPUT) $(LIBBPF_OUTPUT) $(BPFTOOL_OUTPUT):
	$(call msg,MKDIR,$@)
	$(Q)mkdir -p $@

$(BPFOBJ): $(wildcard $(LIBBPF_SRC)/*.[ch] $(LIBBPF_SRC)/Makefile) | $(LIBBPF_OUTPUT)
	$(Q)$(MAKE) $(submake_extras) -C $(LIBBPF_SRC)			       \
		    OUTPUT=$(abspath $(dir $@))/ prefix=		       \
		    DESTDIR=$(LIBBPF_DESTDIR) $(abspath $@) install_headers

ifeq ($(CROSS_COMPILE),)
$(DEFAULT_BPFTOOL): $(BPFOBJ) | $(BPFTOOL_OUTPUT)
	$(Q)$(MAKE) $(submake_extras) -C $(BPFTOOL_SRC)			       \
		    OUTPUT=$(BPFTOOL_OUTPUT)/				       \
		    LIBBPF_BOOTSTRAP_OUTPUT=$(LIBBPF_OUTPUT)/		       \
		    LIBBPF_BOOTSTRAP_DESTDIR=$(LIBBPF_DESTDIR)/ bootstrap
else
$(DEFAULT_BPFTOOL): | $(BPFTOOL_OUTPUT)
	$(Q)$(MAKE) $(submake_extras) -C $(BPFTOOL_SRC)			       \
		    OUTPUT=$(BPFTOOL_OUTPUT)/ bootstrap
endif
