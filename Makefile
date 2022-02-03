CC=gcc
FUSE_CFLAGS=$(shell pkg-config --cflags fuse)
FUSE_LDFLAGS=$(shell pkg-config --libs fuse)
CFLAGS=-O2 -Wall -Werror fuzzyfs.c $(FUSE_CFLAGS) $(FUSE_LDFLAGS)

fuzzyfs: fuzzyfs.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o fuzzyfs

install:
	install fuzzyfs /usr/local/bin

clean:
	rm -f fuzzyfs
