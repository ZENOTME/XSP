CC = gcc
CFLAGS = -g

all: simple_test per_thread_test

simple_test: simple_test.c
	$(CC) $(CFLAGS) -o simple_test simple_test.c

per_thread_test: per_thread_test.c
	$(CC) $(CFLAGS) -o per_thread_test per_thread_test.c

clean:
	rm -f simple_test per_thread_test
