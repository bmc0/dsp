#!/bin/sh

#
# Generate dsp effects chain from a Room EQ Wizard filter settings file
#
# Usage:
#     rew_to_dsp.sh [file ...]
#

AWK_SCRIPT='
{
	if ($1=="Preamp:" && $2!=0) {
		print "gain", $2
	}
	else if ($1=="Filter" && $3=="ON" && $9!=0) {
		gsub(",", "", $6)
		if ($4=="PK")
			print "eq", $6, $12, $9
		else if ($4=="LP")
			print "lowpass", $6, "0.7071"
		else if ($4=="HP")
			print "highpass", $6, "0.7071"
		else if ($4=="LPQ")
			print "lowpass", $6, $9
		else if ($4=="HPQ")
			print "highpass", $6, $9
		else if ($4=="NO")
			print "notch", $6, "30.0"
		else if ($4=="LS" && $5=="Fc")
			print "lowshelf", $6, "0.9s", $9
		else if ($4=="HS" && $5=="Fc")
			print "highshelf", $6, "0.9s", $9
		else if ($4=="LS" && $5~/[0-9]+dB/)
			print "lowshelf", $7, substr($5, 1, length($5)-2)"d", $10
		else if ($4=="HS" && $5~/[0-9]+dB/)
			print "highshelf", $7, substr($5, 1, length($5)-2)"d", $10
		else if ($4=="AP")
			print "allpass", $6, $9
	}
}'

FILTERS=$(awk -F' ' "$AWK_SCRIPT" "$@")
echo $FILTERS
