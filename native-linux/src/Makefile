# Native Linux Wave:3 control daemon

CFLAGS  += -O2 -Wall -Wextra $(shell pkg-config --cflags glib-2.0 gio-2.0 libusb-1.0)
LDLIBS  += $(shell pkg-config --libs glib-2.0 gio-2.0 libusb-1.0) -lm

BIN      = wave3-daemon
TOOLS    = probe_wave3 restore_defaults cfg_probe watch_state device_info auto_probe poll_observatory
FUZZERS  = fuzz_vendor fuzz_vendor_smart
PROBES   = probe_vendor_ids probe_vendor_targeted probe_vendor_targeted2

.PHONY: all clean install

all: $(BIN)

$(BIN): wave3-daemon.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

# Helper probes (not installed)
probes: $(TOOLS)

vendor-probes: $(PROBES)

%: %.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f $(BIN) $(TOOLS) $(FUZZERS) $(PROBES)

PREFIX  ?= /usr/local
BINDIR   = $(PREFIX)/bin
DATADIR  = $(PREFIX)/share

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)
	install -Dm644 ../dbus/org.wave3.Daemon-system.conf $(DESTDIR)$(DATADIR)/dbus-1/system.d/org.wave3.Daemon.conf
	install -Dm644 ../systemd/wave3-daemon.service $(DESTDIR)$(DATADIR)/systemd/user/wave3-daemon.service
