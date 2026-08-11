PREFIX     ?= /usr/local
BUILD_DIR  ?= build
BINARY     := ovi-code
JOBS       ?= $(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
ARGS       ?=
DOCS_REF   ?= main

.PHONY: all build configure run install uninstall clean docs-install docs docs-serve docs-deploy

all: build

configure:
	cmake -S . -B $(BUILD_DIR)

build: configure
	cmake --build $(BUILD_DIR) -j $(JOBS)

run: build
	$(BUILD_DIR)/$(BINARY) $(ARGS)

install: build
	install -d $(DESTDIR)$(PREFIX)/bin
	sudo install -m 755 $(BUILD_DIR)/$(BINARY) $(DESTDIR)$(PREFIX)/bin/$(BINARY)

uninstall:
	sudo rm -f $(DESTDIR)$(PREFIX)/bin/$(BINARY)

clean:
	rm -rf $(BUILD_DIR)

docs-install:
	python3 -m pip install -r requirements-docs.txt

docs:
	python3 -m mkdocs build --strict

docs-serve:
	python3 -m mkdocs serve

docs-deploy:
	gh workflow run docs.yml --ref $(DOCS_REF)
