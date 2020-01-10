all: rwho

include ../MCONFIG
include ../MRULES

CFLAGS += -I../include

rwho: rwho.o
	$(CC) $(LDFLAGS) $^ $(LIBS) -o $@

rwho.o: ../version.h

install: rwho
	install -s -m$(BINMODE) rwho $(INSTALLROOT)$(BINDIR)
	install -m$(MANMODE) rwho.1 $(INSTALLROOT)$(MANDIR)/man1/

clean:
	rm -f *.o rwho

