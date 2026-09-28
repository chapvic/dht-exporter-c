# Makefile for dht-exporter
# Build: make
# Install: sudo make install

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
LDFLAGS ?= -pthread

PREFIX  ?= /usr/local
BINDIR   = $(PREFIX)/bin
UNITDIR  = /etc/systemd/system
CONFDIR  = /etc/default
STATEDIR = /var/lib/dht-exporter
USER     = dht-exporter

BIN      = dht-exporter
SRC      = dht-exporter.c
UNIT     = dht-exporter.service
CONF     = dht-exporter.default

.PHONY: all install uninstall clean start stop restart status logs

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $(BIN) $(SRC) $(LDFLAGS)

install: $(BIN)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)

	install -d $(DESTDIR)$(UNITDIR)
	install -m 644 $(UNIT) $(DESTDIR)$(UNITDIR)/$(UNIT)

	install -d $(DESTDIR)$(CONFDIR)
	install -m 644 $(CONF) $(DESTDIR)$(CONFDIR)/dht-exporter

	id $(USER) >/dev/null 2>&1 || \
		useradd --system --no-create-home --shell /usr/sbin/nologin $(USER)

	install -d -o $(USER) -g $(USER) $(DESTDIR)$(STATEDIR)

	systemctl daemon-reload
	systemctl enable $(BIN).service

uninstall:
	systemctl stop $(BIN).service 2>/dev/null || true
	systemctl disable $(BIN).service 2>/dev/null || true
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)
	rm -f $(DESTDIR)$(UNITDIR)/$(UNIT)
	rm -f $(DESTDIR)$(CONFDIR)/dht-exporter
	systemctl daemon-reload

start:
	systemctl start $(BIN).service

stop:
	systemctl stop $(BIN).service

restart:
	systemctl restart $(BIN).service

status:
	systemctl status $(BIN).service

logs:
	journalctl -u $(BIN).service -f

clean:
	rm -f $(BIN)
