-include $(or $(CONFIG),$(ARCH),$(shell uname -m)).mk

SYSROOT = $(addprefix --sysroot=,$(ROOT))

CC = $(CROSS_COMPILE)gcc
CPPFLAGS = $(SYSROOT) -I$(LINUX)/include -I$(FFMPEG)
CFLAGS ?= -O3 -g -Wall -fomit-frame-pointer
LDFLAGS = $(SYSROOT) -L$(FFMPEG)/libavcodec -L$(FFMPEG)/libavformat -L$(FFMPEG)/libavutil
LDLIBS = -lavformat -lavcodec -lavutil -lm -lz -lbz2 -lpthread -lrt

ALL = omapfbplay xvplay

default: $(or $(DEFAULT),help)

help:
	@echo Available targets: $(ALL)

all: $(ALL)

omapfbplay: omapfbplay.o omapfb.o timer.o yuv.o

xvplay: omapfbplay.o xv.o timer.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) -lXv -lXext -lX11

clean:
	rm -f *.o omapfbplay xvplay
