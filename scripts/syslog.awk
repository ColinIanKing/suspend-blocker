#!/usr/bin/awk -f
#
# Copyright (C) 2015 Canonical, Ltd.
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

BEGIN {
	m_min = 1440
	m_max = 0
}

{
	t=$3
	h=substr(t, 0, 2)
	m=substr(t, 4, 2)
	mins=(h * 60) + m

	if (mins > m_max)
		m_max = mins
	if (mins < m_min)
		m_min = mins
	x=index($0, "wake up by");
	if (x > 0) {
		s=substr($0, x + 11)
		y=index(s, ",")
		if (y > 1) {
			wakeup = substr(s, 0, y - 1)
			wakeups[wakeup]++
			event[mins][wakeup]++
			times[mins]++
			time[mins] = substr(t, 0, 5)
			#print "-->", mins, wakeup, event[mins][wakeup]
		}
	}
}

END {	
	printf "Time HH:MM\t"
	for (w in wakeups)
		printf "%s\t", w
	printf "\n"

	for (m = m_min; m < m_max; m++) {
		if (times[m] > 0) {
			printf "%s\t", time[m]
			for (w in wakeups) {
				if (event[m][w] > 0)
					printf "%d\t", event[m][w]
				else
					printf "0\t"
			}
			printf "\n"
		}
	}
}
	
