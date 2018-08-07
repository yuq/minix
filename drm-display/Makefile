#!makefile

FLAGS := $(shell pkg-config --libs --cflags gbm libdrm epoxy libpng)

all:
	gcc -std=c99 -g -o drm-display main.c $(FLAGS)

