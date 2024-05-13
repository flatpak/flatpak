#!/bin/bash
#
# Copyright (C) 2021 Alexander Larsson <alexl@redhat.com>
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

echo "1..5"

create_commit() {
    # Wrap this to avoid set -x showing the commands
    { { local BASH_XTRACEFD=3; } 2> /dev/null
        local REPO=${1}
        local APP=${2}
        local DEPTH=${3}

        local F=$(mktemp -d files.XXXXXX)

        # Each commit has:
        #  * 2 unique file objects
        #  * 2 app shared file objects
        #  * 2 unique dirtree object
        #  * 1 app shared dirtree object
        #  * 1 global shared dirmeta object
        #  * 1 unique commit object
        # => 5 unique + 4 app shared + 1 global shared == 10 total
        #
        # Additionally, each commit will generate one commitmeta2
        # files once prune has been run. However, these will not
        # be reported as total or deleted objects by the prune call,
        # as they are purely side-caches.

        echo "$APP.$DEPTH.1" > $F/file1
        mkdir $F/dir
        echo "$APP.$DEPTH.2" > $F/dir/file2
        echo "$APP.shared1" > $F/dir/shared1
        mkdir $F/shared-dir
        echo "$APP.shared2" > $F/shared-dir/shared2

        echo commiting $APP depth $DEPTH >&2
        ostree --repo=$REPO --branch=$APP --fsync=false --canonical-permissions --no-xattrs commit $F >&2
        rm -rf $F

    if [ ${DEPTH} != "1" ]; then
        create_commit $REPO $APP $((${DEPTH} - 1 ))
    fi
    } 3> /dev/null
}

count_objects() {
    # Wrap this to avoid set -x showing the ls commands
    { { local BASH_XTRACEFD=3; } 2> /dev/null
        NUM_FILE=$(ls -d $1/objects/*/*.filez | wc -l)
        NUM_DIRTREE=$(ls -d $1/objects/*/*.dirtree | wc -l)
        NUM_COMMIT=$(ls -d $1/objects/*/*.commit | wc -l)
        NUM_DIRMETA=$(ls -d $1/objects/*/*.dirmeta | wc -l)
        NUM_COMMITMETA2=0
        if compgen -G "$1/objects/*/*.commitmeta2" > /dev/null; then
            NUM_COMMITMETA2=$(ls -d $1/objects/*/*.commitmeta2 | wc -l)
        fi
        NUM_OBJECT=$(ls -d $1/objects/*/* | wc -l)
        echo OBJCOUNT $1: $NUM_FILE files + $NUM_DIRTREE dirtree + $NUM_COMMIT commit + $NUM_DIRMETA dirmeta + $NUM_COMMITMETA2 commitmeta2 == $NUM_OBJECT objects >&2
    } 3> /dev/null
}


ostree --repo=orig-repo --mode=archive init >&2

create_commit orig-repo app1 3 #   8f 7d 3c 2m == 20
create_commit orig-repo app2 4 #+ 10f 9d 4c 0m == 23
create_commit orig-repo app3 3 #+  8f 7d 3c 0m == 18
#####################   26 23 10  2     61

count_objects orig-repo
assert_streq $NUM_FILE 26
assert_streq $NUM_DIRTREE 23
assert_streq $NUM_COMMIT 10
assert_streq $NUM_DIRMETA 2
assert_streq $NUM_OBJECT 61
assert_streq $NUM_COMMITMETA2 0

#################### Try a no-op prune, should do nothing but create commitmeta2 objects

cp -a orig-repo repo # Work on a copy

# Prune with full depth should change nothing
$FLATPAK build-update-repo --no-update-summary --no-update-appstream --prune --prune-depth=-1 repo > prune.log
cat prune.log >&2

assert_file_has_content prune.log "Total objects: 61"

count_objects repo
assert_streq $NUM_FILE 26
assert_streq $NUM_DIRTREE 23
assert_streq $NUM_COMMIT 10
assert_streq $NUM_DIRMETA 2
assert_streq $NUM_COMMITMETA2 $NUM_COMMIT
assert_streq $NUM_OBJECT 71

cp -a repo incremental-repo # Make a copy of the orig repo with the commitmeta2 objects

# Try again with the commitmeta existing

# Prune with full depth should change nothing
$FLATPAK build-update-repo --no-update-summary --no-update-appstream --prune --prune-depth=-1 repo > prune.log
cat prune.log >&2

assert_file_has_content prune.log "Total objects: 61"

count_objects repo
assert_streq $NUM_FILE 26
assert_streq $NUM_DIRTREE 23
assert_streq $NUM_COMMIT 10
assert_streq $NUM_DIRMETA 2
assert_streq $NUM_COMMITMETA2 $NUM_COMMIT
assert_streq $NUM_OBJECT 71

ok "no-op prune"

############# Try various depth prunes, with and without .commitmeta2

rm -rf repo
cp -a orig-repo repo # Work on a copy

# depth = 2 will only remove one commit from app2

$FLATPAK build-update-repo --no-update-summary --no-update-appstream --prune --prune-depth=2 repo > prune.log
cat prune.log >&2

assert_file_has_content prune.log "Total objects: 61"
assert_file_has_content prune.log "Deleted 5 objects,"

count_objects repo
assert_streq $NUM_FILE 24
assert_streq $NUM_DIRTREE 21
assert_streq $NUM_COMMIT 9
assert_streq $NUM_DIRMETA 2
assert_streq $NUM_COMMITMETA2 $NUM_COMMIT
assert_streq $NUM_OBJECT 65

rm -rf repo
cp -a incremental-repo repo # Work on a copy w/ commitmeta2s

# depth = 2 will only remove one commit from app2

$FLATPAK build-update-repo --no-update-summary --no-update-appstream --prune --prune-depth=2 repo > prune.log
cat prune.log >&2

assert_file_has_content prune.log "Total objects: 61"
assert_file_has_content prune.log "Deleted 5 objects,"

count_objects repo
assert_streq $NUM_FILE 24
assert_streq $NUM_DIRTREE 21
assert_streq $NUM_COMMIT 9
assert_streq $NUM_DIRMETA 2
assert_streq $NUM_COMMITMETA2 $NUM_COMMIT
assert_streq $NUM_OBJECT 65

# depth = 1 will only remove 4 commits

rm -rf repo
cp -a orig-repo repo # Work on a copy

$FLATPAK build-update-repo --no-update-summary --no-update-appstream --prune --prune-depth=1 repo > prune.log
cat prune.log >&2

assert_file_has_content prune.log "Total objects: 61"
assert_file_has_content prune.log "Deleted 20 objects,"

count_objects repo
assert_streq $NUM_FILE 18
assert_streq $NUM_DIRTREE 15
assert_streq $NUM_COMMIT 6
assert_streq $NUM_DIRMETA 2
assert_streq $NUM_COMMITMETA2 $NUM_COMMIT
assert_streq $NUM_OBJECT 47

rm -rf repo
cp -a incremental-repo repo # Work on a copy w/ commitmeta2s

$FLATPAK build-update-repo --no-update-summary --no-update-appstream --prune --prune-depth=1 repo > prune.log
cat prune.log >&2

assert_file_has_content prune.log "Total objects: 61"
assert_file_has_content prune.log "Deleted 20 objects,"

count_objects repo
assert_streq $NUM_FILE 18
assert_streq $NUM_DIRTREE 15
assert_streq $NUM_COMMIT 6
assert_streq $NUM_DIRMETA 2
assert_streq $NUM_COMMITMETA2 $NUM_COMMIT
assert_streq $NUM_OBJECT 47

ok "depth prune"

############# Try non-reachable prunes, with and without .commitmeta2

rm -rf repo
cp -a orig-repo repo # Work on a copy

rm repo/refs/heads/app3 # Removes 3 commits

$FLATPAK build-update-repo --no-update-summary --no-update-appstream --prune --prune-depth=-1 repo > prune.log
cat prune.log >&2
assert_file_has_content prune.log "Total objects: 61"
assert_file_has_content prune.log "Deleted 18 objects,"

count_objects repo
assert_streq $NUM_FILE 18
assert_streq $NUM_DIRTREE 16
assert_streq $NUM_COMMIT 7
assert_streq $NUM_DIRMETA 2
assert_streq $NUM_COMMITMETA2 $NUM_COMMIT
assert_streq $NUM_OBJECT 50

rm -rf repo
cp -a incremental-repo repo # Work on a copy w/ commitmeta2s

rm repo/refs/heads/app3 # Removes 3 commits

$FLATPAK build-update-repo --no-update-summary --no-update-appstream --prune --prune-depth=-1 repo > prune.log
cat prune.log >&2
assert_file_has_content prune.log "Total objects: 61"
assert_file_has_content prune.log "Deleted 18 objects,"

count_objects repo
assert_streq $NUM_FILE 18
assert_streq $NUM_DIRTREE 16
assert_streq $NUM_COMMIT 7
assert_streq $NUM_DIRMETA 2
assert_streq $NUM_COMMITMETA2 $NUM_COMMIT
assert_streq $NUM_OBJECT 50

ok "unreachable prune"

# Combine depth and unreachable

rm -rf repo
cp -a orig-repo repo # Work on a copy

rm repo/refs/heads/app3 # Removes 3 commits

$FLATPAK build-update-repo --no-update-summary --no-update-appstream --prune --prune-depth=2 repo > prune.log
cat prune.log >&2
assert_file_has_content prune.log "Total objects: 61"
assert_file_has_content prune.log "Deleted 23 objects,"

count_objects repo
assert_streq $NUM_FILE 16
assert_streq $NUM_DIRTREE 14
assert_streq $NUM_COMMIT 6
assert_streq $NUM_DIRMETA 2
assert_streq $NUM_COMMITMETA2 $NUM_COMMIT
assert_streq $NUM_OBJECT 44

rm -rf repo
cp -a incremental-repo repo # Work on a copy w/ commitmeta2s

rm repo/refs/heads/app3 # Removes 3 commits

$FLATPAK build-update-repo --no-update-summary --no-update-appstream --prune --prune-depth=2 repo > prune.log
cat prune.log >&2
assert_file_has_content prune.log "Total objects: 61"
assert_file_has_content prune.log "Deleted 23 objects,"

count_objects repo
assert_streq $NUM_FILE 16
assert_streq $NUM_DIRTREE 14
assert_streq $NUM_COMMIT 6
assert_streq $NUM_DIRMETA 2
assert_streq $NUM_COMMITMETA2 $NUM_COMMIT
assert_streq $NUM_OBJECT 44

ok "unreachable and depth prune"

# Compare last result with ostree prune:
cp -a orig-repo ostree-repo
rm ostree-repo/refs/heads/app3 # Removes 3 commits
ostree prune --refs-only --depth=2 --repo=ostree-repo >&2
rm -rf repo/objects/*/*.commitmeta2
diff -r repo ostree-repo >&2

ok "Compare with ostree prune"
