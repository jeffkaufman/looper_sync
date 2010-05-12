looper_sync:
	gcc -std=c99 -o looper_sync -ljack -lpthread -lrt looper_sync.c

looper_potato:
	gcc -std=c99 -o looper_potato -ljack -lpthread -lrt looper_potato.c

all: looper_sync looper_potato

clean:
	rm looper_sync *~