#!/usr/bin/env sh
set -eu
mkdir -p build
: "${CC:=gcc}"
: "${CFLAGS:=-std=c11 -Wall -Wextra -Wpedantic -O0}"
$CC $CFLAGS -I src -c src/main.c -o build/main.o
$CC $CFLAGS -I src -c src/mqtt.c -o build/mqtt.o
$CC $CFLAGS -I src -c src/broker.c -o build/broker.o
$CC $CFLAGS -o sol-broker build/main.o build/mqtt.o build/broker.o
printf 'Built ./sol-broker\n'
