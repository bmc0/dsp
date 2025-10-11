#!/bin/bash

# This is a helper script for changing between DSP configurations more easily with a PulseAudio setup.

# It was created for the use case of daisy-chained headphone and speaker amps that are
# connected to a single optical output on a computer, but need different correction
# profiles depending on which device they're currently driving. Another use case would
# be multiple pairs of headphones that you swap out with the same amp/output. Basically
# any time that the computer doesn't know you switched hardware.

# Configuration
master="alsa_output.pci-0000_00_1f.3.iec958-stereo"
dspconfig="$HOME/.config/ladspa_dsp"
configfiles=($(cd $dspconfig && ls config_* | grep -oE "[^config_]+$"))

# Actual stuff happening
if ! pulseaudio --check; then
	pulseaudio --start
fi

if [[ $1 = "--none" ]]; then
	pacmd set-default-sink $master
	echo "EQ disabled"
elif [[ $1 = "--config" ]]; then
	for config in ${configfiles[@]}; do
		echo "load-module module-ladspa-sink sink_name=dsp_$config sink_master=$master plugin=ladspa_dsp label=ladspa_dsp:$config"
	done
elif [[ $1 = "--list" ]]; then
	for config in ${configfiles[@]}; do
		echo $config
	done
elif [[ $1 = "--current" ]]; then
	pacmd list-sinks | awk '/* index/{getline; print}'
	# TODO make this better
elif [[ -f "$dspconfig/config_$1" ]]; then
	if ! pacmd list-sinks | grep -qo "name: <dsp_$1>"; then
		pacmd load-module module-ladspa-sink sink_name=dsp_$1 sink_master=$master plugin=ladspa_dsp label=ladspa_dsp:$1
	fi
	pacmd set-default-sink dsp_$1
	echo "Set config $1"
	pacmd list-sink-inputs | grep index | while read line; do
		echo $line | cut -f2 -d' '
		pacmd move-sink-input $(echo $line | cut -f2 -d' ') dsp_$1
	done
else
	echo "Usage: $( basename $0 ) [config name or options]
	--list		Show available config files
	--current	Show current default sink
	--none		Set default sink to master
	--config	Print PulseAudio configuration for available configs"
	exit
fi
