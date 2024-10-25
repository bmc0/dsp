/*
 * This file is part of dsp.
 *
 * Copyright (c) 2017-2024 Michael Barbour <barbour.michael.0@gmail.com>
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

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <ladspa.h>
#include <ltdl.h>
#include "ladspa_host.h"
#include "util.h"

#define DEBUG_CHANNEL_MAPPING 0
#if DEBUG_CHANNEL_MAPPING
	#define CM_DEBUG(...) do { if (!state->cm_printed) fprintf(stderr, __VA_ARGS__); } while(0)
#else
	#define CM_DEBUG(...)
#endif

struct ladspa_host_state {
	lt_dlhandle lt_handle;
	const LADSPA_Descriptor *desc;
	LADSPA_Handle *handles;
	int n_handles;
	LADSPA_Data **in, **out, *control;
	int n_in, n_out;
	ssize_t buf_size;
	#if DEBUG_CHANNEL_MAPPING
		int cm_printed;
	#endif
};

sample_t * ladspa_host_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	ssize_t f = 0, len;
	struct ladspa_host_state *state = (struct ladspa_host_state *) e->data;

	while (f < *frames) {
		len = (*frames - f > state->buf_size) ? state->buf_size : *frames - f;
		CM_DEBUG("\n%s: %s: info: begin channel map\n", dsp_globals.prog_name, e->name);
		for (int ch = 0, in_port = 0; ch < e->istream.channels; ++ch) {
			if (GET_BIT(e->channel_selector, ch)) {
				CM_DEBUG("%s: %s: info: channel map: in_port[%d] <- ibuf[%d]\n", dsp_globals.prog_name, e->name, in_port, ch);
				for (ssize_t i = 0; i < len; ++i)
					state->in[in_port][i] = (LADSPA_Data) ibuf[(f + i) * e->istream.channels + ch];
				++in_port;
			}
		}
		for (int i = 0; i < state->n_handles; ++i)
			state->desc->run(state->handles[i], (unsigned long) len);
		for (int out_ch = 0, out_port = 0, in_ch = 0; out_ch < e->ostream.channels; ++out_ch, ++in_ch) {
			if (in_ch >= e->istream.channels || GET_BIT(e->channel_selector, in_ch)) {
				if (out_port < state->n_out) {
					CM_DEBUG("%s: %s: info: channel map: obuf[%d] <- out_port[%d]\n", dsp_globals.prog_name, e->name, out_ch, out_port);
					for (ssize_t i = 0; i < len; ++i)
						obuf[(f + i) * e->ostream.channels + out_ch] = (sample_t) state->out[out_port][i];
					++out_port;
				}
				else {
					while (in_ch < e->istream.channels && GET_BIT(e->channel_selector, in_ch)) ++in_ch;
					if (in_ch < e->istream.channels) goto copy_ibuf;
				}
			}
			else {
				copy_ibuf:
				CM_DEBUG("%s: %s: info: channel map: obuf[%d] <- ibuf[%d]\n", dsp_globals.prog_name, e->name, out_ch, in_ch);
				for (ssize_t i = 0; i < len; ++i)
					obuf[(f + i) * e->ostream.channels + out_ch] = ibuf[(f + i) * e->istream.channels + in_ch];
			}
		}
		CM_DEBUG("%s: %s: info: end channel map\n", dsp_globals.prog_name, e->name);
		f += len;
		#if DEBUG_CHANNEL_MAPPING
			state->cm_printed = 1;
		#endif
	}
	return obuf;
}

void ladspa_host_effect_destroy(struct effect *e)
{
	struct ladspa_host_state *state = (struct ladspa_host_state *) e->data;
	if (state->handles != NULL) {
		for (int i = 0; i < state->n_handles; ++i) {
			if (state->handles[i] != NULL) {
				if (state->desc->deactivate != NULL) state->desc->deactivate(state->handles[i]);
				state->desc->cleanup(state->handles[i]);
			}
		}
	}
	free(state->handles);
	if (state->in != NULL) for (int i = 0; i < state->n_in; ++i) free(state->in[i]);
	free(state->in);
	if (state->out != NULL) for (int i = 0; i < state->n_out; ++i) free(state->out[i]);
	free(state->out);
	free(state->control);
	if (state->lt_handle != NULL) lt_dlclose(state->lt_handle);
	free(state);
	lt_dlexit();
	free(e->channel_selector);
}

struct effect * ladspa_host_effect_init(const struct effect_info *ei, const struct stream_info *istream, const char *channel_selector, const char *dir, int argc, const char *const *argv)
{
	const char *search_path;
	char *path, *endptr;
	LADSPA_Descriptor_Function descriptor_fn;
	const LADSPA_Descriptor *desc;
	struct effect *e;
	struct ladspa_host_state *state;
	int in_control_port_count = 0, out_control_port_count = 0;

	if (argc < 3) {
		LOG_FMT(LL_ERROR, "%s: usage %s", argv[0], ei->usage);
		return NULL;
	}

	if ((search_path = getenv("LADSPA_PATH")) == NULL) search_path = "/usr/local/lib/ladspa:/usr/lib/ladspa";
	if (lt_dlinit() || lt_dlsetsearchpath(search_path)) {
		LOG_FMT(LL_ERROR, "%s: error: failed to initialize libltdl: %s", argv[0], lt_dlerror());
		return NULL;
	}

	state = calloc(1, sizeof(struct ladspa_host_state));
	e = calloc(1, sizeof(struct effect));
	e->data = state;

	/* Build paths and open the plugin module */
	if (argv[1][0] == '.' && argv[1][1] == '/') path = construct_full_path(dir, argv[1]);
	else path = strdup(argv[1]);
	if ((state->lt_handle = lt_dlopenext(path)) == NULL) {
		LOG_FMT(LL_ERROR, "%s: error: failed to open LADSPA plugin: %s: %s", argv[0], path, lt_dlerror());
		goto fail;
	}

	/* Get address of ladspa_descriptor() */
	if ((descriptor_fn = lt_dlsym(state->lt_handle, "ladspa_descriptor")) == NULL) {
		LOG_FMT(LL_ERROR, "%s: %s: error: could not find ladspa_descriptor()", argv[0], path);
		goto fail;
	}

	/* Find correct descriptor by its label */
	for (unsigned long plugin_idx = 0; (desc = descriptor_fn(plugin_idx)) != NULL; ++plugin_idx) {
		if (strcmp(desc->Label, argv[2]) == 0) {
			state->desc = desc;
			break;
		}
	}
	if (state->desc == NULL) {
		LOG_FMT(LL_ERROR, "%s: %s: error: could not find plugin: %s", argv[0], path, argv[2]);
		goto fail;
	}
	desc = state->desc;

	/* Count each type of port; error if port types are invalid */
	for (unsigned long i = 0; i < desc->PortCount; ++i) {
		LADSPA_PortDescriptor pd = desc->PortDescriptors[i];
		if (LADSPA_IS_PORT_INPUT(pd) && LADSPA_IS_PORT_OUTPUT(pd)) {
			LOG_FMT(LL_ERROR, "%s: %s: %s: error: port '%s' (%lu) is both an input and an output", argv[0], path, argv[2], desc->PortNames[i], i);
			goto fail;
		}
		if (LADSPA_IS_PORT_AUDIO(pd) && LADSPA_IS_PORT_CONTROL(pd)) {
			LOG_FMT(LL_ERROR, "%s: %s: %s: error: port '%s' (%lu) is both audio and control", argv[0], path, argv[2], desc->PortNames[i], i);
			goto fail;
		}
		if (LADSPA_IS_PORT_INPUT(pd) && LADSPA_IS_PORT_AUDIO(pd)) ++state->n_in;
		else if (LADSPA_IS_PORT_INPUT(pd) && LADSPA_IS_PORT_CONTROL(pd)) ++in_control_port_count;
		else if (LADSPA_IS_PORT_OUTPUT(pd) && LADSPA_IS_PORT_AUDIO(pd)) ++state->n_out;
		else if (LADSPA_IS_PORT_OUTPUT(pd) && LADSPA_IS_PORT_CONTROL(pd)) ++out_control_port_count;
	}

	if (state->n_out < 1) {
		LOG_FMT(LL_ERROR, "%s: %s: %s: error: plugin has no audio outputs", argv[0], path, argv[2]);
		goto fail;
	}
	const int selected_channel_count = num_bits_set(channel_selector, istream->channels);
	if (state->n_in > 1) {
		if (state->n_in != selected_channel_count) {
			LOG_FMT(LL_ERROR, "%s: %s: %s: error: expected %d input channels, got %d", argv[0], path, argv[2], state->n_in, selected_channel_count);
			goto fail;
		}
		state->n_handles = 1;
	}
	else {
		state->n_handles = selected_channel_count;
		state->n_in *= state->n_handles;
		state->n_out *= state->n_handles;
	}

	state->handles = calloc(state->n_handles, sizeof(LADSPA_Handle));
	state->buf_size = DEFAULT_BLOCK_FRAMES;
	if (state->n_in > 0) {
		state->in = calloc(state->n_in, sizeof(LADSPA_Data *));
		for (int i = 0; i < state->n_in; ++i) state->in[i] = calloc(state->buf_size, sizeof(LADSPA_Data));
	}
	state->out = calloc(state->n_out, sizeof(LADSPA_Data *));
	for (int i = 0; i < state->n_out; ++i) state->out[i] = calloc(state->buf_size, sizeof(LADSPA_Data));
	if (in_control_port_count + out_control_port_count > 0)
		state->control = calloc(in_control_port_count + out_control_port_count, sizeof(LADSPA_Data));
	const int total_output_channels = istream->channels + state->n_out - ((state->n_in == 0) ? state->n_handles : state->n_in);

	/* Set input control port values */
	if (argc > 3 + in_control_port_count) {
		LOG_FMT(LL_ERROR, "%s: %s: %s: error: plugin expects %d controls, got %d", argv[0], path, argv[2], in_control_port_count, argc - 3);
		goto fail;
	}
	if (in_control_port_count > 0) {
		int cport = 0, k = 3;
		for (unsigned long i = 0; i < desc->PortCount; ++i) {
			LADSPA_PortDescriptor pd = desc->PortDescriptors[i];
			const LADSPA_PortRangeHint *pr = &desc->PortRangeHints[i];
			if (LADSPA_IS_PORT_CONTROL(pd)) {
				if (LADSPA_IS_PORT_INPUT(pd)) {
					LADSPA_Data lower = pr->LowerBound, upper = pr->UpperBound;
					if (LADSPA_IS_HINT_SAMPLE_RATE(pr->HintDescriptor)) {
						lower *= istream->fs;
						upper *= istream->fs;
					}
					if (k < argc && strcmp(argv[k], "-") != 0) {
						state->control[cport] = strtof(argv[k], &endptr);
						CHECK_ENDPTR(argv[k], endptr, desc->PortNames[i], goto fail);
					}
					else if (LADSPA_IS_HINT_HAS_DEFAULT(pr->HintDescriptor)) {
						/* set default value */
						if (LADSPA_IS_HINT_DEFAULT_MINIMUM(pr->HintDescriptor))
							state->control[cport] = lower;
						else if (LADSPA_IS_HINT_DEFAULT_LOW(pr->HintDescriptor))
							state->control[cport] = (LADSPA_IS_HINT_LOGARITHMIC(pr->HintDescriptor)) ? exp(log(lower) * 0.75 + log(upper) * 0.25) : (lower * 0.75 + upper * 0.25);
						else if (LADSPA_IS_HINT_DEFAULT_MIDDLE(pr->HintDescriptor))
							state->control[cport] = (LADSPA_IS_HINT_LOGARITHMIC(pr->HintDescriptor)) ? exp(log(lower) * 0.5 + log(upper) * 0.5) : (lower * 0.5 + upper * 0.5);
						else if (LADSPA_IS_HINT_DEFAULT_HIGH(pr->HintDescriptor))
							state->control[cport] = (LADSPA_IS_HINT_LOGARITHMIC(pr->HintDescriptor)) ? exp(log(lower) * 0.25 + log(upper) * 0.75) : (lower * 0.25 + upper * 0.75);
						else if (LADSPA_IS_HINT_DEFAULT_MAXIMUM(pr->HintDescriptor))
							state->control[cport] = upper;
						else if (LADSPA_IS_HINT_DEFAULT_0(pr->HintDescriptor))
							state->control[cport] = 0.0;
						else if (LADSPA_IS_HINT_DEFAULT_1(pr->HintDescriptor))
							state->control[cport] = 1.0;
						else if (LADSPA_IS_HINT_DEFAULT_100(pr->HintDescriptor))
							state->control[cport] = 100.0;
						else if (LADSPA_IS_HINT_DEFAULT_440(pr->HintDescriptor))
							state->control[cport] = 440.0;
					}
					else {
						LOG_FMT(LL_ERROR, "%s: %s: %s: error: control \"%s\" has no default value and is not set", argv[0], path, argv[2], desc->PortNames[i]);
						goto fail;
					}
					if (LADSPA_IS_HINT_INTEGER(pr->HintDescriptor))
						state->control[cport] = round(state->control[cport]);
					if (LADSPA_IS_HINT_BOUNDED_BELOW(pr->HintDescriptor))
						CHECK_RANGE(state->control[cport] >= lower, desc->PortNames[i], goto fail);
					if (LADSPA_IS_HINT_BOUNDED_ABOVE(pr->HintDescriptor))
						CHECK_RANGE(state->control[cport] <= upper, desc->PortNames[i], goto fail);
					++k;
				}
				++cport;
			}
		}
	}

	/* Instantiate plugins, connect ports, and activate plugins (if required) */
	for (int i = 0, iport = 0, oport = 0, cport = 0; i < state->n_handles; ++i) {
		if ((state->handles[i] = desc->instantiate(desc, istream->fs)) == NULL) {
			LOG_FMT(LL_ERROR, "%s: %s: %s: error: instantiate() failed", argv[0], path, argv[2]);
			goto fail;
		}
		for (unsigned long k = 0; k < desc->PortCount; ++k) {
			LADSPA_PortDescriptor pd = desc->PortDescriptors[k];
			if (LADSPA_IS_PORT_INPUT(pd) && LADSPA_IS_PORT_AUDIO(pd))
				desc->connect_port(state->handles[i], k, state->in[iport++]);
			else if (LADSPA_IS_PORT_OUTPUT(pd) && LADSPA_IS_PORT_AUDIO(pd))
				desc->connect_port(state->handles[i], k, state->out[oport++]);
			else if (LADSPA_IS_PORT_CONTROL(pd))
				desc->connect_port(state->handles[i], k, &state->control[cport++]);
		}
		if (desc->activate != NULL) desc->activate(state->handles[i]);
	}

	/* Print input control port names and values */
	if (in_control_port_count > 0 && LOGLEVEL(LL_VERBOSE)) {
		int cport = 0;
		fprintf(stderr, "%s: %s: %s: %s: info: controls:", dsp_globals.prog_name, argv[0], path, argv[2]);
		for (unsigned long i = 0; i < desc->PortCount; ++i) {
			LADSPA_PortDescriptor pd = desc->PortDescriptors[i];
			if (LADSPA_IS_PORT_CONTROL(pd)) {
				if (LADSPA_IS_PORT_INPUT(pd))
					fprintf(stderr, " \"%s\"=%g", desc->PortNames[i], state->control[cport]);
				++cport;
			}
		}
		fputc('\n', stderr);
	}

	e->name = ei->name;
	e->istream.fs = e->ostream.fs = istream->fs;
	e->istream.channels = istream->channels;
	e->ostream.channels = total_output_channels;
	e->channel_selector = NEW_SELECTOR(istream->channels);
	COPY_SELECTOR(e->channel_selector, channel_selector, istream->channels);
	e->run = ladspa_host_effect_run;
	e->destroy = ladspa_host_effect_destroy;

	free(path);
	return e;

	fail:
	free(path);
	ladspa_host_effect_destroy(e);
	free(e);
	return NULL;
}
