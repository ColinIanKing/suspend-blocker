#!/usr/bin/python
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
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA
#
import json
import sys
import os

debug = False

def parse_wakelock_data(data):
    if debug:
        print "Parsing results from wakelocks.."
        print "Test duration (seconds): %f" % data['duration-seconds']

    if data['duration-seconds'] < 60:
        print "FAILED: %s test duration was less than 60 seconds: %f" % data['duration-seconds']
    for wl in data['wakelocks']:
        if debug:
            print "Wakelock: %s" % wl['wakelock']
            print "  Active count (per second): %f" % wl['active_count_per_second']
            print "  Count (per second)       : %f" % wl['count_per_second']
            print "  Expire count (per second): %f" % wl['expire_count_per_second']
            print "  Wakeup count (per second): %f" % wl['wakeup_count_per_second']
            print "  Total time (%%)           : %f" % wl['total_time_percent']
            print "  Sleep time (%%)           : %f" % wl['sleep_time_percent']
            print "  Prevent time (%%)         : %f" % wl['prevent_time_percent']
            print " "

        if wl['total_time_percent'] > 5.0:
            print "FAILED: %s total time too large: %f %%" % (wl['wakelock'], wl['total_time_percent'])

def parse_wakelock_klog(data):
    if debug:
        print "Parsing results from klog analysis.."
    #
    #  We can have one or more kernel logs to parse
    #
    for kernlog in data:
        if debug:
           print "Kernel log: %s" % kernlog['kernel-log']
           print "Suspends attempted                      : %d" % kernlog['suspends-attempted']
           print "Suspends aborted                        : %d" % kernlog['suspends-aborted']
           print "Suspends succeeded                      : %d" % kernlog['suspends-succeeded']
           print "Suspends aborted (%%)                    : %f" % kernlog['suspends-aborted-percent']
           print "Suspends succeeded (%%)                  : %f" % kernlog['suspends-succeeded-percent']
           print "Suspends total time (%%)                 : %f" % kernlog['suspends-total-time-percent']
           print "Suspend maximum duration (seconds)      : %f" % kernlog['suspend-maximum-duration-seconds']
           print "Awake maximum duration (seconds)        : %f" % kernlog['awake-maximum-duration-seconds']

        if kernlog['suspends-aborted-percent'] > 25:
            print "FAILED: %s: aborted suspends too large: %f" % (kernlog['kernel-log'], kernlog['suspends-aborted-percent'])
        if kernlog['suspends-succeeded-percent'] < 75:
            print "FAILED: %s: succeeded suspends too small: %f" % (kernlog['kernel-log'], kernlog['suspends-succeeded-percent'])
        if kernlog['suspends-attempted'] < 1:
            print "FAILED: %s: did not attempt to suspend, this is unexpected." % kernlog['kernel-log']
        if kernlog['suspends-succeeded'] < 1:
            print "FAILED: %s: did not succeed any suspends, most probably blocked by a wakelock." % kernlog['kernel-log']
        if kernlog['suspend-maximum-duration-seconds'] < 30:
            print "FAILED: %s: maximum duration of suspend was %f seconds, way too short." % (kernlog['kernel-log'], kernlog['suspend-maximum-duration-seconds'])
        if kernlog['awake-maximum-duration-seconds'] > 10:
            print "FAILED: %s: maximum duration of awake time was %f seconds, way too long." % (kernlog['kernel-log'], kernlog['awake-maximum-duration-seconds'])
    

def main(file):
    with open(file) as f:
        data = json.load(f)

    if data.has_key('wakelock-data'):
        parse_wakelock_data(data['wakelock-data'])
    if data.has_key('wakelock-stats-from-klog'):
        parse_wakelock_klog(data['wakelock-stats-from-klog'])


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.stderr.write("Usage: %s jsonfile\n" % sys.argv[0])
        exit(1)

    main(sys.argv[1])
