CC = gcc
LD = gcc
CFLAGS = -g -Wall -I/usr/local/include -I/opt/local/include
LDFLAGS = -L/usr/local/lib/event2 -L/opt/local/lib/event2 -L/opt/local/lib -L/usr/local/lib -g
RM = /bin/rm -f

LIBS = -levent_core -lrabbitmq -ljson -lz

OBJS = logenqueue.o config.o
PROG = logenqueue

all: $(PROG)

$(PROG): $(OBJS)
		$(LD) $(LDFLAGS) $(OBJS) $(LIBS) -o $(PROG)

%.o: %.c
		$(CC) $(CFLAGS) -c $<

clean:
		$(RM) $(PROG) $(OBJS)
