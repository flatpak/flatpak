#!/bin/bash
#
# Copyright (C) 2011 Colin Walters <walters@verbum.org>
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the
# Free Software Foundation, Inc., 59 Temple Place - Suite 330,
# Boston, MA 02111-1307, USA.

set -euo pipefail

. $(dirname $0)/libtest.sh

skip_without_bwrap
skip_without_user_xattrs
skip_without_python2

echo "1..1"

setup_repo
install_repo
setup_python2_repo
install_python2_repo

# Need /var/tmp cwd for xattrs
REPO=`pwd`/repo
cd $TEST_DATA_DIR/

cp $(dirname $0)/org.test.Python.json .
cp -a $(dirname $0)/empty-configure .
cp -a $(dirname $0)/testpython.py .
cp $(dirname $0)/importme.py .
cp $(dirname $0)/importme2.py .
chmod u+w *.py
flatpak-builder --force-clean appdir org.test.Python.json

assert_has_file appdir/files/bin/importme.pyc

flatpak-builder --run appdir org.test.Python.json testpython.py > testpython.out

assert_file_has_content testpython.out ^modified$

echo "ok handled pyc rewriting multiple times"
