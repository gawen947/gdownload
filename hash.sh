#!/bin/sh
# script to determine git hash of current source tree
hash=$(git log 2>/dev/null | head -n1 2>/dev/null | sed "s/.* //" 2>/dev/null)
if [ x"$hash" != x ]
then
	echo $hash
elif [ "$FROM_ARCHIVE" != ':%H$' ]
then
	echo $FROM_ARCHIVE
else
	echo "commit hash detection fail.  Dear packager, please figure out what goes wrong or get in touch with us" >&2
	echo unknown
	exit 2
fi
exit 0
