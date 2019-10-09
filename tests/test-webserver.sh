#!/bin/bash

set -euo pipefail

dir=$1

rm -f httpd-pipe
mkfifo httpd-pipe
$(dirname $0)/web-server.py "$dir" 3> httpd-pipe &
read < httpd-pipe
