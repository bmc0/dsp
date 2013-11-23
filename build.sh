#!/bin/sh

fail() {
	echo ">> Failed"
	exit $1
}

packages="alsa sndfile mad fftw3"
for i in $packages; do
	if ! pkg-config --exists $i; then
		echo ">> Error: dsp requires $i"
		fail 1
	fi
done

[ -z "$CC" ] && CC="cc"
CFLAGS="-Os -Wall -std=gnu99 $(pkg-config --cflags $packages) $CFLAGS"
LDFLAGS="-lm $(pkg-config --libs $packages) $LDFLAGS"

$CC -o dsp \
	dsp.c \
	effect.c \
	codec.c \
	dither.c \
	sampleconv.c \
	util.c \
	effects/biquad.c \
	effects/gain.c \
	effects/crossfeed.c \
	effects/crossfeed_hrtf.c \
	effects/mono.c \
	codecs/sndfile.c \
	codecs/alsa.c \
	codecs/mp3.c \
	codecs/null.c \
	$CFLAGS $LDFLAGS || fail $?

echo ">> Build successful"
