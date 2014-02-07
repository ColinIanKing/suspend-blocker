#!/bin/bash
#
# Copyright (C) 2014 Canonical, Ltd.
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

DURATION=60

if [ $EUID -ne 0 ];
then
	echo must be root to run this script
	exit 0
fi

#
# Clean kernel log, restart rsyslogd
#
rm /var/log/kern.log
kill -HUP $(cat /var/run/rsyslogd.pid)

#
#  Collect wakelock stats
#
./suspend-blocker -w $DURATION -o wakelock-stats.json
#
#  And parse kernel log for suspend reasons
#
./suspend-blocker /var/log/kern.log -r -o suspend-stats.json


