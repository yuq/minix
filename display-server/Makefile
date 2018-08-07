#!makefile

FLAGS := $(shell pkg-config --libs --cflags gbm libdrm epoxy)

all:
	gcc -std=c99 -g -o display-server server.c client.c share.c $(FLAGS)

