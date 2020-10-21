#!/bin/bash

for feature in $(echo $1 | sed "s/^.*@\(.*\).wrap/\1/" | tr "," "\n"); do
    case $feature in
        system)
            export USE_SYSTEMDIR=yes
            ;;
        system-norevokefs)
            export USE_SYSTEMDIR=yes
            export FLATPAK_DISABLE_REVOKEFS=yes
            ;;
        user)
            export USE_SYSTEMDIR=no
            ;;
        deltas)
            export USE_DELTAS=yes
            ;;
        newsummary)
            export SUMMARY_FORMAT=new
            ;;
        oldsummary)
            export SUMMARY_FORMAT=old
            ;;
        nodeltas)
            export USE_DELTAS=no
            ;;
        labels)
            export USE_OCI_LABELS=yes
            ;;
        annotations)
            export USE_OCI_ANNOTATIONS=yes
            ;;
        *)
            echo unsupported test feature $feature
            exit 1
    esac
done

WRAPPED=$(echo $1 | sed "s/@.*/\.sh/")
. $WRAPPED "$@"
