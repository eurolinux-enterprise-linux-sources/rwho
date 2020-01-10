all: ruptime

include ../MCONFIG
include ../MRULES

CFLAGS += -I../include

ruptime: ruptime.o
	$(CC) $(LDFLAGS) $^ $(LIBS) -o $@

install: ruptime
	install -s -m$(BINMODE) ruptime $(INSTALLROOT)$(BINDIR)
	install -m$(MANMODE) ruptime.1 $(INSTALLROOT)$(MANDIR)/man1/

clean:
	rm -f *.o ruptime
