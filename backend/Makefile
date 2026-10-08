# Kodak i2x00 SANE backend, built out of tree against libsane-dev and libusb-1.0.
#   make            build/libsane-kodak_i2x00.so.1 and the sequence file
#   make check      offline tests (no scanner needed)
#   make testenv    build/conf: use the backend without installing it
#                   (LD_LIBRARY_PATH=build SANE_CONFIG_DIR=build/conf scanimage -L)
#   sudo make install

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra -fPIC -fvisibility=hidden $(shell pkg-config --cflags libusb-1.0)
LDLIBS = $(shell pkg-config --libs libusb-1.0) -lpthread -lm

PREFIX ?= /usr
SANELIBDIR ?= $(shell pkg-config --variable=libdir sane-backends)/sane
CONFDIR ?= /etc/sane.d
DATADIR ?= $(PREFIX)/share/sane/kodak_i2x00
SEQ_JSON = ../pi/scan-station/sequences/color300-duplex.json

B = build
LIB = $(B)/libsane-kodak_i2x00.so.1
OBJS = $(B)/kodak_i2x00.o $(B)/kds_dev.o $(B)/kds_split.o $(B)/kds_image.o

all: $(LIB) $(B)/color300-duplex.seq

$(B)/%.o: %.c kds.h | $(B)
	$(CC) $(CFLAGS) -DKDS_DATADIR='"$(DATADIR)"' -DKDS_CONFDIR='"$(CONFDIR)"' -c -o $@ $<

$(LIB): $(OBJS)
	$(CC) -shared -Wl,-soname,libsane-kodak_i2x00.so.1 -o $@ $(OBJS) $(LDLIBS)

$(B)/color300-duplex.seq: $(SEQ_JSON) tools/json2seq.py | $(B)
	python3 tools/json2seq.py $(SEQ_JSON) $@

$(B)/test_offline: tests/test_offline.c $(B)/kds_dev.o $(B)/kds_split.o $(B)/kds_image.o
	$(CC) $(CFLAGS) -I. -o $@ $^ $(LDLIBS)

check: $(B)/test_offline $(B)/color300-duplex.seq
	$(B)/test_offline $(B)/color300-duplex.seq $(TEST_STREAMS)

testenv: all
	mkdir -p $(B)/conf
	echo kodak_i2x00 > $(B)/conf/dll.conf
	printf 'usb 0x040a 0x601d\nsequence %s\n' "$(abspath $(B)/color300-duplex.seq)" > $(B)/conf/kodak_i2x00.conf

install: all
	install -D -m 644 $(LIB) $(DESTDIR)$(SANELIBDIR)/libsane-kodak_i2x00.so.1
	install -D -m 644 $(B)/color300-duplex.seq $(DESTDIR)$(DATADIR)/color300-duplex.seq
	test -e $(DESTDIR)$(CONFDIR)/kodak_i2x00.conf || install -D -m 644 kodak_i2x00.conf $(DESTDIR)$(CONFDIR)/kodak_i2x00.conf
	mkdir -p $(DESTDIR)$(CONFDIR)/dll.d && echo kodak_i2x00 > $(DESTDIR)$(CONFDIR)/dll.d/kodak_i2x00

uninstall:
	rm -f $(DESTDIR)$(SANELIBDIR)/libsane-kodak_i2x00.so.1 $(DESTDIR)$(CONFDIR)/dll.d/kodak_i2x00
	rm -rf $(DESTDIR)$(DATADIR)

$(B):
	mkdir -p $(B)

clean:
	rm -rf $(B)

.PHONY: all check testenv install uninstall clean
