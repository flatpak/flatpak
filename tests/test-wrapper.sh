#!/bin/bash

for feature in $(echo $1 | sed "s/^.*@\(.*\).wrap/\1/" | tr "," "\n"); do
    case $feature in
        system)
            export USE_SYSTEMDIR=yes
            ;;
        user)
            export USE_SYSTEMDIR=no
            ;;
        deltas)
            export USE_DELTAS=yes
            ;;
        nodeltas)
            export USE_DELTAS=no
            ;;
        collections)
            export USE_COLLECTIONS_IN_SERVER=yes
            export USE_COLLECTIONS_IN_CLIENT=yes
            ;;
        collections-server-only)
            export USE_COLLECTIONS_IN_SERVER=yes
            export USE_COLLECTIONS_IN_CLIENT=no
            ;;
        *)
            echo unsupported test feature $feature
            exit 1
    esac
done

WRAPPED=$(echo $1 | sed "s/@.*/\.sh/")
. $WRAPPED "$@"
