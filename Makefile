all: looper_sync looper_potato looper_rhythmpotato

looper_sync:
	gcc -Wall -std=c99 -o looper_sync -ljack -lpthread -lrt looper_sync.c

looper_potato:
	gcc -Wall -std=c99 -o looper_potato -ljack -lpthread -lrt looper_potato.c

looper_rhythmpotato:
	gcc -Wall -std=c99 -o looper_rhythmpotato -ljack -lpthread -lrt looper_rhythmpotato.c

clean:
	rm looper_sync looper_potato looper_rhythmpotato *~