-include $(or $(CONFIG),$(ARCH),$(shell uname -m)).mk

$(if $(findstring y,$(OMAPFB) $(XV) $(V4L2)),,$(error No display drivers enabled))

override O := $(O:%=$(O:%/=%)/)

ARCH ?= generic
$(ARCH) = y

SYSROOT = $(addprefix --sysroot=,$(ROOT))

CC = $(CROSS_COMPILE)gcc

CPPFLAGS = $(SYSROOT) -MMD
CPPFLAGS += $(LINUX:%=-I%/include)
CPPFLAGS += $(and $(LINUX),$(ARCH),-I$(LINUX)/arch/$(ARCH)/include)
CPPFLAGS += $(LIBAV:%=-I%)

CFLAGS = -O3 -g -Wall -fomit-frame-pointer -fno-tree-vectorize $(CPUFLAGS)

LIBAV_LIBS = libavformat libavcodec libavutil

LDFLAGS = $(SYSROOT)
LDFLAGS += $(foreach AV,$(LIBAV),$(addprefix -L$(AV)/,$(LIBAV_LIBS)))
LDLIBS = $(LIBAV_LIBS:lib%=-l%) -lm -lpthread -lrt $(EXTRA_LIBS)

DRV-y                    = sysclk.o sysmem.o avcodec.o
DRV-$(CMEM)             += cmem.o
DRV-$(NETSYNC)          += netsync.o
DRV-$(OMAPFB)           += omapfb.o
DRV-$(arm)              += neon_pixconv.o
DRV-$(SDMA)             += sdma.o
DRV-$(XV)               += xv.o
DRV-$(V4L2)             += v4l2.o
DRV-$(DCE)              += dce.o

CFLAGS-$(CMEM)          += $(CMEM_CFLAGS)
CFLAGS-$(SDMA)          += $(SDMA_CFLAGS)
CFLAGS-$(DCE)           += `pkg-config --cflags libdce`

LDLIBS-$(CMEM)          += $(CMEM_LIBS)
LDLIBS-$(SDMA)          += $(SDMA_LIBS)
LDLIBS-$(XV)            += -lXv -lXext -lX11
LDLIBS-$(DCE)           += `pkg-config --libs libdce`

CFLAGS += $(CFLAGS-y)
LDLIBS += $(LDLIBS-y)

CORE = omapfbplay.o pixfmt.o time.o
DRV  = magic-head.o $(DRV-y) magic-tail.o
OBJ  = $(addprefix $(O),$(CORE) $(DRV))

$(O)omapfbplay: $(OBJ)

$(O)%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(O)%.o: %.S
	$(CC) $(CPPFLAGS) $(ASFLAGS) -c -o $@ $<

clean:
	rm -f $(O)*.o $(O)*.d $(O)omapfbplay

-include $(OBJ:.o=.d)
