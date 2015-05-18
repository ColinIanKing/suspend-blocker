#
# Copyright (C) 2013-2015 Canonical, Ltd.
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
#

VERSION=0.01.24

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
