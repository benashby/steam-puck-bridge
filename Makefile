CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
PREFIX  ?= $(HOME)/.local

BIN     := steam-puck-bridge
SRC     := src/steam-puck-bridge.c
UNIT    := systemd/steam-puck-bridge.service
UNITDIR := $(HOME)/.config/systemd/user

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $<

install: $(BIN)
	install -Dm755 $(BIN) $(PREFIX)/bin/$(BIN)
	install -Dm644 $(UNIT) $(UNITDIR)/steam-puck-bridge.service
	systemctl --user daemon-reload

enable: install
	systemctl --user enable --now steam-puck-bridge.service

uninstall:
	-systemctl --user disable --now steam-puck-bridge.service
	rm -f $(PREFIX)/bin/$(BIN)
	rm -f $(UNITDIR)/steam-puck-bridge.service
	systemctl --user daemon-reload

clean:
	rm -f $(BIN)

.PHONY: all install enable uninstall clean
