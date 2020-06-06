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
	else if ($1=="Channel:") {
		printf(":%s", $2-1)
		for (i=3; i<=NF; ++i)
			printf(",%s", $i-1)
		print ""
	}
	else if ($1=="Convolution:") {
		print "zita_convolver", $2
	}
	else if ($1=="Delay:") {
		printf("delay %s", $2)
		if ($3=="ms") printf("m")
		else if ($3=="samples") printf("S")
		print ""
	}
	else if ($1=="Filter" && $3=="ON") {
		if ($4=="PK" && $9!=0) {
			gsub(",", "", $6)
			print "eq", $6, $12, $9
		}
		else if ($4=="LP") {
			gsub(",", "", $6)
			print "lowpass", $6, "0.7071"
		}
		else if ($4=="HP") {
			gsub(",", "", $6)
			print "highpass", $6, "0.7071"
		}
		else if ($4=="LP1") {
			gsub(",", "", $6)
			print "lowpass_1", $6
		}
		else if ($4=="HP1") {
			gsub(",", "", $6)
			print "highpass_1", $6
		}
		else if ($4=="LPQ") {
			gsub(",", "", $6)
			print "lowpass", $6, $9
		}
		else if ($4=="HPQ") {
			gsub(",", "", $6)
			print "highpass", $6, $9
		}
		else if ($4=="NO") {
			gsub(",", "", $6)
			print "notch", $6, "30.0"
		}
		else if ($4=="LS" && $5=="Fc" && $9!=0) {
			gsub(",", "", $6)
			print "lowshelf", $6, "0.9s", $9
		}
		else if ($4=="HS" && $5=="Fc" && $9!=0) {
			gsub(",", "", $6)
			print "highshelf", $6, "0.9s", $9
		}
		else if ($4=="LS" && $5~/[0-9]+dB/ && $10!=0) {
			gsub(",", "", $7)
			print "lowshelf", $7, substr($5, 1, length($5)-2)"d", $10
		}
		else if ($4=="HS" && $5~/[0-9]+dB/ && $10!=0) {
			gsub(",", "", $7)
			print "highshelf", $7, substr($5, 1, length($5)-2)"d", $10
		}
		else if ($4=="LSQ" && $9!=0) {
			gsub(",", "", $6)
			print "lowshelf", $6, $12, $9
		}
		else if ($4=="HSQ" && $9!=0) {
			gsub(",", "", $6)
			print "highshelf", $6, $12, $9
		}
		else if ($4=="AP") {
			gsub(",", "", $6)
			print "allpass", $6, $9
		}
	}
}'

FILTERS=$(awk -F' ' "$AWK_SCRIPT" "$@")
echo $FILTERS
