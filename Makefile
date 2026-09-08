# ioma - a static and shared library plus the playground examples that link it.
#
#   make            build libioma.a, libioma.so and the examples
#   make lib        just the libraries
#   make install    install libs, headers (under <prefix>/include/ioma) and ioma.pc
#   sudo make install PREFIX=/usr/local
#
# Downstream then builds with:  cc app.c $(pkg-config --cflags --libs ioma) -o app

# The compiler: unless CC is given, the newest gcc on the PATH (a distro's `gcc` is often older
# than a `gcc-NN` installed beside it). The code is C23, which needs gcc 14 or newer.
ifeq ($(origin CC),default)
CC := $(shell for c in gcc gcc-14 gcc-15 gcc-16; do command -v $$c >/dev/null 2>&1 && echo "$$($$c -dumpfullversion) $$c"; done | sort -V | tail -1 | cut -d' ' -f2)
CC := $(if $(CC),$(CC),gcc)
endif
STD     := $(shell $(CC) -std=gnu23 -x c -c /dev/null -o /dev/null 2>/dev/null && echo -std=gnu23)
ifeq ($(STD),)
$(error $(CC) does not know -std=gnu23: libioma is C23 and needs gcc 14 or newer (make CC=gcc-14))
endif
AR      ?= ar
# Fat LTO objects when the compiler supports them: the archive stays linkable by anyone (it also
# carries plain machine code), and a consumer that links with -flto gets cross-file inlining -
# including its own handlers into the engine. Measured ~+2% on saturated throughput.
LTO     := $(shell $(CC) -Werror -flto -ffat-lto-objects -x c -c /dev/null -o /dev/null 2>/dev/null && echo -flto -ffat-lto-objects)
CFLAGS  ?= -O3 -g $(LTO)
WARN    := -Wall -Wextra $(STD)
CPP     := -D_GNU_SOURCE -Iinclude -Isrc -Ithird_party/picohttpparser
HDRS    := $(wildcard include/*.h src/*/*.h)
PTHREAD := -pthread

VERSION := 0.1.0
SONAME  := libioma.so.0

PREFIX ?= /usr/local
LIBDIR := $(PREFIX)/lib
INCDIR := $(PREFIX)/include/ioma
PCDIR  := $(LIBDIR)/pkgconfig

UNITS  := io/uring io/coro io/bufring io/conn io/proactor io/pipe http/engine http/api http/router http/run
OBJ    := $(addprefix obj/,$(addsuffix .o,$(UNITS))) obj/io/switch_x86_64.o obj/picohttpparser.o
PICOBJ := $(addprefix obj/pic/,$(addsuffix .o,$(UNITS))) obj/pic/io/switch_x86_64.o obj/pic/picohttpparser.o

EXAMPLES := ioma-hello
TESTSRV  := tests/ioma-test-server
PIPESRV  := tests/ioma-pipe-server
UNIT     := tests/ioma-unit

.PHONY: all lib examples check clean install uninstall
all: lib examples

lib: libioma.a libioma.so

libioma.a: $(OBJ)
	$(AR) rcs $@ $^

libioma.so: $(PICOBJ)
	$(CC) $(CFLAGS) -shared -Wl,-soname,$(SONAME) -o $@ $^ $(PTHREAD)

# --- static objects (used by libioma.a and the examples) ---
obj/%.o: src/%.c $(HDRS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(WARN) $(CPP) $(PTHREAD) -c $< -o $@
obj/io/switch_x86_64.o: src/io/switch_x86_64.S
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(CPP) -c $< -o $@
obj/picohttpparser.o: third_party/picohttpparser/picohttpparser.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -w -Ithird_party/picohttpparser -c $< -o $@

# --- position-independent objects (used by libioma.so) ---
obj/pic/%.o: src/%.c $(HDRS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(WARN) $(CPP) $(PTHREAD) -fPIC -c $< -o $@
obj/pic/io/switch_x86_64.o: src/io/switch_x86_64.S
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(CPP) -fPIC -c $< -o $@
obj/pic/picohttpparser.o: third_party/picohttpparser/picohttpparser.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -w -fPIC -Ithird_party/picohttpparser -c $< -o $@

# --- examples link the static library ---
examples: $(EXAMPLES)
# Link the static archive directly so the example runs in-tree without installing the .so.
ioma-hello: playground/hello/main.c libioma.a
	$(CC) $(CFLAGS) $(WARN) $(CPP) $(PTHREAD) $< libioma.a -o $@ $(PTHREAD)

# --- tests: the unit test, then the fixture server with both suites against it ---
$(TESTSRV): tests/server.c libioma.a
	$(CC) $(CFLAGS) $(WARN) $(CPP) $(PTHREAD) $< libioma.a -o $@ $(PTHREAD)
$(UNIT): tests/unit.c libioma.a
	$(CC) $(CFLAGS) $(WARN) $(CPP) $(PTHREAD) $< libioma.a -o $@ $(PTHREAD)
$(PIPESRV): tests/pipe-server.c libioma.a
	$(CC) $(CFLAGS) $(WARN) $(CPP) $(PTHREAD) $< libioma.a -o $@ $(PTHREAD)

CHECK_PORT ?= 8099
PIPE_PORT  ?= 8100
check: $(TESTSRV) $(UNIT) $(PIPESRV)
	@./$(UNIT) || exit 1; \
	 IOMA_WORKERS=2 IOMA_PORT=$(CHECK_PORT) ./$(TESTSRV) >/dev/null 2>&1 & pid=$$!; \
	 for i in $$(seq 1 50); do ss -ltn | grep -q ":$(CHECK_PORT) " && break; sleep 0.1; done; \
	 python3 tests/smoke.py $(CHECK_PORT); s=$$?; python3 tests/stress.py $(CHECK_PORT); t=$$?; \
	 kill -INT $$pid; wait $$pid 2>/dev/null; \
	 ./$(PIPESRV) $(PIPE_PORT) >/dev/null 2>&1 & pid=$$!; \
	 for i in $$(seq 1 50); do ss -ltn | grep -q ":$(PIPE_PORT) " && break; sleep 0.1; done; \
	 python3 tests/pipes.py $(PIPE_PORT); u=$$?; \
	 kill -INT $$pid; wait $$pid 2>/dev/null; exit $$((s | t | u))

# --- pkg-config ---
ioma.pc: ioma.pc.in
	sed -e 's|@PREFIX@|$(PREFIX)|g' -e 's|@VERSION@|$(VERSION)|g' $< > $@

# --- install / uninstall ---
install: lib ioma.pc
	install -d $(DESTDIR)$(LIBDIR) $(DESTDIR)$(INCDIR) $(DESTDIR)$(PCDIR)
	install -m644 libioma.a $(DESTDIR)$(LIBDIR)/
	install -m755 libioma.so $(DESTDIR)$(LIBDIR)/libioma.so.$(VERSION)
	ln -sf libioma.so.$(VERSION) $(DESTDIR)$(LIBDIR)/$(SONAME)
	ln -sf $(SONAME) $(DESTDIR)$(LIBDIR)/libioma.so
	install -m644 include/ioma.h $(DESTDIR)$(INCDIR)/
	install -m644 ioma.pc $(DESTDIR)$(PCDIR)/
	@echo "installed ioma $(VERSION) to $(PREFIX)"

uninstall:
	rm -f $(DESTDIR)$(LIBDIR)/libioma.a $(DESTDIR)$(LIBDIR)/libioma.so*
	rm -rf $(DESTDIR)$(INCDIR)
	rm -f $(DESTDIR)$(PCDIR)/ioma.pc

clean:
	rm -rf obj libioma.a libioma.so $(EXAMPLES) $(TESTSRV) $(PIPESRV) $(UNIT) ioma.pc
