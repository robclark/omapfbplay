-include $(or $(CONFIG),$(ARCH),$(shell uname -m)).mk

$(if $(findstring y,$(OMAPFB) $(XV)),,$(error No display drivers enabled))

override O := $(O:%=$(O:%/=%)/)

ARCH ?= generic
$(ARCH) = y

SYSROOT = $(addprefix --sysroot=,$(ROOT))

CC = $(CROSS_COMPILE)gcc

CPPFLAGS = $(SYSROOT) -MMD
CPPFLAGS += $(LINUX:%=-I%/include)
CPPFLAGS += $(and $(LINUX),$(ARCH),-I$(LINUX)/arch/$(ARCH)/include)
CPPFLAGS += $(FFMPEG:%=-I%)

CFLAGS = -O3 -g -Wall -fomit-frame-pointer -fno-tree-vectorize $(CPUFLAGS)

FFMPEG_LIBS = libavcodec libavformat libavutil

LDFLAGS = $(SYSROOT)
LDFLAGS += $(FFMPEG:%=$(addprefix -L$(FFMPEG)/,$(FFMPEG_LIBS)))
LDLIBS = -lavformat -lavcodec -lavutil -lm -lpthread -lrt $(EXTRA_LIBS)

DRV-y                    = sysclk.o sysmem.o
DRV-$(CMEM)             += cmem.o
DRV-$(NETSYNC)          += netsync.o
DRV-$(OMAPFB)           += omapfb.o
DRV-$(arm)              += neon_pixconv.o
DRV-$(SDMA)             += sdma.o
DRV-$(XV)               += xv.o

CFLAGS-$(CMEM)          += $(CMEM_CFLAGS)
CFLAGS-$(SDMA)          += $(SDMA_CFLAGS)

LDLIBS-$(CMEM)          += $(CMEM_LIBS)
LDLIBS-$(SDMA)          += $(SDMA_LIBS)
LDLIBS-$(XV)            += -lXv -lXext -lX11

CFLAGS += $(CFLAGS-y)
LDLIBS += $(LDLIBS-y)

CORE = omapfbplay.o time.o
DRV  = magic-head.o $(DRV-y) magic-tail.o
OBJ  = $(addprefix $(O),$(CORE) $(DRV))

$(O)omapfbplay: $(OBJ)

$(O)%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(O)%.o: %.S
	$(CC) $(CPPFLAGS) $(ASFLAGS) -c -o $@ $<

clean:
	rm -f $(O)*.o $(O)omapfbplay

-include $(OBJ:.o=.d)
