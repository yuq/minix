#!makefile

FLAGS := $(shell pkg-config --libs --cflags gbm libdrm epoxy) -lm

all:
	gcc -std=c99 -g -o implicit-sync server.c client.c share.c $(FLAGS)

