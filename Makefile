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
UNIT_IN   := systemd/steam-puck-bridge.service.in
UNIT      := systemd/steam-puck-bridge.service
RULES     := udev/60-steam-puck-bridge.rules

# Path baked into the unit's ExecStart=. A home install keeps systemd's %h
# specifier so one unit file works for any user; a system-wide install (a
# distro package, PREFIX=/usr) needs the real absolute path instead.
ifeq ($(strip $(BINDIR)),$(strip $(HOME)/.local/bin))
UNIT_BINDIR := %h/.local/bin
else
UNIT_BINDIR := $(BINDIR)
endif

# Set SKIP_RELOAD=1 for staged/packaging installs, where poking the running
# systemd instance is wrong (and usually impossible).
SKIP_RELOAD ?=

all: $(BIN) $(UNIT)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $<

# The unit has to be regenerated when the substituted path changes, not only
# when the template does — otherwise `make && make install PREFIX=/usr` reuses
# the unit generated for the home layout and ships a wrong ExecStart=. Make
# can't depend on a variable's value, so record it in a stamp file that is
# only rewritten (and so only triggers a rebuild) when it actually differs.
.bindir-stamp: FORCE
	@echo '$(UNIT_BINDIR)' | cmp -s - $@ 2>/dev/null || echo '$(UNIT_BINDIR)' > $@

$(UNIT): $(UNIT_IN) .bindir-stamp
	sed 's|@BINDIR@|$(UNIT_BINDIR)|g' $< > $@

FORCE:

install: $(BIN) $(UNIT)
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

# What CI runs. Must be a real optimizing compile, not -fsyntax-only:
# -Wmaybe-uninitialized and friends only fire after the optimizer runs.
check:
	gcc   $(CFLAGS) -Werror -c -o /dev/null $(SRC)
	clang $(CFLAGS) -Werror -c -o /dev/null $(SRC)

clean:
	rm -f $(BIN) $(UNIT) .bindir-stamp

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

.PHONY: all install enable uninstall install-udev uninstall-udev check clean help FORCE
