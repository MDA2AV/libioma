# ioxd - a static and shared library plus the playground examples that link it.
#
#   make            build libioxd.a, libioxd.so and the examples
#   make lib        just the libraries
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
CPP     := -D_GNU_SOURCE -Iinclude -Ilib -Ithird_party/picohttpparser
HDRS    := $(wildcard include/*.h include/ioxd/*.h lib/*/*.h)
PTHREAD := -pthread

VERSION := 0.1.0
SONAME  := libioxd.so.0

PREFIX ?= /usr/local
LIBDIR := $(PREFIX)/lib
INCDIR := $(PREFIX)/include
PCDIR  := $(LIBDIR)/pkgconfig

UNITS  := io/uring io/coro io/bufring io/conn io/proactor io/pipe http/engine http/api http/router http/run json/json
OBJ    := $(addprefix obj/,$(addsuffix .o,$(UNITS))) obj/io/switch_x86_64.o obj/picohttpparser.o
PICOBJ := $(addprefix obj/pic/,$(addsuffix .o,$(UNITS))) obj/pic/io/switch_x86_64.o obj/pic/picohttpparser.o

EXAMPLES := ioxd-hello
TESTSRV  := tests/ioxd-test-server
PIPESRV  := tests/ioxd-pipe-server
UNIT     := tests/ioxd-unit

.PHONY: all lib examples check clean install uninstall
all: lib examples

lib: libioxd.a libioxd.so

libioxd.a: $(OBJ)
	$(AR) rcs $@ $^

libioxd.so: $(PICOBJ)
	$(CC) $(CFLAGS) -shared -Wl,-soname,$(SONAME) -o $@ $^ $(PTHREAD)

# --- static objects (used by libioxd.a and the examples) ---
obj/%.o: lib/%.c $(HDRS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(WARN) $(CPP) $(PTHREAD) -c $< -o $@
obj/io/switch_x86_64.o: lib/io/switch_x86_64.S
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(CPP) -c $< -o $@
obj/picohttpparser.o: third_party/picohttpparser/picohttpparser.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -w -Ithird_party/picohttpparser -c $< -o $@

# --- position-independent objects (used by libioxd.so) ---
obj/pic/%.o: lib/%.c $(HDRS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(WARN) $(CPP) $(PTHREAD) -fPIC -c $< -o $@
obj/pic/io/switch_x86_64.o: lib/io/switch_x86_64.S
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(CPP) -fPIC -c $< -o $@
obj/pic/picohttpparser.o: third_party/picohttpparser/picohttpparser.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -w -fPIC -Ithird_party/picohttpparser -c $< -o $@

# --- examples link the static library ---
examples: $(EXAMPLES)
# Link the static archive directly so the example runs in-tree without installing the .so.
ioxd-hello: playground/hello/main.c libioxd.a
	$(CC) $(CFLAGS) $(WARN) $(CPP) $(PTHREAD) $< libioxd.a -o $@ $(PTHREAD)

# --- tests: the unit test, then the fixture server with both suites against it ---
$(TESTSRV): tests/server.c libioxd.a
	$(CC) $(CFLAGS) $(WARN) $(CPP) $(PTHREAD) $< libioxd.a -o $@ $(PTHREAD)
$(UNIT): tests/unit.c libioxd.a
	$(CC) $(CFLAGS) $(WARN) $(CPP) $(PTHREAD) $< libioxd.a -o $@ $(PTHREAD)
$(PIPESRV): tests/pipe-server.c libioxd.a
	$(CC) $(CFLAGS) $(WARN) $(CPP) $(PTHREAD) $< libioxd.a -o $@ $(PTHREAD)

CHECK_PORT ?= 8099
PIPE_PORT  ?= 8100
check: $(TESTSRV) $(UNIT) $(PIPESRV)
	@./$(UNIT) || exit 1; \
	 IOXD_WORKERS=2 IOXD_PORT=$(CHECK_PORT) ./$(TESTSRV) >/dev/null 2>&1 & pid=$$!; \
	 for i in $$(seq 1 50); do ss -ltn | grep -q ":$(CHECK_PORT) " && break; sleep 0.1; done; \
	 python3 tests/smoke.py $(CHECK_PORT); s=$$?; python3 tests/stress.py $(CHECK_PORT); t=$$?; \
	 kill -INT $$pid; wait $$pid 2>/dev/null; \
	 ./$(PIPESRV) $(PIPE_PORT) >/dev/null 2>&1 & pid=$$!; \
	 for i in $$(seq 1 50); do ss -ltn | grep -q ":$(PIPE_PORT) " && break; sleep 0.1; done; \
	 python3 tests/pipes.py $(PIPE_PORT); u=$$?; \
	 kill -INT $$pid; wait $$pid 2>/dev/null; exit $$((s | t | u))

# --- pkg-config ---
ioxd.pc: ioxd.pc.in
	sed -e 's|@PREFIX@|$(PREFIX)|g' -e 's|@VERSION@|$(VERSION)|g' $< > $@

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
	rm -rf obj libioxd.a libioxd.so $(EXAMPLES) $(TESTSRV) $(PIPESRV) $(UNIT) ioxd.pc
