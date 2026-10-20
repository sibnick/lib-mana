# SPDX-License-Identifier: BSD-3-Clause
CC ?= cc
PYTHON ?= python3
CPPFLAGS += -Iinclude
CFLAGS ?= -O2 -g
WARNINGS = -std=c11 -Wall -Wextra -Werror -pedantic
SOURCES = src/mana.c src/pci.c src/pci_ids.c src/gdma.c src/control.c src/data_queue.c src/tx.c src/rx.c
HEADERS = $(wildcard include/uk/*.h include/uk/mana/*.h src/*.h)
HOST_SUPPORT = tests/host/mock.c tests/host/bootstrap.c tests/host/device.c
# Test-only mirror headers, used by host builds of src/pci_ids.c.
HOST_INCLUDES = -Itests/host/include
HOST_HEADERS = $(wildcard tests/host/*.h)
HOST_TESTS = scaffold host bootstrap control data_queue tx rx device e2e
HOST_BINARIES = $(addprefix build/test_,$(HOST_TESTS))
HOST_SANITIZERS = $(addsuffix _sanitize,$(HOST_BINARIES))
ROOT := $(CURDIR)
UK_SOURCE ?= $(ROOT)/build/unikraft
UK_BUILD := $(ROOT)/build/unikraft-test
UK_MAKE = $(MAKE) -C $(UK_SOURCE) A=$(ROOT)/tests/unikraft \
	L=$(ROOT) O=$(UK_BUILD) C=$(UK_BUILD)/.config UK_CFLAGS=-std=gnu11
NETDEV_INCLUDES = -Itests/netdev/include \
	$(addprefix -isystem $(UK_SOURCE)/,include arch/x86/x86_64/include \
	arch/x86/include arch/include lib/uknetdev/include lib/ukalloc/include \
	lib/uklibparam/include lib/ukdebug/include lib/ukprint/include \
	lib/ukatomic/include lib/uklcpu/include)
NETDEV_SOURCES = $(SOURCES) src/netdev.c $(HOST_SUPPORT) tests/test_netdev.c \
	$(UK_SOURCE)/lib/uknetdev/netdev.c $(UK_SOURCE)/lib/uknetdev/netbuf.c

.PHONY: all test test-sanitize sanitize license-check clean prepare-unikraft test-msix unikraft-test test-netdev test-netdev-sanitize
all: test

build/test_%: tests/test_%.c $(SOURCES) $(HEADERS) $(HOST_SUPPORT) $(HOST_HEADERS) Makefile
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(HOST_INCLUDES) $(CFLAGS) $(WARNINGS) $(SOURCES) $(HOST_SUPPORT) $< \
		$(LDFLAGS) $(LDLIBS) -o $@

build/test_%_sanitize: tests/test_%.c $(SOURCES) $(HEADERS) $(HOST_SUPPORT) $(HOST_HEADERS) Makefile
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(HOST_INCLUDES) $(CFLAGS) $(WARNINGS) -O0 -fsanitize=address,undefined \
		-fno-omit-frame-pointer $(SOURCES) $(HOST_SUPPORT) $< $(LDFLAGS) \
		-fsanitize=address,undefined $(LDLIBS) -o $@

test: license-check $(HOST_BINARIES) test-netdev
	@set -e; for test in $(HOST_BINARIES); do ./$$test; done

sanitize: test-sanitize
test-sanitize: license-check $(HOST_SANITIZERS) test-netdev-sanitize
	@set -e; for test in $(HOST_SANITIZERS); do ./$$test; done

build/test_netdev: $(NETDEV_SOURCES) $(HEADERS) $(HOST_HEADERS) Makefile \
		$(wildcard tests/netdev/include/uk/*.h tests/netdev/include/uk/bits/*.h)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(HOST_INCLUDES) $(NETDEV_INCLUDES) $(CFLAGS) -std=gnu11 \
		-Wall -Wextra -Werror $(NETDEV_SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

build/test_netdev_sanitize: $(NETDEV_SOURCES) $(HEADERS) $(HOST_HEADERS) Makefile \
		$(wildcard tests/netdev/include/uk/*.h tests/netdev/include/uk/bits/*.h)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(HOST_INCLUDES) $(NETDEV_INCLUDES) $(CFLAGS) -O0 -std=gnu11 \
		-Wall -Wextra -Werror -fsanitize=address,undefined \
		-fno-omit-frame-pointer $(NETDEV_SOURCES) $(LDFLAGS) \
		-fsanitize=address,undefined $(LDLIBS) -o $@

$(UK_SOURCE)/lib/uknetdev/netdev.c $(UK_SOURCE)/lib/uknetdev/netbuf.c: | prepare-unikraft
	@test -f $@

test-netdev: prepare-unikraft license-check build/test_netdev
	./build/test_netdev

test-netdev-sanitize: prepare-unikraft license-check build/test_netdev_sanitize
	./build/test_netdev_sanitize

license-check:
	$(PYTHON) scripts/check-license.py

prepare-unikraft:
	@if [ "$(UK_SOURCE)" = "$(ROOT)/build/unikraft" ]; then \
		sh scripts/prepare-unikraft.sh; \
	else \
		patch --dry-run -R -p1 -d "$(UK_SOURCE)" \
			< patches/unikraft-e31b2c44-msix.patch; \
	fi

test-msix: prepare-unikraft license-check
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -Itests/msix/include \
		-I$(UK_SOURCE)/drivers/ukintctlr/xpic/include \
		tests/test_msix.c $(UK_SOURCE)/drivers/ukintctlr/xpic/msix.c \
		$(LDFLAGS) $(LDLIBS) -o build/test_msix
	./build/test_msix
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -fsanitize=undefined \
		-fno-sanitize-recover=undefined -Itests/msix/include \
		-I$(UK_SOURCE)/arch/x86/x86_64/include \
		-I$(UK_SOURCE)/drivers/ukintctlr/xpic \
		tests/test_pic.c $(UK_SOURCE)/drivers/ukintctlr/xpic/pic.c \
		$(LDFLAGS) $(LDLIBS) -o build/test_pic
	./build/test_pic

unikraft-test: test-msix
	$(UK_MAKE) UK_DEFCONFIG=$(ROOT)/tests/unikraft/defconfig defconfig
	@grep -qx 'CONFIG_LIBMANA=y' $(UK_BUILD)/.config || \
		{ echo "error: MANA was not enabled by Kconfig" >&2; exit 1; }
	$(UK_MAKE) -j2

clean:
	$(RM) $(HOST_BINARIES) $(HOST_SANITIZERS) build/test_msix build/test_pic \
		build/test_netdev build/test_netdev_sanitize
