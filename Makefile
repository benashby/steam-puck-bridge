CC        ?= gcc
CFLAGS    ?= -O2 -std=c99 -Wall -Wextra
PREFIX    ?= $(HOME)/.local
BINDIR    ?= $(PREFIX)/bin
UNITDIR   ?= $(HOME)/.config/systemd/user
UDEVDIR   ?= /etc/udev/rules.d
DESTDIR   ?=
INSTALL   ?= install

BIN       := steam-puck-bridge
SRC       := src/steam-puck-bridge.c
UNIT      := systemd/steam-puck-bridge.service
RULES     := udev/60-steam-puck-bridge.rules

# Set SKIP_RELOAD=1 for staged/packaging installs, where poking the running
# systemd instance is wrong (and usually impossible).
SKIP_RELOAD ?=

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $<

install: $(BIN)
	$(INSTALL) -Dm755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)
	$(INSTALL) -Dm644 $(UNIT) $(DESTDIR)$(UNITDIR)/steam-puck-bridge.service
ifeq ($(SKIP_RELOAD),)
	systemctl --user daemon-reload
endif

enable: install
	systemctl --user enable --now steam-puck-bridge.service

uninstall:
	-systemctl --user disable --now steam-puck-bridge.service
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)
	rm -f $(DESTDIR)$(UNITDIR)/steam-puck-bridge.service
ifeq ($(SKIP_RELOAD),)
	systemctl --user daemon-reload
endif

# Only needed on distros without Valve's steam-devices rules. Needs root.
install-udev:
	$(INSTALL) -Dm644 $(RULES) $(DESTDIR)$(UDEVDIR)/60-steam-puck-bridge.rules
ifeq ($(SKIP_RELOAD),)
	udevadm control --reload-rules
	udevadm trigger
endif

uninstall-udev:
	rm -f $(DESTDIR)$(UDEVDIR)/60-steam-puck-bridge.rules
ifeq ($(SKIP_RELOAD),)
	udevadm control --reload-rules
endif

# What CI runs.
check:
	gcc   $(CFLAGS) -Werror -fsyntax-only $(SRC)
	clang $(CFLAGS) -Werror -fsyntax-only $(SRC)

clean:
	rm -f $(BIN)

help:
	@echo 'targets:'
	@echo '  make                 build ./$(BIN)'
	@echo '  make install         -> $(BINDIR)/$(BIN) + user systemd unit'
	@echo '  make enable          install, then systemctl --user enable --now'
	@echo '  make uninstall       remove both'
	@echo '  make install-udev    device access rules (root; non-Bazzite distros)'
	@echo '  make check           compile clean under gcc and clang with -Werror'
	@echo '  make clean           remove build output'
	@echo
	@echo 'variables: CC CFLAGS PREFIX BINDIR UNITDIR UDEVDIR DESTDIR SKIP_RELOAD'
	@echo 'e.g. system-wide staged install:'
	@echo '  make install DESTDIR=/tmp/stage PREFIX=/usr \'
	@echo '       UNITDIR=/usr/lib/systemd/user SKIP_RELOAD=1'

.PHONY: all install enable uninstall install-udev uninstall-udev check clean help
