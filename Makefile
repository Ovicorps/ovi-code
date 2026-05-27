PREFIX     ?= /usr/local
BUILD_DIR  ?= build
BINARY     := ovi-code
JOBS       ?= $(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

.PHONY: all build configure install uninstall clean

all: build

configure:
	cmake -S . -B $(BUILD_DIR)

build: configure
	cmake --build $(BUILD_DIR) -j $(JOBS)

install: build
	install -d $(DESTDIR)$(PREFIX)/bin
	sudo install -m 755 $(BUILD_DIR)/$(BINARY) $(DESTDIR)$(PREFIX)/bin/$(BINARY)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BINARY)

clean:
	rm -rf $(BUILD_DIR)
