all: rwhod

include ../MCONFIG
include ../MRULES

CFLAGS += -I../include
OBJS = rwhod.o daemon.o

rwhod: $(OBJS)
	$(CC) $(LDFLAGS) $^ $(LIBS) -o $@

$(OBJS): daemon.h

install: rwhod
	install -s -m$(DAEMONMODE) rwhod $(INSTALLROOT)$(SBINDIR)
	install -m$(MANMODE) rwhod.8 $(INSTALLROOT)$(MANDIR)/man8/

clean:
	rm -f *.o rwhod

