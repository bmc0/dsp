### About:

Dsp is an audio processing program with simple digital signal processing capabilities and an interactive mode. Dsp is also capable of generating gnuplot commands to plot the amplitude vs frequency response of a given effects chain.

### Building:

#### Dependencies:

* dash
* libsndfile
* libmad
* alsa-lib

#### Build:

	$ ./build.sh

### Usage:

#### Synopsis:

	dsp [[options] path ...] [plot] [[effect] [args ...] ...]

Run `dsp -h` for options, supported input/output types and supported effects.

#### Examples:

##### Read file.flac, apply a bass boost, and write to alsa device hw:2:

	dsp file.flac -ot alsa -e s24_3 hw:2 lowshelf 60 0.5 +4.0

##### Plot amplitude vs frequency for a complex effects chain:

	dsp plot gain -1.5 lowshelf 41 0.75 +8.0 lowshelf 90 0.6 +2.5 eq 100 2.5 -4.5 eq 240 1.2 -4.7 eq 620 2.8 +2.0 lowshelf 1450 0.60 -12.0 eq 2250 2.0 +2.0 eq 3000 8.0 -1.0 eq 3750 2.1 +2.8 eq 5000 2.5 -2.1 eq 9500 1.8 -7.2 highshelf 10000 0.7 +1.5 | gnuplot -p

### Bugs:

* No support for metadata.
