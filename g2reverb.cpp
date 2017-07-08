// -----------------------------------------------------------------------
//
//	Copyright (C) 2003-2013 Fons Adriaensen <fons@linuxaudio.org>
//
//	This program is free software; you can redistribute it and/or modify
//	it under the terms of the GNU General Public License as published by
//	the Free Software Foundation; either version 2 of the License, or
//	(at your option) any later version.
//
//	This program is distributed in the hope that it will be useful,
//	but WITHOUT ANY WARRANTY; without even the implied warranty of
//	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//	GNU General Public License for more details.
//
//	You should have received a copy of the GNU General Public License
//	along with this program; if not, write to the Free Software
//	Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
//
// -----------------------------------------------------------------------


#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <cstring>
#include "g2reverb_cpp.h"
#include "g2reverb.h"

extern "C" {
	#include "util.h"
}

void Diffuser::init (unsigned long size, sample_t c)
{
	_size = size;
	_data = new sample_t [size];
	_c = c;
	reset ();
}

void Diffuser::reset (void)
{
	memset (_data, 0, _size * sizeof (sample_t));
	_i = 0;
}

void Diffuser::fini (void)
{
	delete[] _data;
}

void QuadFDN::init (unsigned long size)
{
	_size = size;
	for (int j = 0; j < 4; j++)
	{
		_data [j] = new sample_t [size];
		_g [j] = 0;
		_d [j] = 0;
	}
	_c = 1;
	reset ();
}

void QuadFDN::reset (void)
{
	for (int j = 0; j < 4; j++)
	{
		memset (_data [j], 0, _size * sizeof (sample_t));
		_y [j] = 0;
	}
	_i = 0;
}

void QuadFDN::fini (void)
{
	for (int j = 0; j < 4; j++) delete[] _data [j];
}

void MTDelay::init (unsigned long size)
{
	_size = size;
	_data = new sample_t [size];
	for (int j = 0; j < 4; j++) _d [j] = 0;
	_c = 1;
	reset ();
}

void MTDelay::reset (void)
{
	memset (_data, 0, _size * sizeof (sample_t));
	for (int j = 0; j < 4; j++) _y [j] = 0;
	_z = 0;
	_i = 0;
}

void MTDelay::fini (void)
{
	delete[] _data;
}

Greverb::Greverb (unsigned long rate, sample_t max_roomsize) :
	_rate (rate),
	_max_roomsize (max_roomsize),
	_roomsize (0.0),
	_revbtime (0.0),
	_ipbandw (0.8),
	_damping (0.2),
	_refllev (0.3),
	_taillev (0.3)
{
	unsigned long n;

	n = (unsigned long)(rate * 0.015);
	_dif0.init (n, 0.450);
	_dif1.init (n, 0.450);
	_qfdn.init ((unsigned long)(rate * _max_roomsize / 340.0));
	n = (unsigned long)(_qfdn._size * 0.450);
	_del0.init (n);
	_del1.init (n);
	n = (unsigned long)(rate * 0.124);
	_dif1L.init ((unsigned long)(n * 0.2137), 0.5);
	_dif2L.init ((unsigned long)(n * 0.3753), 0.5);
	_dif3L.init (n - _dif1L._size - _dif2L._size, 0.5);
	_dif1R.init ((unsigned long)(n * 0.1974), 0.5);
	_dif2R.init ((unsigned long)(n * 0.3526), 0.5);
	_dif3R.init (n - _dif1R._size - _dif2R._size, 0.5);

	set_ipbandw (0.8);
	set_damping (0.2);
	set_roomsize (50.0);
	set_revbtime (3.0);
}

Greverb::~Greverb (void)
{
	_dif0.fini ();
	_dif1.fini ();
	_qfdn.fini ();
	_del0.fini ();
	_del1.fini ();
	_dif1L.fini ();
	_dif2L.fini ();
	_dif3L.fini ();
	_dif1R.fini ();
	_dif2R.fini ();
	_dif3R.fini ();
}

void Greverb::reset (void)
{
	// Clear all delay lines and filter states.
	// Current parameters are preserved.
	_dif0.reset ();
	_dif1.reset ();
	_qfdn.reset ();
	_del0.reset ();
	_del1.reset ();
	_dif1L.reset ();
	_dif2L.reset ();
	_dif3L.reset ();
	_dif1R.reset ();
	_dif2R.reset ();
	_dif3R.reset ();
}

void Greverb::set_roomsize (sample_t R)
{
	if (fabs (_roomsize - R) < 0.01) return;
	_roomsize = R;
	_qfdn._d [0] = (unsigned long)(_rate * R / 340.0);
	_qfdn._d [1] = (unsigned long)(_qfdn._d [0] * 0.816490);
	_qfdn._d [2] = (unsigned long)(_qfdn._d [0] * 0.707100);
	_qfdn._d [3] = (unsigned long)(_qfdn._d [0] * 0.632450);

	_del0._d [0] = (unsigned long)(_qfdn._d [0] * 0.100);
	_del0._d [1] = (unsigned long)(_qfdn._d [0] * 0.164);
	_del0._d [2] = (unsigned long)(_qfdn._d [0] * 0.270);
	_del0._d [3] = (unsigned long)(_qfdn._d [0] * 0.443);

	_del1._d [0] = (unsigned long)(_qfdn._d [0] * 0.087);
	_del1._d [1] = (unsigned long)(_qfdn._d [0] * 0.149);
	_del1._d [2] = (unsigned long)(_qfdn._d [0] * 0.256);
	_del1._d [3] = (unsigned long)(_qfdn._d [0] * 0.440);
	set_params ();
}

void Greverb::set_revbtime (sample_t T)
{
	if (T > MAX_REVBTIME) T = MAX_REVBTIME;
	if (T < MIN_REVBTIME) T = MIN_REVBTIME;
	if (fabs (_revbtime - T) < 0.01) return;
	_revbtime = T;
	set_params ();
}

void Greverb::set_ipbandw (sample_t B)
{
	if (B < 0.1) B = 0.1;
	if (B > 1.0) B = 1.0;
	_del1._c = _del0._c = _ipbandw = B;
}

void Greverb::set_damping (sample_t D)
{
	if (D < 0.0) D = 0.0;
	if (D > 0.9) D = 0.9;
	_damping = D;
	_qfdn._c = 1.0 - _damping;
}

void Greverb::set_params (void)
{
	double a;

	a = pow (0.001, 1.0 / (_rate * _revbtime));
	for (int j = 0; j < 4; j++)
	{
		_qfdn._g [j] = pow (a, (double)(_qfdn._d [j]));
	}
}

void Greverb::process (unsigned long n, sample_t *x0, sample_t *x1, sample_t *y0, sample_t *y1)
{
	sample_t z, z0, z1;

	while (n--)
	{
		_del0.process (_dif0.process (*x0 + 1e-20));
		_del1.process (_dif1.process (*x1 + 1e-20));
		_qfdn.process (_del0._y, _del1._y);
		z = _taillev * (_qfdn._y [0] + _qfdn._y [1] + _qfdn._y [2] + _qfdn._y [3]);
		z0 = _refllev * (_del0._y [0] - _del0._y [1] + _del0._y [2] - _del0._y [3]);
		z1 = _refllev * (_del1._y [0] - _del1._y [1] + _del1._y [2] - _del1._y [3]);
		*y0++ = _dif3L.process (_dif2L.process (_dif1L.process (z + z0))) + _dryslev * *x0++;
		*y1++ = _dif3R.process (_dif2R.process (_dif1R.process (z + z1))) + _dryslev * *x1++;
	}
}

/* ----- dsp wrapper ----- */

struct g2reverb_state {
	int c1, c2;
	Greverb *r;
};

void g2reverb_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	ssize_t i, k, samples = *frames * e->ostream.channels;
	struct g2reverb_state *state = (struct g2reverb_state *) e->data;
	for (i = 0; i < samples; i += e->ostream.channels) {
		for (k = 0; k < e->ostream.channels; ++k)
			if (k != state->c1 && k != state->c2)
				obuf[i + k] = ibuf[i + k];
		state->r->process(1, &ibuf[i + state->c1], &ibuf[i + state->c2], &obuf[i + state->c1], &obuf[i + state->c2]);
	}
}

void g2reverb_effect_reset(struct effect *e)
{
	struct g2reverb_state *state = (struct g2reverb_state *) e->data;
	state->r->reset();
}

/* FIXME: Add drain function */

void g2reverb_effect_destroy(struct effect *e)
{
	struct g2reverb_state *state = (struct g2reverb_state *) e->data;
	delete state->r;
	free(state);
}

struct effect * g2reverb_effect_init(struct effect_info *ei, struct stream_info *istream, char *channel_selector, const char *dir, int argc, char **argv)
{
	struct effect *e;
	struct g2reverb_state *state;
	sample_t roomsize = 50.0, revbtime = 3.0, ipbandw = 0.75, damping = 0.2, dryslev = 1.0, refllev = 0.125892541179, taillev = 0.0501187233627;
	int i, n_channels = 0, wet_only = 0;

	for (i = 0; i < istream->channels; ++i)
		if (GET_BIT(channel_selector, i))
			++n_channels;
	if (n_channels != 2) {
		LOG(LL_ERROR, "dsp: %s: error: number of input channels must be 2\n", argv[0]);
		return NULL;
	}

	i = 0;
	if (argc > i + 1 && strcmp(argv[i + 1], "-w") == 0) {
		wet_only = 1;
		++i;
	}
	if (argc > ++i) {
		roomsize = atof(argv[i]);
		CHECK_RANGE(roomsize > 0.0, "room_size", return NULL);
	}
	if (argc > ++i) {
		revbtime = atof(argv[i]);
		CHECK_RANGE(revbtime >= MIN_REVBTIME && revbtime <= MAX_REVBTIME, "reverb_time", return NULL);
	}
	if (argc > ++i) {
		ipbandw = atof(argv[i]);
		CHECK_RANGE(ipbandw >= 0.1 && ipbandw <= 1.0, "input_bandwidth", return NULL);
	}
	if (argc > ++i) {
		damping = atof(argv[i]);
		CHECK_RANGE(damping >= 0.0 && damping <= 0.9, "damping", return NULL);
	}
	if (argc > ++i)
		dryslev = pow(10.0, atof(argv[i]) / 20.0);
	if (argc > ++i)
		refllev = pow(10.0, atof(argv[i]) / 20.0);
	if (argc > ++i)
		taillev = pow(10.0, atof(argv[i]) / 20.0);
	if (argc > 9 || (!wet_only && argc > 8)) {
		LOG(LL_ERROR, "dsp: %s: usage: %s\n", argv[0], ei->usage);
		return NULL;
	}

	if (wet_only)
		dryslev = 0.0;
	LOG(LL_VERBOSE, "dsp: %s: info: wet_only=%s room_size=%.1f reverb_time=%.2f input_bandwidth=%.2f damping=%.2f dry_level=%.2f reflection_level=%.2f tail_level=%.2f\n",
		argv[0], (wet_only) ? "true" : "false", roomsize, revbtime, ipbandw, damping, 20.0 * log10(dryslev), 20.0 * log10(refllev), 20.0 * log10(taillev));

	state = (struct g2reverb_state *) calloc(1, sizeof(struct g2reverb_state));
	state->c1 = state->c2 = -1;
	for (i = 0; i < istream->channels; ++i) {  /* find input channel numbers */
		if (GET_BIT(channel_selector, i)) {
			if (state->c1 == -1)
				state->c1 = i;
			else
				state->c2 = i;
		}
	}
	state->r = new Greverb(istream->fs, roomsize);
	state->r->set_roomsize(roomsize);
	state->r->set_revbtime(revbtime);
	state->r->set_ipbandw(ipbandw);
	state->r->set_damping(damping);
	state->r->set_dryslev(dryslev);
	state->r->set_refllev(refllev);
	state->r->set_taillev(taillev);

	e = (struct effect *) calloc(1, sizeof(struct effect));
	e->name = ei->name;
	e->istream.fs = e->ostream.fs = istream->fs;
	e->istream.channels = e->ostream.channels = istream->channels;
	e->worst_case_ratio = e->ratio = 1.0;
	e->run = g2reverb_effect_run;
	e->reset = g2reverb_effect_reset;
	e->destroy = g2reverb_effect_destroy;
	e->data = state;

	return e;
}
