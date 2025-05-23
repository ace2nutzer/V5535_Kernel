# SPDX-License-Identifier: GPL-2.0
include ../../scripts/Makefile.include
include ../../scripts/utilities.mak		# QUIET_CLEAN

ifeq ($(srctree),)
srctree := $(patsubst %/,%,$(dir $(CURDIR)))
srctree := $(patsubst %/,%,$(dir $(srctree)))
srctree := $(patsubst %/,%,$(dir $(srctree)))
#$(info Determined 'srctree' to be $(srctree))
endif

CC ?= $(CROSS_COMPILE)gcc
LD ?= $(CROSS_COMPILE)ld
AR ?= $(CROSS_COMPILE)ar

RM = rm -f

MAKEFLAGS += --no-print-directory

INSTALL = install

# Use DESTDIR for installing into a different root directory.
# This is useful for building a package. The program will be
# installed in this directory as if it was the root directory.
# Then the build tool can move it later.
DESTDIR ?=
DESTDIR_SQ = '$(subst ','\'',$(DESTDIR))'

LIBFILE = $(OUTPUT)libsubcmd.a

CFLAGS := -ggdb3 -Wall -Wextra -std=gnu99 -fPIC

ifeq ($(DEBUG),0)
  ifeq ($(feature-fortify-source), 1)
    CFLAGS += -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2
  endif
endif

ifeq ($(DEBUG),1)
  CFLAGS += -O0
else
  CFLAGS += -O2
endif

# Treat warnings as errors unless directed not to
ifneq ($(WERROR),0)
  CFLAGS += -Werror
endif

CFLAGS += -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE

CFLAGS += -I$(srctree)/tools/include/

CFLAGS += $(EXTRA_WARNINGS) $(EXTRA_CFLAGS)

SUBCMD_IN := $(OUTPUT)libsubcmd-in.o

ifeq ($(LP64), 1)
  libdir_relative = lib64
else
  libdir_relative = lib
endif

prefix ?=
libdir = $(prefix)/$(libdir_relative)

# Shell quotes
libdir_SQ = $(subst ','\'',$(libdir))

all:

export srctree OUTPUT CC LD CFLAGS V
include $(srctree)/tools/build/Makefile.include

all: fixdep $(LIBFILE)

$(SUBCMD_IN): fixdep FORCE
	@$(MAKE) $(build)=libsubcmd

$(LIBFILE): $(SUBCMD_IN)
	$(QUIET_AR)$(RM) $@ && $(AR) rcs $@ $(SUBCMD_IN)

define do_install_mkdir
	if [ ! -d '$(DESTDIR_SQ)$1' ]; then             \
		$(INSTALL) -d -m 755 '$(DESTDIR_SQ)$1'; \
	fi
endef

define do_install
	if [ ! -d '$2' ]; then             \
		$(INSTALL) -d -m 755 '$2'; \
	fi;                                             \
	$(INSTALL) $1 $(if $3,-m $3,) '$2'
endef

install_lib: $(LIBFILE)
	$(call QUIET_INSTALL, $(LIBFILE)) \
		$(call do_install_mkdir,$(libdir_SQ)); \
		cp -fpR $(LIBFILE) $(DESTDIR)$(libdir_SQ)

HDRS := exec-cmd.h help.h pager.h parse-options.h run-command.h
INSTALL_HDRS_PFX := $(DESTDIR)$(prefix)/include/subcmd
INSTALL_HDRS := $(addprefix $(INSTALL_HDRS_PFX)/, $(HDRS))

$(INSTALL_HDRS): $(INSTALL_HDRS_PFX)/%.h: %.h
	$(call QUIET_INSTALL, $@) \
		$(call do_install,$<,$(INSTALL_HDRS_PFX)/,644)

install_headers: $(INSTALL_HDRS)
	$(call QUIET_INSTALL, libsubcmd_headers)

install: install_lib install_headers

clean:
	$(call QUIET_CLEAN, libsubcmd) $(RM) $(LIBFILE); \
	find $(or $(OUTPUT),.) -name \*.o -or -name \*.o.cmd -or -name \*.o.d | xargs $(RM)

FORCE:

.PHONY: clean FORCE
