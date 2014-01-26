### About:

Dsp is an audio processing program with simple digital signal processing capabilities and an interactive mode. Dsp is also capable of generating gnuplot commands to plot the amplitude vs frequency response of a given effects chain.

### Building:

#### Dependencies:

* libsndfile
* libmad
* alsa-lib
* fftw3

#### Build:

	$ ./build.sh

### Usage:

#### Synopsis:

	dsp [[options] path ...] [[:channel_selector] [effect] [args ...] ...]

Run `dsp -h` for options, supported input/output types and supported effects.

#### Examples:

Read `file.flac`, apply a bass boost, and write to alsa device `hw:2`:

	dsp file.flac -ot alsa -e s24_3 hw:2 lowshelf 60 0.5 +4.0

Plot amplitude vs frequency for a complex effects chain:

	dsp -pn gain -1.5 lowshelf 60 0.7 +7.8 eq 50 2.0 -2.7 eq 100 2.0 -3.9 eq 242 1.0 -3.8 eq 628 2.0 +2.1 eq 700 1.5 -1.0 lowshelf 1420 0.68 -12.5 eq 2500 1.3 +3.0 eq 3000 8.0 -1.8 eq 3500 2.5 +1.4 eq 6000 1.1 -3.4 eq 9000 1.8 -5.6 highshelf 10000 0.7 -0.5 | gnuplot -p

### Bugs:

* No support for metadata.
* Some effects do not support plotting.
