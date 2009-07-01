LINUX  = ../linux-omap
FFMPEG = ../ffmpeg

CC = arm-omap3-linux-gnueabi-gcc
CPPFLAGS = -I$(LINUX)/include -I$(FFMPEG)
CFLAGS = -O3 -Wall -fomit-frame-pointer -mcpu=cortex-a8 -mfpu=neon
LDFLAGS = -L$(FFMPEG)/libavcodec -L$(FFMPEG)/libavformat -L$(FFMPEG)/libavutil
LDLIBS = -lavformat -lavcodec -lavutil -lm -lz -lpthread -lrt

omapfbplay: omapfbplay.o yuv.o

clean:
	rm -f *.o omapfbplay
