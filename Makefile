LINUX  = ../linux-omap
FFMPEG = ../ffmpeg

SYSROOT = $(addprefix --sysroot=,$(ROOT))

CC = $(CROSS_COMPILE)gcc
CPPFLAGS = $(SYSROOT) -I$(LINUX)/include -I$(FFMPEG)
CFLAGS = -O3 -Wall -fomit-frame-pointer -mcpu=cortex-a8 -mfpu=neon
LDFLAGS = $(SYSROOT) -L$(FFMPEG)/libavcodec -L$(FFMPEG)/libavformat -L$(FFMPEG)/libavutil
LDLIBS = -lavformat -lavcodec -lavutil -lm -lz -lbz2 -lpthread -lrt

omapfbplay: omapfbplay.o timer.o yuv.o

clean:
	rm -f *.o omapfbplay
