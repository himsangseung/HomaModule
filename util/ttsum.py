#!/usr/bin/python3

# Copyright (c) 2019-2022 Homa Developers
# SPDX-License-Identifier: BSD-1-Clause

"""
This program reads one or more timetrace logs and generates summary
information. Use the --help option to print usage information.
"""

from collections import defaultdict
from glob import glob
from optparse import OptionParser
import math
import os
import re
import string
import sys

# This variable collects all the times for all events, individually. It is
# a dictionary that maps from key names to a list containing all of the
# intervals for that event name (each interval is the elapsed time between
# the previous event and this event).
eventIntervals = {}

# This variable collects information for all events relative to a given
# starting event (the --from command line option).
#
# relativeEvents:
#     dictionary (event name => OccurrenceList)
#
# The same event may sometimes happen multiple times for single occurrence
# of the starting event. An OccurrenceList is a list, where the nth entry
# describes all the events that occurred after n prior occurrences of the
# event.
# OccurrenceList:
#    list (OccurrenceInfo)
#
# OccurrenceInfo:
#    dictionary (
#        times: list()        One entry for each event: elapsed ns between
#                             the starting event and this event
#        intervals: list()    One entry for each event: elapsed ns between
#                             immediately preceding event and this event
#    )

relativeEvents = {}

# This variable contains a count of the number of times each event has
# occurred since the last time the starting event occurred.

eventCount = {}

# Core number -> time of most recent event on that core. -1 means no
# events seen for that core yet.
corePrev = defaultdict(lambda : None)

# Core number -> most recent time the "starting event" occurred on
# that core.
startTimes = defaultdict(lambda : None)

# Core number -> dictionary mapping from event string to the number
# of times that event has occurred on the given core since the starting
# event.
eventCounts = defaultdict(lambda: defaultdict(lambda: 0))

def scan(f, startingEvent):
    """
    Scan the log file given by 'f' (handle for an open file) and collect
    information from time trace records as described by the arguments.
    If 'startingEvent' isn't None, it specifies an event indicating the
    beginning of a related sequence of event; times are accumulated for all
    other events, relative to the most recent occurrence of the starting event.
    """

    lastTime = None
    for line in f:
        match = re.match(r'(^|.* )([0-9.]+) us \(\+ *([0-9.]+) us\) '
                r'\[C([0-9]+)\] (.+)', line)
        if not match:
            continue
        thisEventTime = float(match.group(2))
        core = int(match.group(4))
        thisEvent = match.group(5)
        if not options.useCores:
            core = 0
        prevTime = corePrev[core]
        if prevTime == None:
            thisEventInterval = 0
        else:
            thisEventInterval = thisEventTime - prevTime
        rawEvent = thisEvent
        if options.noNumbers:
            thisEvent = re.sub(r'\b0x[0-9a-f]+\b', '?', thisEvent)
            thisEvent = re.sub(r'\b[0-9.]+\b', '?', thisEvent)
        if (lastTime != None) and (thisEventTime < lastTime):
            print('Time went backwards at the following line:\n%s' % (line))
        lastTime = thisEventTime
        corePrev[core] = thisEventTime
        if thisEventInterval != 0.0:
            if not thisEvent in eventIntervals:
                eventIntervals[thisEvent] = []
            eventIntervals[thisEvent].append(thisEventInterval)
            # print('%s %s %s' % (thisEventTime, thisEventInterval, thisEvent))
        if startingEvent:
            if startingEvent in rawEvent:
                # Reset variables to indicate that we are starting a new
                # sequence of events from the starting event.
                startTimes[core] = thisEventTime
                eventCounts[core] = defaultdict(lambda: 0)

            startTime = startTimes[core]
            if startTime == None:
                continue

            # If we get here, it means that we have found an event that
            # is not the starting event, and startTime indicates the time of
            # the starting event. First, see how many times this event has
            # occurred since the last occurrence of the starting event.
            relativeTime = thisEventTime - startTime
            # print('%9.3f: %.1f %.1f %s' % (thisEventTime, relativeTime,
            #         thisEventInterval, thisEvent))
            count = eventCounts[core][thisEvent] + 1
            eventCounts[core][thisEvent] = count

            # print("%9.3f: count for '%s': %d" % (thisEventTime, thisEvent,
            #         count))
            if not thisEvent in relativeEvents:
                relativeEvents[thisEvent] = []
            occurrences = relativeEvents[thisEvent]
            while len(occurrences) < count:
                occurrences.append({'times': [], 'intervals': []})
            occurrences[count-1]['times'].append(relativeTime)
            occurrences[count-1]['intervals'].append(thisEventInterval)

# Parse command line options
parser = OptionParser(description=
        'Read one or more log files and summarize the time trace information '
        'present in the file(s) as specified by the arguments. If no files '
        'are given then a time trace is read from standard input.',
        usage='%prog [options] [file file ...]',
        conflict_handler='resolve')
parser.add_option('-a', '--alt', action='store_true', default=False,
        dest='altFormat',
        help='use alternate output format if -f is specified (print min, '
        'max, etc. for cumulative time, not delta)')
parser.add_option('-c', '--cores', action='store_true', default=False,
        dest='useCores',
        help='treat events on each core independently: compute elapsed time '
        'for each event relative to the previous event on the same core, and '
        'if -f is specified, compute relative times separately on each core '
        '(default: consider all events on all cores as a single stream)')
parser.add_option('-f', '--from', type='string', dest='startEvent',
        help='measure times for other events relative to FROM; FROM contains a '
        'substring of an event')
parser.add_option('-n', '--numbers', action='store_false', default=True,
        dest='noNumbers',
        help='treat numbers in event names as significant; if this flag '
        'is not specified, all numbers are replaced with ? (events will be '
        'considered the same if they differ only in numeric fields)')

(options, files) = parser.parse_args()
if len(files) == 0:
    scan(sys.stdin, options.startEvent)
else:
    for name in files:
        scan(open(name), options.startEvent)

# Print information about all events, unless --from was specified.
if not options.startEvent:
    # Do this in 2 passes. First, generate a string describing each
    # event; then sort the list of messages and print.

    # Each entry in the following variable will contain a list with
    # 2 elements: time to use for sorting, and string to print.
    outputInfo = []

    # Compute the length of the longest event name.
    nameLength = 0
    for event in eventIntervals.keys():
        nameLength = max(nameLength, len(event))

    # Each iteration through the following loop processes one event name
    for event in eventIntervals.keys():
        intervals = eventIntervals[event]
        intervals.sort()
        medianTime = intervals[len(intervals)//2]
        message = '%-*s  %6.0f %6.0f %6.0f %6.0f %7d' % (nameLength,
            event, medianTime, intervals[0], intervals[-1],
            sum(intervals)/len(intervals), len(intervals))
        outputInfo.append([medianTime, message])

    # Pass 2: sort in order of median interval length, then print.
    outputInfo.sort(key=lambda item: item[0], reverse=True)
    print('%-*s  Median    Min    Max    Avg   Count' % (nameLength,
            "Event"))
    print('%s---------------------------------------------' %
            ('-' * nameLength))
    for message in outputInfo:
        print(message[1])

# Print output for the --from option. First, process each event occurrence,
# then sort them by elapsed time from the starting event.
if options.startEvent:
    # Each entry in the following variable will contain a list with
    # 2 elements: time to use for sorting, and string to print.
    outputInfo = []

    # Compute the length of the longest event name.
    nameLength = 0
    for event in relativeEvents.keys():
        occurrences = relativeEvents[event]
        thisLength = len(event)
        if len(occurrences) > 1:
            thisLength += len(' (#%d)' % (len(occurrences)))
        nameLength = max(nameLength, thisLength)

    # Each iteration through the following loop processes one event name
    for event in relativeEvents.keys():
        occurrences = relativeEvents[event]

        # Each iteration through the following loop processes the nth
        # occurrence of this event.
        for i in range(len(occurrences)):
            eventName = event
            if i != 0:
                eventName = '%s (#%d)' % (event, i+1)
            times = occurrences[i]['times']
            intervals = occurrences[i]['intervals']
            times.sort()
            medianTime = times[len(times)//2]
            intervals.sort()
            medianInterval = intervals[len(intervals)//2]
            if options.altFormat:
                message = '%-*s  %6.0f %6.0f %6.0f %6.0f %6.0f %7d' % (
                    nameLength, eventName, medianTime*1e03, times[0]*1e03,
                    times[-1]*1e03, sum(times)*1e03/len(times),
                    intervals[len(intervals)//2]*1e03, len(times))
            else:
                message = '%-*s  %6.0f %6.0f %6.0f %6.0f %6.0f %7d' % (
                    nameLength, eventName, medianTime*1e03, medianInterval*1e03,
                    intervals[0]*1e03, intervals[-1]*1e03,
                    sum(intervals)/len(intervals)*1e03, len(intervals))
            outputInfo.append([medianTime, message])

    outputInfo.sort(key=lambda item: item[0])
    if options.altFormat:
        print('%-*s  Median    Min    Max    Avg  Delta   Count' % (nameLength,
                "Event"))
        print('%s--------------------------------------------' %
                ('-' * nameLength))
    else:
        print('%-*s   Cum.  ---------------Delta---------------' %
                (nameLength, ""))
        print('%-*s  Median Median    Min    Max    Avg   Count' %
                (nameLength, "Event"))
        print('%s--------------------------------------------' %
                ('-' * nameLength))
    for message in outputInfo:
        print(message[1])