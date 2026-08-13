#!/bin/sh
set -eu

if [ "${1:-}" = install ]; then
    command=$1
    shift
    set -- "$command" --allow-downgrades "$@"
fi

exec apt-get \
    -o Acquire::AllowInsecureRepositories=false \
    -o Acquire::AllowDowngradeToInsecureRepositories=false \
    -o APT::Get::AllowUnauthenticated=false \
    -o Acquire::Retries=5 \
    -o Acquire::http::Timeout=30 \
    -o Acquire::https::Timeout=30 \
    "$@"
