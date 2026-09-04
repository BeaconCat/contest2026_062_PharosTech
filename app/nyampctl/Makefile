include $(APPDIR)/Make.defs

PROGNAME  = $(CONFIG_EXAMPLES_NYAMPCTL_PROGNAME)
PRIORITY  = $(CONFIG_EXAMPLES_NYAMPCTL_PRIORITY)
STACKSIZE = $(CONFIG_EXAMPLES_NYAMPCTL_STACKSIZE)
MODULE    = $(CONFIG_EXAMPLES_NYAMPCTL)

MAINSRC = nyampctl_main.c
CSRCS   = nyamp_protocol.c

VPATH += $(TOPDIR)/../vendor/rockchip/tools/amp/protocol
CFLAGS += ${INCDIR_PREFIX}$(TOPDIR)/../vendor/rockchip/tools/amp/protocol

include $(APPDIR)/Application.mk
