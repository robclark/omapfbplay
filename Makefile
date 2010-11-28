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
CPPFLAGS += $(FFMPEG:%=-I%)

CFLAGS = -O3 -g -Wall -fomit-frame-pointer -fno-tree-vectorize $(CPUFLAGS)

FFMPEG_LIBS = libavformat libavcodec libavcore libavutil

LDFLAGS = $(SYSROOT)
LDFLAGS += $(foreach FF,$(FFMPEG),$(addprefix -L$(FF)/,$(FFMPEG_LIBS)))
LDLIBS = $(FFMPEG_LIBS:lib%=-l%) -lm -lpthread -lrt $(EXTRA_LIBS)

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
CFLAGS-$(DCE)           += $(DCE_CFLAGS)

LDLIBS-$(CMEM)          += $(CMEM_LIBS)
LDLIBS-$(SDMA)          += $(SDMA_LIBS)
LDLIBS-$(XV)            += -lXv -lXext -lX11
LDLIBS-$(DCE)           += -ldce -lmemmgr

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
	rm -f $(O)*.o $(O)*.d $(O)omapfbplay

-include $(OBJ:.o=.d)
