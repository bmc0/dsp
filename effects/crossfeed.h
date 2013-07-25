#ifndef _EFFECTS_CROSSFEED_H
#define _EFFECTS_CROSSFEED_H

#include "../dsp.h"
#include "../effect.h"
#include "biquad.h"

struct crossfeed_state {
	int type;
	double direct_gain;
	double cross_gain;
	struct biquad_state f0_c0;  /* lowpass for channel 0 */
	struct biquad_state f0_c1;  /* lowpass for channel 1 */
	struct biquad_state f1_c0;  /* highpass for channel 0 */
	struct biquad_state f1_c1;  /* highpass for channel 1 */
};

void crossfeed_init(struct crossfeed_state *, double, double);
struct effect * crossfeed_effect_init(struct effect_info *, int, char **);

#endif
