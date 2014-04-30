#!/bin/sh

fail() {
	echo ">> Failed"
	exit $1
}

unset PLUGIN_SOURCES
PACKAGES="sndfile"  # Required packages
for i in $PACKAGES; do
	if ! pkg-config --exists $i; then
		echo ">> Error: dsp requires $i"
		fail 1
	fi
done

# Optional plugins
echo -n ">> Enabled plugins:"
if pkg-config --exists alsa; then
	PACKAGES="$PACKAGES alsa"
	PLUGIN_SOURCES="$PLUGIN_SOURCES codecs/alsa.c"
	CFLAGS="$CFLAGS -D__HAVE_ALSA__"
	echo -n " codecs/alsa.c"
fi
if pkg-config --exists ao; then
	PACKAGES="$PACKAGES ao"
	PLUGIN_SOURCES="$PLUGIN_SOURCES codecs/ao.c"
	CFLAGS="$CFLAGS -D__HAVE_AO__"
	echo -n " codecs/ao.c"
fi
if pkg-config --exists mad; then
	PACKAGES="$PACKAGES mad"
	PLUGIN_SOURCES="$PLUGIN_SOURCES codecs/mp3.c"
	CFLAGS="$CFLAGS -D__HAVE_MAD__"
	echo -n " codecs/mp3.c"
fi
if pkg-config --exists fftw3; then
	PACKAGES="$PACKAGES fftw3"
	PLUGIN_SOURCES="$PLUGIN_SOURCES effects/crossfeed_hrtf.c"
	CFLAGS="$CFLAGS -D__HAVE_FFTW3__"
	echo -n " effects/crossfeed_hrtf.c"
fi
echo

[ -z "$CC" ] && CC="cc"
CFLAGS="-Os -Wall -std=gnu99 $(pkg-config --cflags $PACKAGES) $CFLAGS"
LDFLAGS="-lm $(pkg-config --libs $PACKAGES) $LDFLAGS"

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
	effects/remix.c \
	codecs/sndfile.c \
	codecs/null.c \
	$PLUGIN_SOURCES \
	$CFLAGS $LDFLAGS || fail $?

echo ">> Build successful"
