VERSION=0.01.22

CFLAGS += -Wall -Wextra -DVERSION='"$(VERSION)"' -g
LDFLAGS += -ljson -lm

BINDIR=/usr/bin
MANDIR=/usr/share/man/man8

suspend-blocker: suspend-blocker.o
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

suspend-blocker.8.gz: suspend-blocker.8
	gzip -c $< > $@

dist:
	rm -rf suspend-blocker-$(VERSION)
	mkdir suspend-blocker-$(VERSION)
	cp -rp Makefile suspend-blocker.c suspend-blocker.8 COPYING suspend-blocker-$(VERSION)
	tar -zcf suspend-blocker-$(VERSION).tar.gz suspend-blocker-$(VERSION)
	rm -rf suspend-blocker-$(VERSION)

clean:
	rm -f suspend-blocker suspend-blocker.o suspend-blocker.8.gz
	rm -f suspend-blocker-$(VERSION).tar.gz

install: suspend-blocker suspend-blocker.8.gz
	mkdir -p ${DESTDIR}${BINDIR}
	cp suspend-blocker ${DESTDIR}${BINDIR}
	mkdir -p ${DESTDIR}${MANDIR}
	cp suspend-blocker.8.gz ${DESTDIR}${MANDIR}
