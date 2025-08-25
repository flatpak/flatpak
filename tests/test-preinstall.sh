#!/bin/bash
#
# Copyright (C) 2025 Red Hat, Inc
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

USE_COLLECTIONS_IN_SERVER=yes
USE_COLLECTIONS_IN_CLIENT=yes

. $(dirname $0)/libtest.sh

mkdir -p $FLATPAK_DATA_DIR/preinstall.d
mkdir -p $FLATPAK_CONFIG_DIR/preinstall.d

cat << EOF > hello-install.preinstall
[Flatpak Preinstall org.test.Hello]
EOF

cat << EOF > hello-not-install.preinstall
[Flatpak Preinstall org.test.Hello]
Install=false
EOF

cat << EOF > hello-install-multi.preinstall
[Flatpak Preinstall org.test.Hello]
[Flatpak Preinstall org.test.Hello2]
CollectionID=org.test.Collection.test
EOF

cat << EOF > hello-install-devel.preinstall
[Flatpak Preinstall org.test.Hello]
Branch=devel
EOF

cat << EOF > hello-install-collection.preinstall
[Flatpak Preinstall org.test.Hello2]
CollectionID=org.test.Collection.test2
EOF

cat << EOF > bad.preinstall
[Wrong Group]
a=b

[Flatpak Preinstall ]
Install=false

[Flatpak Preinstall]
Install=true
EOF

# Set up the runtimes
# org.test.Platform//master and org.test.Platform//devel
# and the apps
# org.test.Hello//master, org.test.Hello//devel,
# org.test.Hello2//master, org.test.Hello2//devel
setup_repo test
make_updated_runtime test org.test.Collection.test devel HELLO_DEVEL org.test.Hello
make_updated_app test org.test.Collection.test devel HELLO_DEVEL org.test.Hello
make_updated_app test org.test.Collection.test master HELLO2_MASTER org.test.Hello2
make_updated_app test org.test.Collection.test devel HELLO2_DEVEL org.test.Hello2

setup_repo test2
make_updated_app test2 org.test.Collection.test2 master HELLO2_MASTER_C2 org.test.Hello2

echo "1..10"

# just checking that the test remote got added
port=$(cat httpd-port)
assert_remote_has_config test-repo url "http://127.0.0.1:${port}/test"
assert_remote_has_config test2-repo url "http://127.0.0.1:${port}/test2"

ok "setup"

# if we have nothing configured and nothing is marked as preinstalled
# calling preinstall should be a no-op
${FLATPAK} ${U} preinstall -y > nothingtodo
assert_file_has_content nothingtodo "Nothing to do"

ok "no config"

# make sure nothing is installed
${FLATPAK} ${U} list --columns=ref > list-log
assert_file_empty list-log
! ostree config --repo=$XDG_DATA_HOME/flatpak/repo get --group "core" xa.preinstalled &> /dev/null

# The preinstall config wants org.test.Hello.
cp hello-install.preinstall $FLATPAK_DATA_DIR/preinstall.d/

${FLATPAK} ${U} preinstall -y >&2

# Make sure it and the runtime were installed
${FLATPAK} ${U} list --columns=ref > list-log
assert_file_has_content     list-log "^org\.test\.Hello/.*/master$"
assert_not_file_has_content list-log "^org\.test\.Hello/.*/devel$"
assert_not_file_has_content list-log "^org\.test\.Hello2/.*/master$"
assert_not_file_has_content list-log "^org\.test\.Hello2/.*/devel$"
assert_file_has_content     list-log "^org\.test\.Platform/.*/master$"
assert_not_file_has_content list-log "^org\.test\.Platform/.*/devel$"

ostree config --repo=$XDG_DATA_HOME/flatpak/repo get --group "core" xa.preinstalled > marked-preinstalled
assert_file_has_content marked-preinstalled "^app/org\.test\.Hello/.*/master$"

ok "simple preinstall"

# Make sure calling preinstall with the same config again is a no-op...
${FLATPAK} ${U} preinstall -y > nothingtodo
assert_file_has_content nothingtodo "Nothing to do"

# ...and everything is still installed
${FLATPAK} ${U} list --columns=ref > list-log
assert_file_has_content     list-log "^org\.test\.Hello/.*/master$"
assert_not_file_has_content list-log "^org\.test\.Hello/.*/devel$"
assert_not_file_has_content list-log "^org\.test\.Hello2/.*/master$"
assert_not_file_has_content list-log "^org\.test\.Hello2/.*/devel$"
assert_file_has_content     list-log "^org\.test\.Platform/.*/master$"
assert_not_file_has_content list-log "^org\.test\.Platform/.*/devel$"

ok "simple preinstall no op"

${FLATPAK} ${U} uninstall -y org.test.Hello >&2

${FLATPAK} ${U} list --columns=ref > list-log
assert_not_file_has_content list-log "^org\.test\.Hello/.*/master$"
assert_not_file_has_content list-log "^org\.test\.Hello/.*/devel$"
assert_not_file_has_content list-log "^org\.test\.Hello2/.*/master$"
assert_not_file_has_content list-log "^org\.test\.Hello2/.*/devel$"
assert_file_has_content     list-log "^org\.test\.Platform/.*/master$"
assert_not_file_has_content list-log "^org\.test\.Platform/.*/devel$"

# Make sure calling preinstall with the same config again is a no-op
# Even if the user uninstalled the app (it is marked as preinstalled)
${FLATPAK} ${U} preinstall -y > nothingtodo
assert_file_has_content nothingtodo "Nothing to do"

# Make sure nothing has changed
${FLATPAK} ${U} list --columns=ref > list-log
assert_not_file_has_content list-log "^org\.test\.Hello/.*/master$"
assert_not_file_has_content list-log "^org\.test\.Hello/.*/devel$"
assert_not_file_has_content list-log "^org\.test\.Hello2/.*/master$"
assert_not_file_has_content list-log "^org\.test\.Hello2/.*/devel$"
assert_file_has_content     list-log "^org\.test\.Platform/.*/master$"
assert_not_file_has_content list-log "^org\.test\.Platform/.*/devel$"

ok "uninstall preinstall"

${FLATPAK} ${U} install test-repo -y org.test.Hello master >&2

${FLATPAK} ${U} list --columns=ref > list-log
assert_file_has_content     list-log "^org\.test\.Hello/.*/master$"
assert_not_file_has_content list-log "^org\.test\.Hello/.*/devel$"
assert_not_file_has_content list-log "^org\.test\.Hello2/.*/master$"
assert_not_file_has_content list-log "^org\.test\.Hello2/.*/devel$"
assert_file_has_content     list-log "^org\.test\.Platform/.*/master$"
assert_not_file_has_content list-log "^org\.test\.Platform/.*/devel$"

# Add a config to /etc which overwrites the config in /usr ($FLATPAK_DATA_DIR)
# It has the Install=false setting which means it shall not be installed.
cp hello-not-install.preinstall $FLATPAK_CONFIG_DIR/preinstall.d/

${FLATPAK} ${U} preinstall -y >&2

# Make sure preinstall removed org.test.Hello as indicated by the config
${FLATPAK} ${U} list --columns=ref > list-log
assert_not_file_has_content list-log "^org\.test\.Hello/.*/master$"
assert_not_file_has_content list-log "^org\.test\.Hello/.*/devel$"
assert_not_file_has_content list-log "^org\.test\.Hello2/.*/master$"
assert_not_file_has_content list-log "^org\.test\.Hello2/.*/devel$"
assert_file_has_content     list-log "^org\.test\.Platform/.*/master$"
assert_not_file_has_content list-log "^org\.test\.Platform/.*/devel$"

ok "preinstall install false"

# Remove the existing configs
rm -rf $FLATPAK_CONFIG_DIR/preinstall.d/*
rm -rf $FLATPAK_DATA_DIR/preinstall.d/*

# Add a config file which wants org.test.Hello and org.test.Hello2 installed
cp hello-install-multi.preinstall $FLATPAK_DATA_DIR/preinstall.d/

${FLATPAK} ${U} preinstall -y >&2

# Make sure both apps got installed
${FLATPAK} ${U} list --columns=ref > list-log
assert_file_has_content     list-log "^org\.test\.Hello/.*/master$"
assert_not_file_has_content list-log "^org\.test\.Hello/.*/devel$"
assert_file_has_content     list-log "^org\.test\.Hello2/.*/master$"
assert_not_file_has_content list-log "^org\.test\.Hello2/.*/devel$"
assert_file_has_content     list-log "^org\.test\.Platform/.*/master$"
assert_not_file_has_content list-log "^org\.test\.Platform/.*/devel$"

if have_working_bwrap; then
  # Also make sure we installed the app from the right CollectionID
  ${FLATPAK} run org.test.Hello2 > hello2-output
  assert_file_has_content hello2-output "HELLO2_MASTER$"
fi

ok "install multi"

# Overwrite the branch of org.test.Hello from master to devel
cp hello-install-devel.preinstall $FLATPAK_CONFIG_DIR/preinstall.d/

${FLATPAK} ${U} preinstall -y >&2

# Make sure org.test.Hello//devel replaced org.test.Hello//master
${FLATPAK} ${U} list --columns=ref > list-log
assert_not_file_has_content list-log "^org\.test\.Hello/.*/master$"
assert_file_has_content     list-log "^org\.test\.Hello/.*/devel$"
assert_file_has_content     list-log "^org\.test\.Hello2/.*/master$"
assert_not_file_has_content list-log "^org\.test\.Hello2/.*/devel$"
assert_file_has_content     list-log "^org\.test\.Platform/.*/master$"
assert_file_has_content     list-log "^org\.test\.Platform/.*/devel$"

ok "overwrite branch"

# Overwrite the CollectionID we're installing org.test.Hello2 from
cp hello-install-collection.preinstall $FLATPAK_CONFIG_DIR/preinstall.d/

# Changing the collection id doesn't automatically change apps over so we need
# to uninstall and mark it as not pre-installed
${FLATPAK} ${U} uninstall -y org.test.Hello2 >&2
ostree config --repo=$XDG_DATA_HOME/flatpak/repo unset --group "core" xa.preinstalled

${FLATPAK} ${U} preinstall -y >&2

if have_working_bwrap; then
  # Make sure the app with the right CollectionID got installed
  ${FLATPAK} run org.test.Hello2 > hello2-output
  assert_file_has_content hello2-output "HELLO2_MASTER_C2$"
fi

ok "change collection id"

# Make sure some config file parsing edge cases don't blow up
cp bad.preinstall $FLATPAK_CONFIG_DIR/preinstall.d/

${FLATPAK} ${U} preinstall -y > nothingtodo
assert_file_has_content nothingtodo "Nothing to do"

ok "bad config"