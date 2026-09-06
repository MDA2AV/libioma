# ioma - a static and shared library plus the playground examples that link it.
#
#   make            build libioma.a, libioma.so and the examples
#   make lib        just the libraries
#   make install    install libs, headers (under <prefix>/include/ioma) and ioma.pc
#   sudo make install PREFIX=/usr/local
#
# Downstream then builds with:  cc app.c $(pkg-config --cflags --libs ioma) -o app

CC      ?= gcc
AR      ?= ar
CFLAGS  ?= -O2 -g
WARN    := -Wall -Wextra -std=gnu11
CPP     := -Iinclude -Ithird_party/picohttpparser
PTHREAD := -pthread

VERSION := 0.1.0
SONAME  := libioma.so.0

PREFIX ?= /usr/local
LIBDIR := $(PREFIX)/lib
INCDIR := $(PREFIX)/include/ioma
PCDIR  := $(LIBDIR)/pkgconfig

UNITS  := uring coro proactor http router
OBJ    := $(addprefix obj/,$(addsuffix .o,$(UNITS))) obj/switch_x86_64.o obj/picohttpparser.o
PICOBJ := $(addprefix obj/pic/,$(addsuffix .o,$(UNITS))) obj/pic/switch_x86_64.o obj/pic/picohttpparser.o

EXAMPLES := ioma-hello

.PHONY: all lib examples clean install uninstall
all: lib examples

lib: libioma.a libioma.so

libioma.a: $(OBJ)
	$(AR) rcs $@ $^

libioma.so: $(PICOBJ)
	$(CC) -shared -Wl,-soname,$(SONAME) -o $@ $^ $(PTHREAD)

# --- static objects (used by libioma.a and the examples) ---
obj/%.o: src/%.c include/*.h | obj
	$(CC) $(CFLAGS) $(WARN) $(CPP) $(PTHREAD) -c $< -o $@
obj/switch_x86_64.o: src/switch_x86_64.S | obj
	$(CC) $(CFLAGS) $(CPP) -c $< -o $@
obj/picohttpparser.o: third_party/picohttpparser/picohttpparser.c | obj
	$(CC) $(CFLAGS) -w -Ithird_party/picohttpparser -c $< -o $@

# --- position-independent objects (used by libioma.so) ---
obj/pic/%.o: src/%.c include/*.h | obj/pic
	$(CC) $(CFLAGS) $(WARN) $(CPP) $(PTHREAD) -fPIC -c $< -o $@
obj/pic/switch_x86_64.o: src/switch_x86_64.S | obj/pic
	$(CC) $(CFLAGS) $(CPP) -fPIC -c $< -o $@
obj/pic/picohttpparser.o: third_party/picohttpparser/picohttpparser.c | obj/pic
	$(CC) $(CFLAGS) -w -fPIC -Ithird_party/picohttpparser -c $< -o $@

obj obj/pic:
	mkdir -p $@

# --- examples link the static library ---
examples: $(EXAMPLES)
# Link the static archive directly so the example runs in-tree without installing the .so.
ioma-hello: playground/hello/main.c libioma.a
	$(CC) $(CFLAGS) $(WARN) $(CPP) $(PTHREAD) $< libioma.a -o $@ $(PTHREAD)

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
	install -m644 include/ioma.h include/http.h include/proactor.h include/coro.h include/uring.h $(DESTDIR)$(INCDIR)/
	install -m644 ioma.pc $(DESTDIR)$(PCDIR)/
	@echo "installed ioma $(VERSION) to $(PREFIX)"

uninstall:
	rm -f $(DESTDIR)$(LIBDIR)/libioma.a $(DESTDIR)$(LIBDIR)/libioma.so*
	rm -rf $(DESTDIR)$(INCDIR)
	rm -f $(DESTDIR)$(PCDIR)/ioma.pc

clean:
	rm -rf obj libioma.a libioma.so $(EXAMPLES) ioma.pc
