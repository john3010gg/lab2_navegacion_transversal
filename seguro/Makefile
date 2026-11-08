CC      = gcc
CFLAGS  = -std=gnu11 -O2 -Wall -Wextra -pthread -D_FORTIFY_SOURCE=2 -fstack-protector-strong -fPIE
LDFLAGS = -pie -Wl,-z,relro,-z,now

server_seguro: server_seguro.c
	$(CC) $(CFLAGS) server_seguro.c -o server_seguro $(LDFLAGS)

clean:
	rm -f server_seguro

.PHONY: clean
