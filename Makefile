CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -D_FILE_OFFSET_BITS=64 \
          -Iinclude $(shell pkg-config --cflags fuse3)
LDFLAGS = $(shell pkg-config --libs fuse3)

VERSION = 0.1.0
PREFIX  = /usr

LIB     = libpartfs.so
BIN     = partfs
PC      = libpartfs.pc

LIB_SRCS = src/crc32c.c \
           src/io.c \
           src/alloc.c \
           src/btree.c \
           src/inode.c \
           src/dir.c \
           src/file.c \
           src/fuse_ops.c

BIN_SRCS = src/main.c

HEADERS = include/partfs.h $(wildcard src/*.h)

PUB_HEADERS = include/partfs.h \
              src/crc32c.h \
              src/io.h \
              src/alloc.h \
              src/btree.h \
              src/inode.h \
              src/dir.h \
              src/file.h \
              src/fuse_ops.h

all: $(LIB) $(BIN) $(PC)

$(LIB): $(LIB_SRCS) $(HEADERS)
	$(CC) $(CFLAGS) -fPIC -shared -o $@ $(LIB_SRCS) $(LDFLAGS)

$(BIN): $(BIN_SRCS) $(LIB) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $(BIN_SRCS) -L. -lpartfs $(LDFLAGS)

$(PC): libpartfs.pc.in
	sed -e 's|@PREFIX@|$(PREFIX)|g' \
	    -e 's|@VERSION@|$(VERSION)|g' \
	    $< > $@

install: all
	install -Dm755 $(LIB)  $(DESTDIR)$(PREFIX)/lib/$(LIB)
	install -Dm755 $(BIN)  $(DESTDIR)$(PREFIX)/bin/$(BIN)
	install -Dm644 $(PC)   $(DESTDIR)$(PREFIX)/lib/pkgconfig/$(PC)
	for h in $(PUB_HEADERS); do \
	    install -Dm644 $$h $(DESTDIR)$(PREFIX)/include/partfs/$$(basename $$h); \
	done
	ldconfig

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/lib/$(LIB)
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN)
	rm -f $(DESTDIR)$(PREFIX)/lib/pkgconfig/$(PC)
	rm -rf $(DESTDIR)$(PREFIX)/include/partfs
	ldconfig

clean:
	rm -f $(LIB) $(BIN) $(PC)

.PHONY: all install uninstall clean
