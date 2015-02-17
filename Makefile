CC=gcc
CFLAGS=-I./ -I./librtmp/ -I./libhttp/ -g
OBJS=rtoh.o sysv.o http_callback.o assoc.o thread.o

rtoh:$(OBJS)
	gcc -o rtoh libhttp/http_parser.o $(OBJS) -L./librtmp/ -lrtmp -lssl -levent -lpthread -lz
rtoh.o:rtoh.h sysv.h info.h http_callback.h assoc.h thread.h
sysv.o:sysv.h
http_callback.o:http_callback.h rtoh.h
assoc.o:assoc.h rtoh.h
thread.o:thread.h rtoh.h

.PHONY:clean rtmp

clean:
	-rm -f $(OBJS) rtoh rtmp
rtmp:
	-gcc -Das_debug  -g -o rtmp thread.c -lpthread -Llibrtmp -lrtmp -lssl -lcrypto -lz -levent -I./ -I./librtmp/ -I./libhttp
