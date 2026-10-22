#!/bin/sh
#
# Copyright (c) 2026, Renaud Allard <renaud@allard.it>
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#
# 1. Redistributions of source code must retain the above copyright notice,
#    this list of conditions and the following disclaimer.
#
# 2. Redistributions in binary form must reproduce the above copyright notice,
#    this list of conditions and the following disclaimer in the documentation
#    and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
# LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
# CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
# SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
# INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
# CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
# ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
# POSSIBILITY OF SUCH DAMAGE.
#

# Install vmnotify and its manual page from a release package.

set -e

case $# in
0)	prefix=/usr/local ;;
1)	prefix=$1 ;;
*)	prefix=- ;;
esac
case $prefix in
/*)	;;
*)	echo "usage: install.sh [prefix], prefix is /usr/local by default" >&2
	exit 1 ;;
esac

cd "$(dirname "$0")"
install -d "$prefix/bin" "$prefix/share/man/man1"
install -m 755 vmnotify "$prefix/bin/vmnotify"
install -m 644 vmnotify.1 "$prefix/share/man/man1/vmnotify.1"
echo "installed $prefix/bin/vmnotify and $prefix/share/man/man1/vmnotify.1"

# needed at run time, not to install
for cmd in tesseract notify-send; do
	command -v $cmd > /dev/null ||
	    echo "warning: $cmd not found, install it (see README.md)" >&2
done
