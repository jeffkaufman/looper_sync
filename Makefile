all: looper_sync looper_potato

looper_sync:
	gcc -Wall -std=c99 -o looper_sync -ljack -lpthread -lrt looper_sync.c

looper_potato:
	gcc -Wall -std=c99 -o looper_potato -ljack -lpthread -lrt looper_potato.c

clean:
	rm looper_sync looper_potato *~