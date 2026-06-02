#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Print the minimum supported version of the given tool.

case "$1" in
binutils)
	echo 2.23.0
	;;
gcc)
	echo 4.9.0
	;;
llvm)
	echo 10.0.1
	;;
pahole)
	echo 1.16
	;;
*)
	echo "$1: unknown tool" >&2
	exit 1
	;;
esac
