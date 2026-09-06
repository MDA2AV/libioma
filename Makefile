CC      = gcc
CFLAGS  = -O2 -g -Wall -Wextra -std=gnu11 -pthread -Iinclude -Ithird_party/picohttpparser
LDFLAGS = -pthread

SRCS   = src/uring.c src/coro.c src/proactor.c src/http.c src/router.c src/main.c
ASMS   = src/switch_x86_64.S
VENDOR = third_party/picohttpparser/picohttpparser.c
OBJS   = $(SRCS:src/%.c=obj/%.o) $(ASMS:src/%.S=obj/%.o) obj/picohttpparser.o

ioma: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

obj/%.o: src/%.c include/*.h | obj
	$(CC) $(CFLAGS) -c $< -o $@

obj/%.o: src/%.S | obj
	$(CC) $(CFLAGS) -c $< -o $@

# Vendored parser: built without -Wextra so its SSE tricks don't spam the build.
obj/picohttpparser.o: $(VENDOR) | obj
	$(CC) -O2 -g -std=gnu11 -Ithird_party/picohttpparser -c $< -o $@

obj:
	mkdir -p obj

clean:
	rm -rf obj ioma

.PHONY: clean
