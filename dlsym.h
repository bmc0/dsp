/*
 * This file is part of dsp.
 *
 * Copyright (c) 2025 Michael Barbour <barbour.michael.0@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

#ifndef DSP_DLSYM_H
#define DSP_DLSYM_H

#include <dlfcn.h>
#include "dsp.h"

#define DLSYM_PROTOTYPE(x) typeof(x) * sym_ ## x
#define DLSYM_RESOLVE(handle, symbol) (sym_ ## symbol = try_dlsym(handle, #symbol))

static inline void * try_dlopen(const char *path, int flags)
{
	void *dl = dlopen(path, flags);
	if (!dl)
		LOG_FMT(LL_ERROR, "error: dlopen(): %s", dlerror());
	return dl;
}

static inline void * try_dlsym(void *handle, const char *symbol)
{
	void *sp = dlsym(handle, symbol);
	if (!sp)
		LOG_FMT(LL_ERROR, "error: dlsym(): %s", dlerror());
	return sp;
}

#endif
