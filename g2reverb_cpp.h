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


#ifndef _G2REVERB_CPP_H
#define _G2REVERB_CPP_H

extern "C" {
	#include "dsp.h"
}

class Diffuser
{
private:

	friend class Greverb;

	void init (unsigned long size, sample_t c);
	void reset (void);
	void fini (void);

	sample_t process (sample_t x)
	{
		sample_t w;

		w = x - _c * _data [_i];
		x = _data [_i] + _c * w;
		_data [_i] = w;
		if (++_i == _size) _i = 0;
		return x;
	}

	sample_t        *_data;
	unsigned long   _size;
	unsigned long   _i;      // sample index
	sample_t        _c;      // feedback
};

class QuadFDN
{
private:

	friend class Greverb;

	void init (unsigned long size);
	void reset (void);
	void fini (void);

	void process (sample_t *x0, sample_t *x1)
	{
		int j;
		long k;

		for (j = 0; j < 4; j++)
		{
			k = _i - _d [j];
			if (k < 0) k += _size;
			_y [j] += _c * (_g [j] * _data [j][k] - _y [j]);
		}
		_data [0][_i] = x0 [0] + x1 [0] + 0.5 * ( _y [0] + _y [1] - _y [2] - _y [3]);
		_data [1][_i] = x0 [1] + x1 [1] + 0.5 * ( _y [0] - _y [1] - _y [2] + _y [3]);
		_data [2][_i] = x0 [2] + x1 [2] + 0.5 * (-_y [0] + _y [1] - _y [2] + _y [3]);
		_data [3][_i] = x0 [3] + x1 [3] + 0.5 * ( _y [0] + _y [1] + _y [2] + _y [3]);
		if (++_i == _size) _i = 0;
	}

	sample_t        *_data [4];
	unsigned long   _size;
	sample_t        _g [4];  // gain
	sample_t        _y [4];  // filtered output
	unsigned long   _d [4];  // delay
	unsigned long   _i;      // input index
	sample_t        _c;      // damping
};

class MTDelay
{
private:

	friend class Greverb;

	void init (unsigned long size);
	void reset (void);
	void fini (void);

	void process (sample_t x)
	{
		int j;
		long k;

		for (j = 0; j < 4; j++)
		{
			k = _i - _d [j];
			if (k < 0) k += _size;
			_y [j] = _data [k];
		}
		_z += _c * (x - _z);
		_data [_i] = _z;
		if (++_i == _size) _i = 0;
	}

	sample_t        *_data;
	unsigned long   _size;
	sample_t        _y [4];  // output
	unsigned long   _d [4];  // delay
	unsigned long   _i;      // input index
	sample_t        _c;      // damping;
	sample_t        _z;      // filter state
};

#define MIN_REVBTIME 0.01  /* was 1, seems a bit long... */
#define MAX_REVBTIME 20

class Greverb
{
public:

	Greverb (unsigned long rate, sample_t max_roomsize);
	~Greverb (void);

	void reset (void);
	void set_roomsize (sample_t roomsize);
	void set_revbtime (sample_t revbtime);
	void set_ipbandw (sample_t ipbandw);
	void set_damping (sample_t damping);
	void set_dryslev (sample_t refllev) { _dryslev = refllev; }
	void set_refllev (sample_t refllev) { _refllev = refllev; }
	void set_taillev (sample_t taillev) { _taillev = taillev; }
	void process (unsigned long n, sample_t *x0, sample_t *x1, sample_t *y0, sample_t *y1);

private:

	void set_params (void);

	unsigned long _rate;

	sample_t  _max_roomsize;
	sample_t  _roomsize;
	sample_t  _revbtime;
	sample_t  _ipbandw;
	sample_t  _damping;
	sample_t  _dryslev;
	sample_t  _refllev;
	sample_t  _taillev;

	Diffuser  _dif0;
	Diffuser  _dif1;
	MTDelay   _del0;
	MTDelay   _del1;
	QuadFDN   _qfdn;
	Diffuser  _dif1L;
	Diffuser  _dif2L;
	Diffuser  _dif3L;
	Diffuser  _dif1R;
	Diffuser  _dif2R;
	Diffuser  _dif3R;
};

#endif
