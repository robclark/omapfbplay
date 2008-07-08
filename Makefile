LINUX  = ../linux-omap
FFMPEG = ../ffmpeg

CC = arm-omap3-linux-gnueabi-gcc
CFLAGS = -O3 -Wall -fomit-frame-pointer -mcpu=cortex-a8 -mfpu=neon \
	-I$(LINUX)/include -I$(FFMPEG)
LDFLAGS = -L$(FFMPEG)/libavcodec -L$(FFMPEG)/libavformat -L$(FFMPEG)/libavutil
LDLIBS = -lavformat -lavcodec -lavutil -lm -lz

all: omapfbplay
