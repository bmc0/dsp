### About

dsp is an audio processing program with an interactive mode.

### Building

#### Dependencies

* GNU Make
* pkg-config

#### Optional dependencies

* fftw3: For `resample` and `fir` effects.
* zita-convolver: For the `zita_convolver` effect.
* libsndfile: For sndfile input/output support (recommended).
* ffmpeg (libavcodec, libavformat, and libavutil): For ffmpeg input support.
* alsa-lib: For alsa input/output support.
* libao: For ao output support.
* libmad: For mp3 input support.
* libpulse-simple: For PulseAudio input/ouput support.
* LADSPA: For the LADSPA frontend and the `ladspa_host` effect.
* libltdl (libtool): For the `ladspa_host` effect.

#### Build

	$ make

Run `./configure [options]` manually if you want to build with non-default
options. Run `./configure --help` to see all available options.

#### Install

	# make install

### Synopsis

	dsp [options] path ... [!] [:channel_selector]
		[@[~/]effects_file] [effect [args ...]] ...

### Options

#### Global options

Flag        | Description
----------- | --------------------------------------------------------------------------
`-h`        | Show help text.
`-b frames` | Set buffer size (must be given before the first input).
`-R ratio`  | Set codec maximum buffer ratio (must be given before the first input).
`-i`        | Force interactive mode.
`-I`        | Disable interactive mode.
`-q`        | Disable progress display.
`-s`        | Silent mode.
`-v`        | Verbose mode.
`-d`        | Force dithering.
`-D`        | Disable dithering.
`-E`        | Don't drain effects chain before rebuilding.
`-p`        | Plot effects chain instead of processing audio.
`-V`        | Enable verbose progress display.
`-S`        | Use "sequence" input combining mode.

#### Input/output options

Flag              | Description
----------------- | -----------------------------
`-o`              | Output.
`-t type`         | Type.
`-e encoding`     | Encoding.
`-B/L/N`          | Big/little/native endian.
`-r frequency[k]` | Sample rate.
`-c channels`     | Number of channels.
`-n`              | Equivalent to `-t null null`.

### Inputs and Outputs

#### Supported input/output types

Type    | Modes | Encodings
------- | ----- | -------------------------------------------------------------------------------------
null    | rw    | sample_t
sgen    | r     | sample_t
sndfile | r     | autodetected
wav     | rw    | s16 u8 s24 s32 float double mu-law a-law ima_adpcm ms_adpcm gsm6.10 g721_32
aiff    | rw    | s16 s8 u8 s24 s32 float double mu-law a-law ima_adpcm gsm6.10 dwvw_12 dwvw_16 dwvw_24
au      | rw    | s16 s8 s24 s32 float double mu-law a-law g721_32 g723_24 g723_40
raw     | rw    | s16 s8 u8 s24 s32 float double mu-law a-law gsm6.10 vox_adpcm dwvw_12 dwvw_16 dwvw_24
paf     | rw    | s16 s8 s24
svx     | rw    | s16 s8
nist    | rw    | s16 s8 s24 s32 mu-law a-law
voc     | rw    | s16 u8 mu-law a-law
ircam   | rw    | s16 s32 float mu-law a-law
w64     | rw    | s16 u8 s24 s32 float double mu-law a-law ima_adpcm ms_adpcm gsm6.10
mat4    | rw    | s16 s32 float double
mat5    | rw    | s16 u8 s32 float double
pvf     | rw    | s16 s8 s32
xi      | rw    | dpcm_8 dpcm_16
htk     | rw    | s16
sds     | rw    | s16 s8 s24
avr     | rw    | s16 s8 u8
wavex   | rw    | s16 u8 s24 s32 float double mu-law a-law
sd2     | rw    | s16 s8 s24
flac    | rw    | s16 s8 s24
caf     | rw    | s16 s8 s24 s32 float double mu-law a-law
wve     | rw    | a-law
ogg     | rw    | vorbis
mpc2k   | rw    | s16
rf64    | rw    | s16 u8 s24 s32 float double mu-law a-law
ffmpeg  | r     | autodetected
alsa    | rw    | s16 u8 s8 s24 s24_3 s32 float double
ao      | w     | s16 u8 s32
mp3     | r     | mad_f
pcm     | rw    | s16 u8 s8 s24 s32 float double
pulse   | rw    | s16 u8 s24 s24_3 s32 float

#### Input combining modes

In concatenate mode (the default), the inputs are concatenated in the order
given and sent to the output. All inputs must have the same sample rate and
number of channels.

In sequence mode, the inputs are sent serially to the output like concatenate
mode, but the inputs do not need to have the same sample rate or number of
channels. The effects chain and/or output will be rebuilt/reopened when
required. Note that if the output is a file, the file will be truncated if it
is reopened. This mode is most useful when the output is an audio device, but
can also be used to concatenate inputs with different sample rates and/or
numbers of channels into a single output file when used with the `resample`
and/or `remix` effects.

#### Signal generator

The `sgen` input type is a basic (for now, at least) signal generator that can
generate impulses and exponential sine sweeps. The syntax for the `path`
argument is as follows:

	[type[@channel_selector][:arg[=value]...]][/type...][+len[s|m|S]]

`type` may be `sine` for sine sweeps or tones, or `delta` for a delta function
(impulse). `sine` accepts the following arguments:

* `freq=f0[k][-f1[k]]`
	Frequency. If `len` is set and `f1` is given, an exponential sine sweep
	is generated.

The arguments for `delta` are:

* `offset=time[s|m|S]`
	Offset in seconds, miliseconds or samples.

Example:

	$ dsp -t sgen -c 2 sine@0:freq=500-1k/sine@1:freq=300-800+2 gain -10

### Effects

#### Full effects list

* `lowpass_1 f0[k]`  
	Single-pole lowpass filter.
* `highpass_1 f0[k]`  
	Single-pole highpass filter.
* `lowpass f0[k] width[q|o|h|k]`  
	Double-pole lowpass filter.
* `highpass f0[k] width[q|o|h|k]`  
	Double-pole highpass filter.
* `bandpass_skirt f0[k] width[q|o|h|k]`  
	Double-pole bandpass filter with constant skirt gain.
* `bandpass_peak f0[k] width[q|o|h|k]`  
	Double-pole bandpass filter with constant peak gain.
* `notch f0[k] width[q|o|h|k]`  
	Double-pole notch filter.
* `allpass f0[k] width[q|o|h|k]`  
	Double-pole allpass filter.
* `eq f0[k] width[q|o|h|k] gain`  
	Double-pole peaking filter.
* `lowshelf f0[k] width[q|s|d|o|h|k] gain`  
	Double-pole lowshelf filter.
* `highshelf f0[k] width[q|s|d|o|h|k] gain`  
	Double-pole highshelf filter.
* `linkwitz_transform fz[k] qz fp[k] qp`  
	Linkwitz transform (see http://www.linkwitzlab.com/filters.htm#9).
* `deemph`  
	Compact Disc de-emphasis filter.
* `biquad b0 b1 b2 a0 a1 a2`  
	Biquad filter.
* `gain [channel] gain`  
	Gain adjustment. Ignores the channel selector when the `channel` argument
	is given.
* `mult [channel] multiplier`  
	Multiplies each sample by `multiplier`. Ignores the channel selector when
	the `channel` argument is given.
* `add [channel] value`  
	Applies a DC shift. Ignores the channel selector when the `channel`
	argument is given.
* `crossfeed f0[k] separation`  
	Simple crossfeed for headphones. Very similar to Linkwitz/Meier/CMoy/bs2b
	crossfeed.
* `remix channel_selector|. ...`  
	Select and mix input channels into output channels. Each channel selector
	specifies the input channels to be mixed to produce each output channel.
	`.` selects no input channels. For example, `remix 0,1 2,3` mixes input
	channels 0 and 1 into output channel 0, and input channels 2 and 3 into
	output channel 1. `remix -` mixes all input channels into a single
	output channel.
* `st2ms`
	Convert stereo to mid/side.
* `ms2st`
	Convert mid/side to stereo.
* `delay delay[s|m|S]`  
	Delay line. The unit for the delay argument depends on the suffix used:
	`s` is seconds (the default), `m` is milliseconds, and `S` is samples.
* `resample [bandwidth] fs[k]`  
	Sinc resampler. Ignores the channel selector.
* `fir [~/]impulse_path`  
	Non-partitioned 64-bit FFT convolution. Latency is equal to the length
	of the impulse.
* `fir_p [min_part_len [max_part_len]] [~/]impulse_path`  
	Non-uniform partitioned 64-bit FFT convolution. Runs slower than the
	`zita_convolver` effect, but potentially useful if you need more precision
	and/or lower latency. Latency is equal to `min_part_len` (16 samples by
	default). `{min,max}_part_len` must be powers of 2.
* `zita_convolver [min_part_len [max_part_len]] [~/]impulse_path`  
	Partitioned 32-bit FFT convolution using the zita-convolver library.
	Latency is equal to `min_part_len` (64 samples by default).
	`{min,max}_part_len` must be powers of 2 between 64 and 8192.
* `noise level`  
	Add TPDF noise. The `level` argument specifies the peak level of the noise
	(dBFS).
* `ladspa_host module_path plugin_label [control ...]`  
	Apply a LADSPA plugin. Supports any number of input/output ports (with
	the exception of zero output ports). Plugins with zero input ports will
	replace selected input channels with their output(s). If a plugin has one
	or zero input ports, it will be instantiated multiple times to handle
	multi-channel input.
	
	Controls which are not explicitly set or are set to `-` will use default
	values (if available).
	
	The `LADSPA_PATH` environment variable can be used to set the search path
	for plugins.
* `stats [ref_level]`  
	Display the DC offset, minimum, maximum, peak level (dBFS), RMS level
	(dBFS), crest factor (dB), peak count, peak sample, number of samples, and
	length (s) for each channel. If `ref_level` is given, peak and RMS levels
	relative to `ref_level` will be shown as well (dBr).

#### Exclamation mark

A `!` marks the effect that follows as "non-essential". If an effect is marked
non-essential and it fails to initialize, it will be skipped.

#### Selector syntax

	[[start][-[end]][,...]]

Example    | Description
---------- | --------------------------
`<empty>`  | all
`-`        | all
`2-`       | 2 to n
`-4`       | 0 through 4
`1,3`      | 1 and 3
`1-4,7,9-` | 1 through 4, 7, and 9 to n

#### Width suffixes

Suffix | Description
------ | ------------------------------
`q`    | Q-factor (default).
`s`    | Slope (shelving filters only).
`d`    | Slope in dB/octave (shelving filters only). Also changes the definition of `f0` from center frequency to corner frequency (like Room EQ Wizard and the Behringer DCX2496).
`o`    | Bandwidth in octaves.
`h`    | Bandwidth in Hz.
`k`    | Bandwidth in kHz.

#### File paths

* On the command line, relative paths are relative to `$PWD`.
* Within an effects file, relative paths are relative to the directory
  containing said effects file.
* The `~/` prefix will be expanded to the contents of `$HOME`.

#### Effects file syntax

* Arguments are delimited by whitespace.
* If the first non-whitespace character in a line is `#`, the line is ignored.
* The `\` character removes any special meaning of the next character.

Example:

	gain -10
	# This is a comment
	eq 1k 1.0 +10.0 eq 3k 3.0 -4.0
	lowshelf 90 0.7 +4.0

Effects files inherit a copy of the current channel selector. In other words,
if an effects chain is this:

	:2,4 @eq_file.txt eq 2k 1.0 -2.0

`eq_file.txt` will inherit the `2,4` selector, but any selector specified
within `eq_file.txt` will not affect the `eq 2k 1.0 -2.0` effect that comes
after it.

### Examples

Read `file.flac`, apply a bass boost, and write to alsa device `hw:2`:

	dsp file.flac -ot alsa -e s24_3 hw:2 lowshelf 60 0.5 +4.0

Plot amplitude vs frequency for a complex effects chain:

	dsp -pn gain -1.5 lowshelf 60 0.7 +7.8 eq 50 2.0 -2.7 eq 100 2.0 -3.9
		eq 242 1.0 -3.8 eq 628 2.0 +2.1 eq 700 1.5 -1.0
		lowshelf 1420 0.68 -12.5 eq 2500 1.3 +3.0 eq 3000 8.0 -1.8
		eq 3500 2.5 +1.4 eq 6000 1.1 -3.4 eq 9000 1.8 -5.6
		highshelf 10000 0.7 -0.5 | gnuplot

Implement an LR4 crossover at 2.2KHz, where output channels 0 and 2 are the
left and right woofers, and channels 1 and 3 are the left and right tweeters,
respectively:

	dsp stereo_file.flac -ot alsa -e s32 hw:3 remix 0 0 1 1 :0,2
		lowpass 2.2k 0.707 lowpass 2.2k 0.707 :1,3 highpass 2.2k 0.707
		highpass 2.2k 0.707 :

Apply effects from a file:

	dsp file.flac @eq.txt

### LADSPA frontend

#### Configuration

`ladspa_dsp` looks for configuration files in the following directories:

* `$XDG_CONFIG_HOME/ladspa_dsp`
* `$HOME/.config/ladspa_dsp` (if `$XDG_CONFIG_HOME` is not set)
* `/etc/ladspa_dsp`

To override the default directories, set the `LADSPA_DSP_CONFIG_PATH`
environment variable to the desired path(s) (colon-separated).

Each file that is named either `config` or `config_<name>` (where `<name>` is
any string) is loaded as a separate plugin. The plugin label is either
`ladspa_dsp` (for `config`) or `ladspa_dsp:<name>` (for `config_<name>`).

Configuration files are a simple key-value format. Leading whitespace is
ignored. The valid keys are:

* `input_channels`  
	Number of input channels. Default value is `1`. May be left unset unless
	you want individual control over each channel.
* `output_channels`  
	Number of output channels. Default value is `1`. Initialization will fail
	if this value is set incorrectly.
* `LC_NUMERIC`  
	Set `LC_NUMERIC` to the given value while building the effects chain. If
	the decimal separator defined by your system locale is something other than
	`.`, you should set this to `C` (to use `.` as the decimal separator) or an
	empty value (to use the decimal separator defined by your locale).
* `effects_chain`  
	String to build the effects chain. The format is the same as an effects
	file, but only a single line is interpreted.

Example configuration:

	# This is a comment
	input_channels=1
	output_channels=1
	LC_NUMERIC=C
	effects_chain=gain -3.0 lowshelf 100 1.0s +3.0 @/path/to/eq_file

Relative file paths in the `effects_chain` line are relative to the
directory in which the configuration file resides.

The loglevel can be set to `VERBOSE`, `NORMAL`, or `SILENT` through the
`LADSPA_DSP_LOGLEVEL` environment variable.

#### Usage example: Route alsa audio through ladspa_dsp

Put this in `~/.asoundrc`:

	pcm.dsp {
		type plug
		slave {
			format FLOAT
			rate unchanged
			channels unchanged
			pcm {
				type ladspa
				path "/usr/lib/ladspa"
				playback_plugins [{
					label "ladspa_dsp"
				}]
				slave.pcm {
					type plug
					slave {
						pcm "<hw_device>"
						rate unchanged
						channels unchanged
					}
				}
			}
		}
	}

Replace `<hw_device>` with the preferred output device (`hw:0`, for example).

If you need individual control over each channel, you need to set the number
of (output) channels:

	pcm.dsp {
		type plug
		slave {
			format FLOAT
			rate unchanged
			pcm {
				type ladspa
				channels <channels>
				path "/usr/lib/ladspa"
				playback_plugins [{
					label "ladspa_dsp"
				}]
				slave.pcm {
					type plug
					slave {
						pcm "<hw_device>"
						rate unchanged
						channels unchanged
					}
				}
			}
		}
	}

To make `dsp` the default device, append this to `~/.asoundrc`:

	pcm.!default {
		type copy
		slave.pcm "dsp"
	}

#### Usage example: Route pulseaudio audio through ladspa_dsp (tested with Ubuntu 18.04; contributed by shaffenmeister)

1. Prepare .asoundrc as stated above.
2. Determine pulseaudio master sink using `pacmd list sinks`. Use attribute
   `name` of the pulseaudio sink you plan to use
   (e.g. `alsa_output.pci-0000_00_14.2.analog-stereo`).
3. Execute `analyseplugin <path to LADSPA plugin>/ladspa_dsp.so` to determine
   plugin name and label.
4. Run `pacmd load-module module-ladspa-sink sink_name=ladspa_out
   sink_master=<master_sink> plugin=<plugin name> label=<plugin label>`.
5. Select new LADSPA sink as system sink (Ubuntu 18.04 Desktop:
   Settings > Sound > Output > LADSPA_Plugin `<plugin label>` on
   `<master sink>`).

Example:

	pacmd list sinks
	analyseplugin /usr/local/lib/ladspa/ladspa_dsp.so
	pacmd load-module module-ladspa-sink sink_name=ladspa_out sink_master=alsa_output.pci-0000_00_14.2.analog-stereo plugin=ladspa_dsp label=ladspa_dsp

##### Load LADSPA plugin as system default

To load the LADSPA module at system startup for all users include settings in `/etc/pulse/default.pa`:

	.ifexists module-ladspa-sink.so
	.nofail
	load-module module-ladspa-sink sink_name=ladspa_out sink_master=<master_sink> plugin=<plugin name> label=<plugin label>
	.fail
	.endif

##### Load LADSPA plugin as user default

To load the LADSPA module at user login include settings in
`~/.config/pulse/default.pa`:

	#!/usr/bin/pulseaudio -nF
	.include /etc/pulse/default.pa
	.ifexists module-ladspa-sink.so
	.nofail
	load-module module-ladspa-sink sink_name=ladspa_out sink_master=<master_sink> plugin=<plugin name> label=<plugin label>
	.fail
	.endif

**Note:** The resample effect cannot be used with the LADSPA frontend.

### Bugs

* No support for metadata.
* Some effects do not support plotting.

### License

This software is released under the ISC license.
