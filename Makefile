CC = gcc
LD = gcc
CFLAGS = -std=c99 -g -Wall -I/usr/local/include -I/opt/local/include -O2
LDFLAGS = -L/opt/local/lib -L/usr/local/lib -g
RM = /bin/rm -f

LIBS = -lrabbitmq -lz -lpthread

OBJS = logenqueue.o config.o
PROG = logenqueue

all: $(PROG)

$(PROG): $(OBJS)
		$(LD) $(LDFLAGS) $(OBJS) $(LIBS) -o $(PROG)

%.o: %.c
		$(CC) $(CFLAGS) -c $<

clean:
		$(RM) $(PROG) $(OBJS)
