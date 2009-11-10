-include $(or $(CONFIG),$(ARCH),$(shell uname -m)).mk

$(if $(findstring y,$(OMAPFB) $(XV)),,$(error No display drivers enabled))

override O := $(and $(O),$(O:%/=%)/)

SYSROOT = $(addprefix --sysroot=,$(ROOT))

CC = $(CROSS_COMPILE)gcc

CPPFLAGS = $(SYSROOT) -MMD
CPPFLAGS += $(and $(LINUX),-I$(LINUX)/include $(and $(ARCH),-I$(LINUX)/arch/$(ARCH)/include))
CPPFLAGS += $(and $(FFMPEG),-I$(FFMPEG))

CFLAGS = -O3 -g -Wall -fomit-frame-pointer -fno-tree-vectorize $(CPUFLAGS)

LDFLAGS = $(SYSROOT)
LDFLAGS += $(and $(FFMPEG),-L$(FFMPEG)/libavcodec -L$(FFMPEG)/libavformat -L$(FFMPEG)/libavutil)
LDLIBS = -lavformat -lavcodec -lavutil -lm -lpthread -lrt $(EXTRA_LIBS)

DRV-y                    = sysclk.o
DRV-$(NETSYNC)          += netsync.o
DRV-$(OMAPFB)           += omapfb.o yuv.o
DRV-$(XV)               += xv.o

LDLIBS-$(XV)            += -lXv -lXext -lX11

LDLIBS += $(LDLIBS-y)

OBJ = $(addprefix $(O),omapfbplay.o time.o magic-head.o $(DRV-y) magic-tail.o)

$(O)omapfbplay: $(OBJ)

$(O)%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(O)%.o: %.S
	$(CC) $(CPPFLAGS) $(ASFLAGS) -c -o $@ $<

clean:
	rm -f $(O)*.o $(O)omapfbplay

-include $(OBJ:.o=.d)
