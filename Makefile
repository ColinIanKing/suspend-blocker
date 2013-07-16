VERSION=0.01.09

CFLAGS += -Wall -Wextra -DVERSION='"$(VERSION)"' -g

BINDIR=/usr/bin
MANDIR=/usr/share/man/man8

suspend-blocker: suspend-blocker.o
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

suspend-blocker.8.gz: suspend-blocker.8
	gzip -c $< > $@

dist:
	git archive --format=tar --prefix="suspend-blocker-$(VERSION)/" V$(VERSION) | \
		gzip > suspend-blocker-$(VERSION).tar.gz

clean:
	rm -f suspend-blocker suspend-blocker.o suspend-blocker.8.gz
	rm -f suspend-blocker-$(VERSION).tar.gz

install: suspend-blocker suspend-blocker.8.gz
	mkdir -p ${DESTDIR}${BINDIR}
	cp suspend-blocker ${DESTDIR}${BINDIR}
	mkdir -p ${DESTDIR}${MANDIR}
	cp suspend-blocker.8.gz ${DESTDIR}${MANDIR}
