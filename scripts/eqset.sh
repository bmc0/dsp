#!/bin/bash

# This is a helper script for changing between DSP configurations more easily with a PulseAudio setup.

# It was created for the use case of daisy-chained headphone and speaker amps that are
# connected to a single optical output on a computer, but need different correction
# profiles depending on which device they're currently driving. Another use case would
# be multiple pairs of headphones that you swap out with the same amp/output. Basically
# any time that the computer doesn't know you switched hardware.


# you'll probably need to change this
master="alsa_output.pci-0000_00_1f.3.iec958-stereo"
# these are probably fine
dspconfig="$HOME/.config/ladspa_dsp"
pulseconfig="$HOME/.config/pulse"
configtext="load-module module-ladspa-sink sink_name=dsp master=$master plugin=ladspa_dsp
set-default-sink dsp"

# begin doing stuff
cd $dspconfig
if [ -z $1 ]; then
	if [ -f config ]; then
		rm config
	fi
	echo "EQ disabled"
elif [ -f $1 ]; then
	ln -sf $1 config
	echo "config $1 linked"
elif [ $1 = "--help" ]; then
	echo "Usage: `basename $0` [config]
	If no config argument is passed, the EQ configuration will be removed.
	Otherwise, if the file exists, it will be linked and the DSP module configured."
	exit
else
	echo "specified config doesn't exist"
	exit
fi

cd $pulseconfig
if [ -f $dspconfig/config ] && ! grep -qs "$configtext" default.pa; then
	echo "$configtext" >> default.pa
elif [ -z $1 ]; then
	if grep -qs "$configtext" default.pa; then
		grep -v "$configtext" default.pa > temp
		mv temp default.pa
	elif [ -f default.pa ] && cat default.pa | wc -c; then
		rm default.pa
	fi
fi

if pulseaudio --check; then
	if pacmd list-sinks | grep -q "name: <dsp>"; then
		pacmd unload-module module-ladspa-sink
	fi
	if [ -f $dspconfig/$1 ]; then
		pacmd load-module module-ladspa-sink sink_name=dsp master=$master plugin=ladspa_dsp label=ladspa_dsp
		pacmd set-default-sink dsp
	fi
fi

