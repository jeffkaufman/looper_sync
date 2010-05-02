looper_sync:
	gcc -std=c99 -o looper_sync -ljack -lpthread -lrt looper_sync.c

all: looper_sync

clean:
	rm looper_sync *~