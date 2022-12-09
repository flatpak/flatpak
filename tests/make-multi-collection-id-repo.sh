#!/bin/bash
#
# make-multi-collection-id-repo.sh: Creates an  ostree repository
# that will hold a different collection ID per ref.
#
# Copyright (C) 2017 Endless, Inc.
#
# Authors:
#     Joaquim Rocha <jrocha@endlessm.com>
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

set -e

. $(dirname $0)/libtest.sh

REPO_DIR=$1
REPO_NAME=$(basename $REPO_DIR)

COLLECTION_ID_PREFIX=org.test.Collection

ostree --repo=${REPO_DIR} init --mode=archive --collection-id=${COLLECTION_ID_PREFIX}1 >&2

for i in {1..3}; do
    APP_REPO=test${i}
    APP_REPO_DIR=`pwd`/repos/${APP_REPO}
    APP_ID=org.test.Hello${i}
    COLLECTION_ID=${COLLECTION_ID_PREFIX}${i}

    $(dirname $0)/make-test-app.sh repos/${APP_REPO} ${APP_ID} master ${COLLECTION_ID}
    ref=$(ostree --repo=${APP_REPO_DIR} refs | grep ${APP_ID})

    ostree --repo=${REPO_DIR} remote add --no-sign-verify --collection-id=${COLLECTION_ID} ${APP_REPO} file://${APP_REPO_DIR} >&2
    ostree --repo=${REPO_DIR} pull ${APP_REPO} ${ref} >&2
done

ostree --repo=${REPO_DIR} summary --update >&2
