### About

dsp is an audio processing program with an interactive mode.

### Building

#### Dependencies

* GNU Make
* pkg-config

#### Optional dependencies

* fftw3: For `resample`, `fir`, `fir_p`, and `hilbert` effects.
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

	dsp [options] path ... [effect [args]] ...

### Options

#### Global options

Flag        | Description
----------- | --------------------------------------------------------------------------
`-h`        | Show help text.
`-b frames` | Block size (must be given before the first input).
`-i`        | Force interactive mode.
`-I`        | Disable interactive mode.
`-q`        | Disable progress display.
`-s`        | Silent mode.
`-v`        | Verbose mode.
`-d`        | Force dithering.
`-D`        | Disable dithering.
`-E`        | Don't drain effects chain before rebuilding.
`-p`        | Plot effects chain magnitude response instead of processing audio.
`-P`        | Same as `-p`, but also plot phase response.
`-V`        | Verbose progress display.
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
`-R ratio`        | Buffer ratio.
`-n`              | Equivalent to `-t null null`.

### Inputs and Outputs

#### Supported input/output types

Type    | Modes | Encodings
------- | ----- | -------------------------------------------------------------------------------------
null    | rw    | sample_t
sgen    | r     | sample_t
sndfile | r     | autodetected
wav     | rw    | s16 u8 s24 s32 float double mu-law a-law ima_adpcm ms_adpcm gsm6.10 nms_adpcm_16 nms_adpcm_24 nms_adpcm_32 g721_32 mpeg2.3
aiff    | rw    | s16 s8 u8 s24 s32 float double mu-law a-law ima_adpcm gsm6.10 dwvw_12 dwvw_16 dwvw_24
au      | rw    | s16 s8 s24 s32 float double mu-law a-law g721_32 g723_24 g723_40
raw     | rw    | s16 s8 u8 s24 s32 float double mu-law a-law gsm6.10 vox_adpcm nms_adpcm_16 nms_adpcm_24 nms_adpcm_32 dwvw_12 dwvw_16 dwvw_24
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
sd2     | rw    | s16 s8 s24 s32
flac    | rw    | s16 s8 s24
caf     | rw    | s16 s8 s24 s32 float double mu-law a-law alac_16 alac_20 alac_24 alac_32
wve     | rw    | a-law
ogg     | rw    | vorbis opus
mpc2k   | rw    | s16
rf64    | rw    | s16 u8 s24 s32 float double mu-law a-law
sf/mpeg | rw    | mpeg1.1 mpeg1.2 mpeg2.3
ffmpeg  | r     | autodetected
alsa    | rw    | s16 u8 s8 s24 s24_3 s32 float double
ao      | w     | s16 u8 s32
mp3     | r     | mad_f
pcm     | rw    | s16 u8 s8 s24 s24_3 s32 float double
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

#### Complete effects list

* `lowpass_1 f0[k]`  
	First-order lowpass filter.
* `highpass_1 f0[k]`  
	First-order highpass filter.
* `allpass_1 f0[k]`  
	First-order allpass filter.
* `lowshelf_1 f0[k] gain`  
	First-order lowshelf filter.
* `highshelf_1 f0[k] gain`  
	First-order highshelf filter.
* `lowpass_1p f0[k]`  
	Single pole lowpass (EWMA) filter.
* `lowpass f0[k] width[q|o|h|k]`  
	Second-order lowpass filter.
* `highpass f0[k] width[q|o|h|k]`  
	Second-order highpass filter.
* `bandpass_skirt f0[k] width[q|o|h|k]`  
	Second-order bandpass filter with constant skirt gain.
* `bandpass_peak f0[k] width[q|o|h|k]`  
	Second-order bandpass filter with constant peak gain.
* `notch f0[k] width[q|o|h|k]`  
	Second-order notch filter.
* `allpass f0[k] width[q|o|h|k]`  
	Second-order allpass filter.
* `eq f0[k] width[q|o|h|k] gain`  
	Second-order peaking filter.
* `lowshelf f0[k] width[q|s|d|o|h|k] gain`  
	Second-order lowshelf filter.
* `highshelf f0[k] width[q|s|d|o|h|k] gain`  
	Second-order highshelf filter.
* `lowpass_transform fz[k] qz fp[k] qp`  
	Second-order lowpass transformation filter. Cancels the poles defined by
	`fz` and `qz` and replaces them with new poles defined by `fp` and `qp`.
	Gain is unity at DC.
* `highpass_transform fz[k] qz fp[k] qp`  
	Second-order highpass transformation filter. Also known as a Linkwitz
	transform (see http://www.linkwitzlab.com/filters.htm#9). Same as
	`lowpass_transform` except the gain is unity at Fs/2.
* `linkwitz_transform fz[k] qz fp[k] qp`  
	Alias for `highpass_transform`.
* `deemph`  
	Compact Disc de-emphasis filter.
* `biquad b0 b1 b2 a0 a1 a2`  
	Biquad filter.
* `gain gain_dB`  
	Gain adjustment in decibels.
* `mult multiplier`  
	Multiplies each sample by `multiplier`.
* `add value`  
	Applies a DC shift.
* `crossfeed f0[k] separation`  
	Simple crossfeed for headphones. Very similar to Linkwitz/Meier/CMoy/bs2b
	crossfeed.
* `matrix4 [options] [surround_level]`  
	2-to-4 channel (2 front and 2 surround) active matrix upmixer designed for
	plain (i.e. unencoded) stereo material.

	The intended speaker configuration is fronts at ±30° and surrounds between
	±60° and ±120°. The surround speakers must be calibrated correctly in
	level and frequency response for best results. The surrounds should be
	delayed by about 10-25ms (acoustically) relative to the fronts. No
	frequency contouring is done internally, so applying low pass and/or
	shelving filters to the surround outputs is recommended:

	```
	matrix4 surround_delay=15m -6 :2,3 lowpass_1 10k :
	```

	The settings shown above (-6dB surround level, 15ms delay, and 10kHz
	rolloff) are a good starting point, but may be adjusted to taste. The
	default `surround_level` is -6dB. Applying the `decorrelate` effect to the
	surround outputs (optionally with the `-m` flag) seems to further improve
	the spatial impression (note: adjust `surround_delay` to compensate for
	the `decorrelate` effect's group delay).

	The front outputs replace the original input channels and the surround
	outputs are appended to the end of the channel list.

	Options are given as a comma-separated list. Recognized options are:

	* `no_dir_boost`  
		Disable directional boost of front channels.
	* `show_status`  
		Show a status line (slightly broken currently, but still useful for
		debugging).
	* `signal`  
		Toggle the effect when `effect.signal()` is called.
	* `linear_phase` (`matrix4_mb` only)  
		Apply an FIR filter to correct the phase distortion caused by the IIR
		filter bank. Has no effect with `matrix4`. Requires the `fir` effect.
	* `surround_delay=delay[s|m|S]`  
		Surround output delay. Default is zero.

* `matrix4_mb [options] [surround_level]`  
	Like the `matrix4` effect, but divides the input into ten individually
	steered bands in order to improve separation of concurrent sound sources.
	See the `matrix4` effect description for more information.
* `remix selector|. ...`  
	Select and mix input channels into output channels. Each selector argument
	specifies the input channels to be mixed to produce an output channel. `.`
	selects no input channels. For example, `remix 0,1 2,3` mixes input
	channels 0 and 1 into output channel 0, and input channels 2 and 3 into
	output channel 1.  `remix -` mixes all input channels into a single output
	channel. The active channel selector is used as an input channel mask for
	the selector arguments.
* `st2ms`
	Convert stereo to mid/side.
* `ms2st`
	Convert mid/side to stereo.
* `delay [-f [order]] delay[s|m|S]`  
	Delay line. The unit for the delay argument depends on the suffix used:
	`s` is seconds (the default), `m` is milliseconds, and `S` is samples. If
	`delay` is negative, a positive delay is applied to all channels which are
	**not** selected (except when plotting—an actual negative delay is
	possible in that case).

	By default, the delay is rounded to whole samples. The `-f` option enables
	fractional delay using Thiran allpass interpolation. The `order` argument
	sets the allpass filter order and may be any integer from 1 through 50. The
	default value is 5.
* `resample [bandwidth] fs[k]`  
	Sinc resampler. Ignores the channel selector.
* `fir [input_options] [file:][~/]filter_path|coefs:list[/list...]`  
	Non-partitioned 64-bit direct or FFT convolution. Latency is zero for
	filters up to 16 taps. For longer filters, the latency is equal to the
	`fft_len` reported in verbose mode. Each `list` is a comma-separated list
	of coefficients for one filter channel. Missing values are filled with
	zeros.

	The `input_options` are useful mostly when loading raw (headerless) input
	files and are as follows:

	Flag              | Description
	----------------- | -----------------------------
	`-t type`         | Type.
	`-e encoding`     | Encoding.
	`-B/L/N`          | Big/little/native endian.
	`-r frequency[k]` | Sample rate.
	`-c channels`     | Number of channels.

	By default, the sample rate of the filter must match that of the effect.
	Mismatches may be ignored by setting the sample rate to "any".

* `fir_p [input_options] [max_part_len] [file:][~/]filter_path|coefs:list[/list...]`  
	Zero-latency non-uniform partitioned 64-bit direct/FFT convolution. Usually
	a bit slower than the `zita_convolver` effect except for very long filters
	on some hardware. `max_part_len` must be a power of 2 and has a default
	value of 16384. Each `list` is a comma-separated list of coefficients for
	one filter channel. Missing values are filled with zeros.

	See the `fir` effect description an explanation of the `input_options`.
* `zita_convolver [input_options] [min_part_len [max_part_len]] [file:][~/]filter_path|coefs:list[/list...]`  
	Partitioned 32-bit FFT convolution using the zita-convolver library.
	Latency is equal to `min_part_len` (64 samples by default).
	`{min,max}_part_len` must be powers of 2 between 64 and 8192. Each `list`
	is a comma-separated list of coefficients for one filter channel. Missing
	values are filled with zeros.

	See the `fir` effect description an explanation of the `input_options`.
* `hilbert [-p] taps`  
	Simple FIR approximation of a Hilbert transform. The number of taps must be
	odd. Bandwidth is controlled by the number of taps. If `-p` is given, the
	`fir_p` convolution engine is used instead of the default `fir` engine.
* `decorrelate [-m] [stages]`  
	Allpass decorrelator as described in "Frequency-Dependent Schroeder
	Allpass Filters" by Sebastian J. Schlecht (doi:10.3390/app10010187).
	If `-m` is given, the same filter parameters are used for all input
	channels. The default number of stages is 5, which results in an
	average group delay of about 9.5ms at high frequencies.
* `noise level[b]`  
	Add TPDF noise. The `level` argument specifies the peak level of the noise
	in dBFS if no suffix is given, or the effective precision in bits if the
	`b` suffix is given.
* `dither [shape] [[quantize_bits] bits]`  
	Apply dither with optional noise shaping. The `shape` argument determines
	the type of dither and the noise shaping filter (if any):

	`shape`    | Description
	---------- | ----------------------
	`flat`     | Flat TPDF with no feedback (default).
	`sloped`   | Flat TPDF with feedback. First-order highpass response.
	`sloped2`  | Sloped TPDF with feedback. Stronger HF emphasis than `sloped`.
	`lipshitz` | 5-tap E-weighted curve from [1]. Notches around 4k and 12k.
	`wan3`     | 3-tap F-weighted curve from [2]. Notch around 4k.
	`wan9`     | 9-tap F-weighted curve from [2]. Notches around 3.5k and 12k.

	The `bits` argument sets the dither level in bits. The `quantize_bits`
	argument sets the number of levels to quantize to. The default setting for
	both is `auto`. If `bits` is not `auto`, dither is applied at the specified
	bit depth regardless of the output sample format. `bits` may be any number.
	`quantize_bits` must be an integer between 2 and 32. If `quantize_bits` is
	not given, it is set to the same value as `bits` (rounded to the nearest
	integer).

	**Note:** Currently, `auto` will not work correctly with `ladspa_dsp` or if
	loaded via `watch`. A default value of 16 is used in those cases.

	[1] S. P. Lipshitz, J. Vanderkooy, and R. A. Wannamaker,
	"Minimally Audible Noise Shaping," J. AES, vol. 39, no. 11,
	November 1991  
	[2] R. A. Wannamaker, "Psychoacoustically Optimal Noise Shaping,"
	J. AES, vol. 40, no. 7/8, July 1992

* `ladspa_host module_path plugin_label [control ...]`  
	Apply a LADSPA plugin. Supports any number of input/output ports (with
	the exception of zero output ports). If a plugin has one or zero input
	ports, it will be instantiated multiple times to handle multi-channel
	input.
	
	Controls which are not explicitly set or are set to `-` will use default
	values (if available).
	
	The `LADSPA_PATH` environment variable can be used to set the search path
	for plugins.
* `stats [ref_level]`  
	Display the DC offset, minimum, maximum, peak level (dBFS), RMS level
	(dBFS), crest factor (dB), peak count, peak sample, number of samples, and
	length (s) for each channel. If `ref_level` is given, peak and RMS levels
	relative to `ref_level` will be shown as well (dBr).
* `watch [-e] [~/]path`  
	Load effects from a file into a sub-chain and reload if the file is
	modified. Other than the automatic reload, the behavior is similar to
	sourcing a file using the `@` directive (see "Effects Files"). Some
	restrictions apply to automatic reload:

	* The new sub-chain must have the same output sample rate and number of
	  channels as the previous sub-chain.
	* The new sub-chain must not require larger buffers than the previous
	  sub-chain.

	If these conditions are not met, the new sub-chain will not be applied and
	an error message will be printed.

	Currently, this effect polls for file modifications once per second.
	Support `inotify` events my be added in the future. Ideally, file
	modifications should be atomic (i.e. by writing to a temporary file, then
	`rename(3)`-ing it over top of the original file). If this is not possible,
	the `-e` flag may be given, which enforces an end-of-file marker in order
	to detect partially-written files. This marker, `#EOF#`, must be placed at
	the beginning of a line and may only be followed by whitespace characters.

#### Selector syntax

Example    | Description
---------- | --------------------------
`<empty>`  | all
`-`        | all
`2-`       | 2 to n
`-4`       | 0 through 4
`1,3`      | 1 and 3
`1-4,7,9-` | 1 through 4, 7, and 9 to n

**Note:** There is no difference between `1,3` and `3,1`. Order is not
preserved.

#### Filter width suffixes

Suffix | Description
------ | ------------------------------
`q`    | Q-factor (default).
`s`    | Slope (shelving filters only).
`d`    | Slope in dB/octave (shelving filters only).
`o`    | Bandwidth in octaves.
`h`    | Bandwidth in Hz.
`k`    | Bandwidth in kHz.

**Note:** The `d` width suffix also changes the definition of `f0` from center
frequency to corner frequency (like Room EQ Wizard and the Behringer DCX2496).

#### File paths

On the command line, relative paths are relative to `$PWD`. Within an effects
file, relative paths are relative to the directory containing said effects
file. The `~/` prefix will be expanded to the contents of `$HOME`.

#### Channel selectors and masks

A colon (`:`) followed by a selector (see "Selector syntax") specifies the
input channels for effects that follow. For example,

	:0,2 eq 1k 1.0 -6

will apply an `eq` effect to channels 0 and 2. If an effect changes the total
number of channels, the last channel selector given is parsed again. Additional
channels are not added unless the selector includes an unbounded range.

Channel numbers refer to the channels in the active channel mask, which is a
property of the containing block. Blocks may be created using braces
(`{ ... }`) or by sourcing a file (see "Effects files"). The channel mask is
derived from the active channel selector at creation. For example,

	:1,3 { :0 gain -6 :1 gain +6 }

creates a block with the mask `1,3`. Within the block, `:0` selects the first
channel in the mask (channel 1), and `:1` selects the second channel in the
mask (channel 3). Channel selectors have block scope.

Channels are automatically added or removed from the active channel mask if an
effect changes the total number of channels. Additional channels are always
appended to the end of the channel list.

#### Effects files

Files may be sourced using the `@` directive: `@[~/]path/to/file`. See "File
paths" for more information about how paths are interpreted. Note that sourcing
a file implicitly creates a block (see "Channel selectors and masks"). Within a
file, lines in which the first non-whitespace character is `#` are ignored. A
backslash (`\`) may be used to escape whitespace, `#`, or `\`. Example:

	gain -4.0
	# This is a comment
	lowshelf 90 1s +4 eq 3k 1.5 -3

#### Other directives

An exclamation mark (`!`) allows initialization failure of the effect that
follows.

#### FFTW wisdom

Effects utilizing FFTW3 can optionally load and save wisdom. For `dsp`, set
`$DSP_FFTW_WISDOM_PATH`. `ladspa_dsp` uses `$LADSPA_DSP_FFTW_WISDOM_PATH`
instead. If a path is set, FFTW plans are created with the FFTW_MEASURE flag.
Accumulated wisdom is written on exit.

### Examples

Read `file.flac`, apply a bass boost, and write to alsa device `hw:2`:

	dsp file.flac -ot alsa -e s24_3 hw:2 lowshelf 60 0.5 +4

Plot the magnitude vs frequency response of an effects chain:

	dsp -pn [effect [args]] ... | gnuplot

Implement an LR4 crossover at 2.2KHz, where output channels 0 and 1 are the
left and right tweeters, and channels 2 and 3 are the left and right woofers,
respectively:

	dsp stereo_file.flac -ot alsa -e s32 hw:3 remix 0 1 0 1
	  :0,1 highpass 2.2k 0.7071 highpass 2.2k 0.7071 :
	  :2,3 lowpass 2.2k 0.7071 lowpass 2.2k 0.7071 :

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
	Number of output channels. Default value is `1`. This parameter is not
	currently set automatically because the number of LADSPA ports must be
	known before the effects chain is built. Initialization will fail if it
	does not match the effects chain.
* `LC_NUMERIC`  
	Set `LC_NUMERIC` to the given value while building the effects chain.
	Default value is `C`, which gives consistent number parsing behavior
	regardless of the system locale and LADSPA host behavior. Setting this to
	an empty value uses the default system locale. The special value `none`
	leaves `LC_NUMERIC` up to the LADSPA host (not generally recommended).
* `effects_chain`  
	String to build the effects chain. The format is the same as an effects
	file, but only a single line is interpreted.

Example configuration:

	# This is a comment
	input_channels=1
	output_channels=1
	LC_NUMERIC=C
	effects_chain=gain -3 lowshelf 100 1s +3 @/path/to/eq_file

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
* When plotting an effects chain containing the `noise` effect, a different
  random sequence is generated for each output channel regardless of whether the
  noise should be correlated between outputs. Summing correlated noise works
  correctly.

### License

This software is released under the ISC license.
