all: lib test

#GCC=aarch64-apple-darwin22-gcc-13
GCC=gcc -arch x86_64

lib: shm_queue.c shm_queue.h opt_time.h
	$(GCC) -o libshmqueue.so shm_queue.c -shared -fPIC

test: lib test.c shm_queue.h
	$(GCC) -o test test.c -L./ -lshmqueue -g

clean:
	rm -f test libshmqueue.so
