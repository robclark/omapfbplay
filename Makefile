CC = arm-omap3-linux-gnueabi-gcc
CFLAGS = -O3 -Wall -fomit-frame-pointer -mcpu=cortex-a8 -mfpu=neon \
	-I../linux-omap/include
LDFLAGS = -L../ffmpeg/libavcodec -L../ffmpeg/libavformat -L../ffmpeg/libavutil
LDLIBS = -lavformat -lavcodec -lavutil -lm -lz

all: fbplay
