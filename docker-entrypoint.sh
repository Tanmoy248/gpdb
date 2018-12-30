#!/bin/bash
set -e

if [ $# -lt 1 ]; then
    echo "Please pass a service name to start"
	exit 1
fi

if [ "$1" = 'greenplum' ]; then
    rm -f /var/run/nologin
	su - gpadmin bash -c 'echo 'y' | gpstart'
fi
