-include $(or $(CONFIG),$(ARCH),$(shell uname -m)).mk

SYSROOT = $(addprefix --sysroot=,$(ROOT))

CC = $(CROSS_COMPILE)gcc
CPPFLAGS = $(SYSROOT) -I$(LINUX)/include -I$(FFMPEG)
CFLAGS = -O3 -g -Wall -fomit-frame-pointer $(CPUFLAGS)
LDFLAGS = $(SYSROOT) -L$(FFMPEG)/libavcodec -L$(FFMPEG)/libavformat -L$(FFMPEG)/libavutil
LDLIBS = -lavformat -lavcodec -lavutil -lm -lz -lbz2 -lpthread -lrt

DRV-y                    = timer.o
DRV-$(OMAPFB)           += omapfb.o yuv.o
DRV-$(XV)               += xv.o

LDLIBS-$(XV)            += -lXv -lXext -lX11

LDLIBS += $(LDLIBS-y)

omapfbplay: omapfbplay.o magic-head.o $(DRV-y) magic-tail.o

clean:
	rm -f *.o omapfbplay
