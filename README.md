### About:

Dsp is an audio processing program with an interactive mode. Dsp is capable of generating gnuplot commands to plot the amplitude vs frequency response of a given effects chain.

### Building:

#### Dependencies:

* GNU Make
* libsndfile

#### Optional dependencies:

* libmad: for mp3 input support
* alsa-lib: for alsa input/output support
* fftw3: for resample and fir effects
* libao: for ao output support
* LADSPA: for building the LADSPA frontend
* ffmpeg (libavcodec, libavformat, libavutil, and libavresample): for ffmpeg input support.
* libpulse: for PulseAudio input/ouput support

#### Build:

	$ make

Run `./configure [options]` manually if you want to build with non-default options. Run `./configure --help` to see all available options.

### Usage:

#### Synopsis:

	dsp [options] path ... [:channel_selector] [@[~/]effects_file] [effect [args ...]] ...

Run `dsp -h` for options, supported input/output types and supported effects.

#### Selector syntax:

	[[start][-[end]][,...]]

Example | Description
--- | ---
<empty> | all
- | all
2- | 2 to n
-4 | 0 through 4
1,3 | 1 and 3
1-4,7,9- | 1 through 4, 7, and 9 to n

#### Effects file paths:

* On the command line, relative paths are relative to `$PWD`.
* Within an effects file, relative paths are relative to the directory containing said effects file.
* The `~/` prefix will be expanded to the contents of `$HOME`.

#### Effects file syntax:

* Arguments are delimited by whitespace.
* If the first non-whitespace character in a line is `#`, the line is ignored.
* The `\` character removes any special meaning of the next character.

Example:

	gain -10
	# This is a comment
	eq 1k 1.0 +10.0 eq 3k 3.0 -4.0
	lowshelf 90 0.7 +4.0

Effects files inherit a copy of the current channel selector. In other words, if an effects chain is this:

	:2,4 @eq_file.txt eq 2k 1.0 -2.0

`eq_file.txt` will inherit the `2,4` selector, but any selector specified within `eq_file.txt` will not affect the `eq 2k 1.0 -2.0` effect that comes after it.

#### Examples:

Read `file.flac`, apply a bass boost, and write to alsa device `hw:2`:

	dsp file.flac -ot alsa -e s24_3 hw:2 lowshelf 60 0.5 +4.0

Plot amplitude vs frequency for a complex effects chain:

	dsp -pn gain -1.5 lowshelf 60 0.7 +7.8 eq 50 2.0 -2.7 eq 100 2.0 -3.9 eq 242 1.0 -3.8 eq 628 2.0 +2.1 eq 700 1.5 -1.0 lowshelf 1420 0.68 -12.5 eq 2500 1.3 +3.0 eq 3000 8.0 -1.8 eq 3500 2.5 +1.4 eq 6000 1.1 -3.4 eq 9000 1.8 -5.6 highshelf 10000 0.7 -0.5 | gnuplot -p

Implement an LR4 crossover at 2.2KHz, where output channels 0 and 2 are the left and right woofers, and channels 1 and 3 are the left and right tweeters, respectively:

	dsp stereo_file.flac -ot alsa -e s32 hw:3 remix 0 0 1 1 :0,2 lowpass 2.2k 0.707 lowpass 2.2k 0.707 :1,3 highpass 2.2k 0.707 highpass 2.2k 0.707 :

Apply effects from a file:

	dsp file.flac @eq.txt

### LADSPA frontend (experimental):

#### Configuration:

The default configuration file is located at `$XDG_CONFIG_HOME/ladspa_dsp/config` (override by setting the `LADSPA_DSP_CONFIG` environment variable) and is a simple key-value format. Whitespace is not ignored. Valid keys are:

Key | Description
--- | ---
input_channels | Number of input channels.
output_channels | Number of output channels.
effects_chain | Args to build the effects chain. The format is the same as an effects file, but only a single line is interpreted.

Example configuration:

	# Comment
	input_channels=2
	output_channels=2
	effects_chain=gain -4.0 lowshelf 90 0.7 +4.0 @/path/to/eq_file

The loglevel can be set to `VERBOSE`, `NORMAL`, or `SILENT` through the `LADSPA_DSP_LOGLEVEL` environment variable.

#### Usage example: Route alsa audio through ladspa_dsp:

Put this in `~/.asoundrc`:

	pcm.<dev>_sampleconv {
		type plug
		slave {
			pcm "<dev>"
			format S32
			rate unchanged
		}
	}
	
	pcm.ladspa_dsp {
		type ladspa
		slave.pcm "<dev>_sampleconv"
		path "/ladspa/path"
		plugins [{
			label "ladspa_dsp"
			policy none
		}]
	}
	
	pcm.dsp {
		type plug
		slave {
			pcm "ladspa_dsp"
			format FLOAT
			rate unchanged
		}
	}

Replace `/ladspa/path` with the path to the directory containing `ladspa_dsp.so` and `<dev>` with the preferred output device (`hw:1`, for example).

To make `dsp` the default device, append this to `~/.asoundrc`:

	pcm.!default {
		type copy
		slave.pcm "dsp"
	}

If mixing is desired, the `dmix` plugin can be used instead of `copy`.

Note: The resample effect cannot be used with the LADSPA frontend.

### Bugs:

* No support for metadata.
* Some effects do not support plotting.
