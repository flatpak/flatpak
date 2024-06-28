#!/bin/env python3
#
# Copyright © 2024 GNOME Foundation, Inc.
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library. If not, see <http://www.gnu.org/licenses/>.
#
# Authors:
#       Hubert Figuière <hub@figuiere.net>
#
# Convert the output of libgphoto2 print-camera-list.

import subprocess
import re

device_match = re.compile(r"^([\dabcdef]*):([\dabcdef]*)")
file = subprocess.Popen(['./packaging/generic/print-camera-list', 'idlist'], stdout=subprocess.PIPE).stdout

vendor = 0
while True:
    line = file.readline()
    if not line:
        break
    line = line.decode('utf-8')
    m = device_match.match(line)
    if m is not None:
        print("--usb=vnd:{}+dev:{}".format(m.group(1), m.group(2)))
    print("--usb=cls:6")
