CC=g++
CFLAGS= -g -Wall 

all: proxy

proxy: proxy_server.cpp
	$(CC) $(CFLAGS) -o proxy_parse.o -c proxy_parse.c -lpthread
	$(CC) $(CFLAGS) -o proxy.o -c proxy_server.cpp -lpthread
	$(CC) $(CFLAGS) -o proxy proxy_parse.o proxy.o -lpthread

clean:
	rm -f proxy *.o

tar:
	tar -cvzf ass1.tgz proxy_server.cpp README Makefile proxy_parse.c proxy_parse.h