# ioxd - a static and shared library plus the playground examples that link it.
#
#   make            build libioxd.a, libioxd.so and the examples
#   make lib        just the libraries
#   make check      the unit test and the suites, against the fixture servers (QUIC's too, in a QUIC build)
#   make check-tiny the stress suite against a build with the buffers starved on purpose
#   make check-all  both
#   make tidy       clang-tidy over the library
#   make install    install libs, ioxd.h and ioxd/*.h (under <prefix>/include) and ioxd.pc
#   sudo make install PREFIX=/usr/local
#
# Downstream then builds with:  cc app.c $(pkg-config --cflags --libs ioxd) -o app

# The compiler: unless CC is given, the newest gcc on the PATH (a distro's `gcc` is often older
# than a `gcc-NN` installed beside it). The code is C23, which needs gcc 14 or newer.
ifeq ($(origin CC),default)
CC := $(shell for c in gcc gcc-14 gcc-15 gcc-16; do command -v $$c >/dev/null 2>&1 && echo "$$($$c -dumpfullversion) $$c"; done | sort -V | tail -1 | cut -d' ' -f2)
CC := $(if $(CC),$(CC),gcc)
endif
STD     := $(shell $(CC) -std=gnu23 -x c -c /dev/null -o /dev/null 2>/dev/null && echo -std=gnu23)
ifeq ($(STD),)
$(error $(CC) does not know -std=gnu23: libioxd is C23 and needs gcc 14 or newer (make CC=gcc-14))
endif
AR      ?= ar
# Fat LTO objects when the compiler supports them: the archive stays linkable by anyone (it also
# carries plain machine code), and a consumer that links with -flto gets cross-file inlining -
# including its own handlers into the engine. Measured ~+2% on saturated throughput.
LTO     := $(shell $(CC) -Werror -flto -ffat-lto-objects -x c -c /dev/null -o /dev/null 2>/dev/null && echo -flto -ffat-lto-objects)
CFLAGS  ?= -O3 -g $(LTO)
WARN    := -Wall -Wextra $(STD)
# A coroutine's stack ends at a guard page, which only stops a frame that touches every page as it
# grows: stack clash protection makes the compiler emit those probes.
HARDEN  := $(shell $(CC) -Werror -fstack-clash-protection -x c -c /dev/null -o /dev/null 2>/dev/null && echo -fstack-clash-protection)
CPP     := -D_GNU_SOURCE -Iinclude -Ilib -Ithird_party/picohttpparser
# TLS: OpenSSL for the handshake only; the kernel does the records. make TLS=0 leaves it out.
# pkg-config finds the OpenSSL to build against when there is one to find (PKG_CONFIG_PATH picks
# a private build); otherwise the toolchain's default is what -lssl names.
TLS     ?= 1
ifeq ($(TLS),1)
CPP     += -DIOXD_TLS=1 $(shell pkg-config --cflags openssl 2>/dev/null)
LIBS    := $(shell pkg-config --libs openssl 2>/dev/null || echo -lssl -lcrypto)
else
CPP     += -DIOXD_TLS=0
LIBS    :=
endif
# QUIC: ngtcp2 over OpenSSL 3.5's QUIC TLS API (libngtcp2_crypto_ossl). In by default when
# pkg-config finds both; make QUIC=1 insists, QUIC=0 leaves it out. Needs TLS=1.
QUIC    ?= $(if $(filter 1,$(TLS)),$(shell pkg-config --exists 'libngtcp2 libngtcp2_crypto_ossl' 2>/dev/null && echo 1 || echo 0),0)
ifeq ($(QUIC),1)
ifeq ($(TLS),0)
$(error QUIC=1 needs TLS=1: QUIC is TLS 1.3 from the same certificate store)
endif
CPP     += -DIOXD_QUIC=1 $(shell pkg-config --cflags libngtcp2 libngtcp2_crypto_ossl)
LIBS    += $(shell pkg-config --libs libngtcp2 libngtcp2_crypto_ossl)
else
CPP     += -DIOXD_QUIC=0
endif
LDFLAGS ?=
HDRS    := $(wildcard include/*.h include/ioxd/*.h lib/*/*.h)
PTHREAD := -pthread

VERSION := 0.1.0
SONAME  := libioxd.so.0

PREFIX ?= /usr/local
LIBDIR := $(PREFIX)/lib
INCDIR := $(PREFIX)/include
PCDIR  := $(LIBDIR)/pkgconfig

UNITS  := io/uring io/coro io/bufring io/conn io/proactor io/pipe clients/timer clients/socket http/engine http/api http/router http/run json/json tls/certs tls/handshake quic/quic quic/stream
OBJ    := $(addprefix obj/,$(addsuffix .o,$(UNITS))) obj/io/switch_x86_64.o obj/picohttpparser.o
PICOBJ := $(addprefix obj/pic/,$(addsuffix .o,$(UNITS))) obj/pic/io/switch_x86_64.o obj/pic/picohttpparser.o

EXAMPLE_SRC := $(wildcard playground/examples/*.c)
EXAMPLES := ioxd-hello $(patsubst playground/examples/%.c,ioxd-example-%,$(EXAMPLE_SRC))
TESTSRV  := tests/ioxd-test-server
PIPESRV  := tests/ioxd-pipe-server
UNIT     := tests/ioxd-unit
ROUTER   := tests/ioxd-router-test
# The linker version script: the .so exports ioxd_* and nothing else.
MAP      := cmake/ioxd.map

# Every flag an object is built with, in a file. TLS=0/1 - or a different CC or CFLAGS - changes
# what the objects must be, and a stamp they all depend on is what makes that a build dependency:
# the recipe rewrites it only when it differs, so an unchanged build stays untouched.
FLAGS := $(CC) $(CFLAGS) $(WARN) $(HARDEN) $(CPP) $(PTHREAD) $(LDFLAGS) $(LIBS)

.PHONY: all lib examples check check-tiny check-all tidy manual clean install uninstall force
all: lib examples

lib: libioxd.a libioxd.so

force:
obj/flags: force
	@mkdir -p $(@D)
	@printf '%s\n' '$(FLAGS)' | cmp -s - $@ || printf '%s\n' '$(FLAGS)' > $@

libioxd.a: $(OBJ)
	$(AR) rcs $@ $^

# The version script keeps the library's own names out of the dynamic symbol table: what a
# consumer may bind to is ioxd_*, and nothing else (nm -D libioxd.so says so).
libioxd.so: $(PICOBJ) $(MAP)
	$(CC) $(CFLAGS) $(LDFLAGS) -shared -Wl,-soname,$(SONAME) -Wl,--version-script,$(MAP) -o $@ $(PICOBJ) $(PTHREAD) $(LIBS)

# --- static objects (used by libioxd.a and the examples) ---
obj/%.o: lib/%.c $(HDRS) obj/flags
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(WARN) $(HARDEN) $(CPP) $(PTHREAD) -c $< -o $@
obj/io/switch_x86_64.o: lib/io/switch_x86_64.S obj/flags
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(CPP) -c $< -o $@
obj/picohttpparser.o: third_party/picohttpparser/picohttpparser.c obj/flags
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -w $(HARDEN) -Ithird_party/picohttpparser -c $< -o $@

# --- position-independent objects (used by libioxd.so) ---
obj/pic/%.o: lib/%.c $(HDRS) obj/flags
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(WARN) $(HARDEN) $(CPP) $(PTHREAD) -fPIC -c $< -o $@
obj/pic/io/switch_x86_64.o: lib/io/switch_x86_64.S obj/flags
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(CPP) -fPIC -c $< -o $@
obj/pic/picohttpparser.o: third_party/picohttpparser/picohttpparser.c obj/flags
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -w $(HARDEN) -fPIC -Ithird_party/picohttpparser -c $< -o $@

# --- examples link the static library ---
examples: $(EXAMPLES)
# Link the static archive directly so the example runs in-tree without installing the .so.
ioxd-hello: playground/hello/main.c libioxd.a
	$(CC) $(CFLAGS) $(WARN) $(HARDEN) $(CPP) $(PTHREAD) $(LDFLAGS) $< libioxd.a -o $@ $(PTHREAD) $(LIBS)
# The manual's examples, playground/examples/<name>.c, each a whole program: ioxd-example-<name>.
ioxd-example-%: playground/examples/%.c libioxd.a
	$(CC) $(CFLAGS) $(WARN) $(HARDEN) $(CPP) $(PTHREAD) $(LDFLAGS) $< libioxd.a -o $@ $(PTHREAD) $(LIBS)

# --- tests: the unit test, then the fixture server with both suites against it ---
$(TESTSRV): tests/server.c libioxd.a
	$(CC) $(CFLAGS) $(WARN) $(HARDEN) $(CPP) $(PTHREAD) $(LDFLAGS) $< libioxd.a -o $@ $(PTHREAD) $(LIBS)
$(UNIT): tests/unit.c libioxd.a
	$(CC) $(CFLAGS) $(WARN) $(HARDEN) $(CPP) $(PTHREAD) $(LDFLAGS) $< libioxd.a -o $@ $(PTHREAD) $(LIBS)
$(PIPESRV): tests/pipe-server.c libioxd.a
	$(CC) $(CFLAGS) $(WARN) $(HARDEN) $(CPP) $(PTHREAD) $(LDFLAGS) $< libioxd.a -o $@ $(PTHREAD) $(LIBS)
$(ROUTER): tests/router_test.c libioxd.a
	$(CC) $(CFLAGS) $(WARN) $(HARDEN) $(CPP) $(PTHREAD) $(LDFLAGS) $< libioxd.a -o $@ $(PTHREAD) $(LIBS)

CHECK_PORT ?= 8099
PIPE_PORT  ?= 8102                       # the fixture takes CHECK_PORT and the two after (plain, TLS)
TLS_PYTHON ?= python3                    # a python with tlslite-ng, for tests/tls_early.py (skips itself otherwise)
TLSFUZZER  ?=                            # a tlsfuzzer checkout, for `make check-tlsfuzzer` (TLS_PYTHON must have its requirements)
# The sequence itself is tests/run-suites.sh, so CMake's `check` target runs exactly this one.
check: $(TESTSRV) $(UNIT) $(PIPESRV) $(ROUTER)
	@./$(ROUTER) || exit 1; \
	 sh tests/run-suites.sh --port $(CHECK_PORT) --pipe-port $(PIPE_PORT) --quic $(QUIC) \
	    --unit ./$(UNIT) --server ./$(TESTSRV) --pipe-server ./$(PIPESRV) --tls-python $(TLS_PYTHON)

# --- the same stress suite, against a build starved on purpose ---
# 8 x 64 B receive buffers (ioxd_configure, through the fixture's environment) and a 4-deep
# per-connection queue (a build-time constant, so a second object directory): every request
# empties the buffer group, so recvs park on -ENOBUFS and are re-armed as handlers give buffers
# back, and the queue overflows at the first stall.
TINY      := -DRX_QUEUE=4                # the buffers come from the environment: ioxd_configure at run time
TINYOBJ   := $(addprefix obj-tiny/,$(addsuffix .o,$(UNITS))) obj-tiny/io/switch_x86_64.o obj-tiny/picohttpparser.o
TINYSRV   := tests/ioxd-test-server-tiny
TINY_PORT ?= 8410

obj-tiny/%.o: lib/%.c $(HDRS) obj/flags
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(WARN) $(HARDEN) $(CPP) $(TINY) $(PTHREAD) -c $< -o $@
obj-tiny/io/switch_x86_64.o: lib/io/switch_x86_64.S obj/flags
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(CPP) $(TINY) -c $< -o $@
obj-tiny/picohttpparser.o: third_party/picohttpparser/picohttpparser.c obj/flags
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -w $(HARDEN) -Ithird_party/picohttpparser -c $< -o $@
libioxd-tiny.a: $(TINYOBJ)
	$(AR) rcs $@ $^
$(TINYSRV): tests/server.c libioxd-tiny.a
	$(CC) $(CFLAGS) $(WARN) $(HARDEN) $(CPP) $(TINY) $(PTHREAD) $(LDFLAGS) $< libioxd-tiny.a -o $@ $(PTHREAD) $(LIBS)

check-tiny: $(TINYSRV)
	@IOXD_RECV_BUFFERS=8 IOXD_RECV_BUFFER_SIZE=64 sh tests/run-suites.sh --suite stress --port $(TINY_PORT) --server ./$(TINYSRV) --work obj-tiny/check

# Everything: the default build's suites, then the starved build's.
check-all: check check-tiny

# --- clang-tidy over the library with the flags its objects are built with (.clang-tidy holds the
# checks). CLion's bundled binary ships without clang's builtin headers, so gcc's own are handed
# to it: make tidy TIDY=<clion>/bin/clang/linux/x64/bin/clang-tidy ---
TIDY      ?= clang-tidy
TIDY_ARGS ?= --extra-arg=-isystem$(shell $(CC) -print-file-name=include) $(if $(filter 1,$(QUIC)),--extra-arg=-isystem$(shell pkg-config --variable=includedir libngtcp2))
TIDY_SRC  := $(wildcard lib/*/*.c) tests/server.c tests/pipe-server.c tests/unit.c
tidy:
	$(TIDY) $(TIDY_ARGS) $(TIDY_SRC) -- $(STD) $(CPP) $(PTHREAD)

# --- the manual: man-page style HTML for every public header, generated from the headers ---
manual:
	python3 manual/build.py

# --- pkg-config ---
ioxd.pc: ioxd.pc.in obj/flags
	sed -e 's|@PREFIX@|$(PREFIX)|g' -e 's|@VERSION@|$(VERSION)|g' -e 's|@LIBS@|$(LIBS)|g' $< > $@

# --- install / uninstall ---
install: lib ioxd.pc
	install -d $(DESTDIR)$(LIBDIR) $(DESTDIR)$(INCDIR)/ioxd $(DESTDIR)$(PCDIR)
	install -m644 libioxd.a $(DESTDIR)$(LIBDIR)/
	install -m755 libioxd.so $(DESTDIR)$(LIBDIR)/libioxd.so.$(VERSION)
	ln -sf libioxd.so.$(VERSION) $(DESTDIR)$(LIBDIR)/$(SONAME)
	ln -sf $(SONAME) $(DESTDIR)$(LIBDIR)/libioxd.so
	install -m644 include/ioxd.h $(DESTDIR)$(INCDIR)/
	install -m644 include/ioxd/*.h $(DESTDIR)$(INCDIR)/ioxd/
	install -m644 ioxd.pc $(DESTDIR)$(PCDIR)/
	@echo "installed ioxd $(VERSION) to $(PREFIX)"

uninstall:
	rm -f $(DESTDIR)$(LIBDIR)/libioxd.a $(DESTDIR)$(LIBDIR)/libioxd.so*
	rm -rf $(DESTDIR)$(INCDIR)/ioxd $(DESTDIR)$(INCDIR)/ioxd.h
	rm -f $(DESTDIR)$(PCDIR)/ioxd.pc

clean:
	rm -rf obj obj-tiny libioxd.a libioxd.so libioxd-tiny.a $(EXAMPLES) $(TESTSRV) $(PIPESRV) $(TINYSRV) $(UNIT) $(ROUTER) ioxd.pc

# tlsfuzzer's TLS 1.3 conformance scripts that apply to a TLS 1.3-only, one-suite server; the
# fixture must be up on CHECK_PORT with IOXD_CERTS (as `make check` runs it). Expected to pass:
# the rest of the suite probes AES-256, TLS 1.2 fallback and alerts we do not send.
TLSFUZZER_SCRIPTS := conversation zero-length-data record-padding unrecognised-groups keyshare-omitted rsa-signatures
.PHONY: check-tlsfuzzer
check-tlsfuzzer: $(TESTSRV)
	@[ -n "$(TLSFUZZER)" ] || { echo "set TLSFUZZER=<checkout> (git clone https://github.com/tlsfuzzer/tlsfuzzer)"; exit 2; }
	@[ -f tests/certs/default/cert.pem ] || sh tests/mkcerts.sh tests/certs >/dev/null
	@IOXD_WORKERS=2 IOXD_PORT=$(CHECK_PORT) IOXD_CERTS=tests/certs ./$(TESTSRV) >/dev/null 2>&1 & pid=$$!; \
	 for i in $$(seq 1 50); do ss -ltn | grep -q ":$$(($(CHECK_PORT) + 2)) " && break; sleep 0.1; done; \
	 rc=0; for t in $(TLSFUZZER_SCRIPTS); do \
	   (cd $(TLSFUZZER) && $(abspath $(TLS_PYTHON)) scripts/test-tls13-$$t.py -h 127.0.0.1 -p $$(($(CHECK_PORT) + 2)) >/dev/null 2>&1) \
	     && echo "ok   tlsfuzzer test-tls13-$$t" || { echo "FAIL tlsfuzzer test-tls13-$$t"; rc=1; }; \
	 done; kill -INT $$pid; wait $$pid 2>/dev/null; exit $$rc
