CXX      := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Wpedantic -D_GNU_SOURCE
LDFLAGS  :=
LIBS_D   := -lcrypto -lcap
LIBS_S   := -lcrypto

PREFIX   := /usr/local
CONFDIR  := /etc
UNITDIR  := /etc/systemd/system

.PHONY: all clean install uninstall

all: poweroffd poweroff-send

poweroffd: poweroffd.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS) $(LIBS_D)

poweroff-send: poweroff-send.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS) $(LIBS_S)

clean:
	rm -f poweroffd poweroff-send

install: all
	install -m 0755 poweroffd     $(DESTDIR)$(PREFIX)/sbin/poweroffd
	install -m 0755 poweroff-send $(DESTDIR)$(PREFIX)/bin/poweroff-send
	@if [ ! -f $(DESTDIR)$(CONFDIR)/poweroffd.conf ]; then \
		install -m 0600 poweroffd.conf $(DESTDIR)$(CONFDIR)/poweroffd.conf; \
		echo "Installed default config to $(CONFDIR)/poweroffd.conf"; \
	else \
		echo "Config $(CONFDIR)/poweroffd.conf already exists — not overwriting"; \
	fi
	install -m 0644 poweroffd.service $(DESTDIR)$(UNITDIR)/poweroffd.service
	systemctl daemon-reload
	@echo ""
	@echo "Installed. Edit $(CONFDIR)/poweroffd.conf then:"
	@echo "  sudo systemctl enable --now poweroffd"

uninstall:
	systemctl stop poweroffd 2>/dev/null || true
	systemctl disable poweroffd 2>/dev/null || true
	rm -f $(DESTDIR)$(PREFIX)/sbin/poweroffd
	rm -f $(DESTDIR)$(PREFIX)/bin/poweroff-send
	rm -f $(DESTDIR)$(UNITDIR)/poweroffd.service
	systemctl daemon-reload
	@echo "Uninstalled. Config file $(CONFDIR)/poweroffd.conf preserved."
