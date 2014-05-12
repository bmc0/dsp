#!/bin/sh

fail() {
	echo ">> Failed"
	exit $1
}

unset PLUGIN_SOURCES
PACKAGES=""  # Required packages
for i in $PACKAGES; do
	if ! pkg-config --exists $i; then
		echo ">> Error: ladspa_dsp requires $i"
		fail 1
	fi
done
echo -n ">> Enabled plugins:"
echo

[ -z "$CC" ] && CC="cc"
[ -z "$LD" ] && LD="ld"
CFLAGS="-Os -Wall -fPIC -DPIC -std=gnu99 $([ -n "$PACKAGES" ] && pkg-config --cflags $PACKAGES) $CFLAGS"
LDFLAGS="-shared -lm $([ -n "$PACKAGES" ] && pkg-config --libs $PACKAGES) $LDFLAGS"

SOURCES="ladspa_dsp.c
	effect.c
	util.c
	effects/biquad.c
	effects/gain.c
	effects/crossfeed.c
	effects/remix.c
	$PLUGIN_SOURCES"
OBJECTS="$(for i in $SOURCES; do echo "${i%.c}.o"; done)"

for i in $SOURCES; do
	echo "CC    ${i%.c}.o"
	$CC -c -o ${i%.c}.o $i $CFLAGS || fail $?
done
echo "LD    ladspa_dsp.so"
$LD -o ladspa_dsp.so $OBJECTS $LDFLAGS || fail $?
rm $OBJECTS

echo ">> Build successful"
