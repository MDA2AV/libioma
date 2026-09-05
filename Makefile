CC      = gcc
CFLAGS  = -O2 -g -Wall -Wextra -std=gnu11 -pthread -Iinclude
LDFLAGS = -pthread

SRCS = src/uring.c src/coro.c src/proactor.c src/main.c
ASMS = src/switch_x86_64.S
OBJS = $(SRCS:src/%.c=obj/%.o) $(ASMS:src/%.S=obj/%.o)

ioma: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

obj/%.o: src/%.c include/*.h | obj
	$(CC) $(CFLAGS) -c $< -o $@

obj/%.o: src/%.S | obj
	$(CC) $(CFLAGS) -c $< -o $@

obj:
	mkdir -p obj

clean:
	rm -rf obj ioma

.PHONY: clean
