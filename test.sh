#!/bin/bash

for I in *.klog
do
	./suspend-blocker -v -b -r < $I > /tmp/$I.output
	diff $I.output /tmp/$I.output
	if [ $? -eq 0 ]; then
		echo "$I: PASSED"
	else
		echo "$I: FAILED"
	fi
done
