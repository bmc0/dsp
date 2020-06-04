#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <ladspa.h>
#include <ltdl.h>
#include "ladspa_host.h"
#include "util.h"

#define DEBUG_CHANNEL_MAPPING 0
#if DEBUG_CHANNEL_MAPPING
	#define CM_DEBUG(...) fprintf(stderr, __VA_ARGS__)
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
};

/* For plugins with multiple input ports */
sample_t * ladspa_host_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	ssize_t f = 0, len;
	struct ladspa_host_state *state = (struct ladspa_host_state *) e->data;

	while (f < *frames) {
		len = (*frames - f > state->buf_size) ? state->buf_size : *frames - f;
		CM_DEBUG("\n-- start --\n");
		for (int c = 0, iport = 0; c < e->istream.channels; ++c) {
			if (GET_BIT(e->channel_selector, c)) {
				CM_DEBUG("ibuf(c%d) -> in[%d]\n", c, iport);
				for (ssize_t i = 0; i < len; ++i)
					state->in[iport][i] = (LADSPA_Data) ibuf[(f + i) * e->istream.channels + c];
				++iport;
			}
		}
		state->desc->run(state->handles[0], (unsigned long) len);
		for (int in_c = 0, out_c = 0, oport = 0; in_c < e->istream.channels && out_c < e->ostream.channels; ++in_c) {
			if (GET_BIT(e->channel_selector, in_c)) {
				if (oport < state->n_out) {
					if (oport < state->n_in) {
						CM_DEBUG("out[%d] -> obuf(c%d)\n", oport, out_c);
						for (ssize_t i = 0; i < len; ++i)
							obuf[(f + i) * e->ostream.channels + out_c] = (sample_t) state->out[oport][i];
						++oport;
						++out_c;
					}
					if (oport == state->n_in) {
						for (; oport < state->n_out; ++oport, ++out_c) {
							CM_DEBUG("out[%d] -> obuf(c%d)\n", oport, out_c);
							for (ssize_t i = 0; i < len; ++i)
								obuf[(f + i) * e->ostream.channels + out_c] = (sample_t) state->out[oport][i];
						}
					}
				}
			}
			else {
				CM_DEBUG("ibuf(c%d) -> obuf(c%d)\n", in_c, out_c);
				for (ssize_t i = 0; i < len; ++i)
					obuf[(f + i) * e->ostream.channels + out_c] = ibuf[(f + i) * e->istream.channels + in_c];
				++out_c;
			}
		}
		CM_DEBUG("-- end --\n");
		f += len;
	}
	return obuf;
}

/* For plugins with a single input port */
sample_t * ladspa_host_effect_run_cloned(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	ssize_t f = 0, len;
	struct ladspa_host_state *state = (struct ladspa_host_state *) e->data;

	while (f < *frames) {
		len = (*frames - f > state->buf_size) ? state->buf_size : *frames - f;
		CM_DEBUG("\n-- start --\n");
		for (int in_c = 0, out_c = 0, handle = 0; in_c < e->istream.channels; ++in_c) {
			if (GET_BIT(e->channel_selector, in_c)) {
				if (state->n_in > 0) {
					CM_DEBUG("ibuf(c%d) -> in[%d]\n", in_c, 0);
					for (ssize_t i = 0; i < len; ++i)
						state->in[0][i] = (LADSPA_Data) ibuf[(f + i) * e->istream.channels + in_c];
				}
				state->desc->run(state->handles[handle++], (unsigned long) len);
				for (int oport = 0; oport < state->n_out; ++oport, ++out_c) {
					CM_DEBUG("out[%d] -> obuf(c%d)\n", oport, out_c);
					for (ssize_t i = 0; i < len; ++i)
						obuf[(f + i) * e->ostream.channels + out_c] = (sample_t) state->out[oport][i];
				}
			}
			else {
				CM_DEBUG("ibuf(c%d) -> obuf(c%d)\n", in_c, out_c);
				for (ssize_t i = 0; i < len; ++i)
					obuf[(f + i) * e->ostream.channels + out_c] = ibuf[(f + i) * e->istream.channels + in_c];
				++out_c;
			}
		}
		CM_DEBUG("-- end --\n");
		f += len;
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

struct effect * ladspa_host_effect_init(struct effect_info *ei, struct stream_info *istream, char *channel_selector, const char *dir, int argc, char **argv)
{
	const char *search_path;
	char *path, *endptr;
	LADSPA_Descriptor_Function descriptor_fn;
	const LADSPA_Descriptor *desc;
	struct effect *e;
	struct ladspa_host_state *state;
	int in_control_port_count = 0, out_control_port_count = 0;
	int selected_channel_count = 0, total_output_channels;

	if (argc < 3) {
		LOG_FMT(LL_ERROR, "%s: usage %s", argv[0], ei->usage);
		return NULL;
	}

	if ((search_path = getenv("LADSPA_PATH")) == NULL) search_path = "/usr/local/lib/ladspa:/usr/lib/ladspa";
	if (lt_dlinit() || lt_dlsetsearchpath(search_path)) {
		LOG_FMT(LL_ERROR, "%s: error: failed to initialize libltdl: %s", argv[0], lt_dlerror());
		return NULL;
	}

	for (int i = 0; i < istream->channels; ++i) if (GET_BIT(channel_selector, i)) ++selected_channel_count;

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

	state->n_handles = 1;
	if (state->n_in <= 1)
		state->n_handles = selected_channel_count;
	else if (state->n_in != selected_channel_count) {
		LOG_FMT(LL_ERROR, "%s: %s: %s: error: expected %d input channels, got %d", argv[0], path, argv[2], state->n_in, selected_channel_count);
		goto fail;
	}
	if (state->n_out < 1) {
		LOG_FMT(LL_ERROR, "%s: %s: %s: error: plugin has no audio outputs", argv[0], path, argv[2]);
		goto fail;
	}

	state->handles = calloc(state->n_handles, sizeof(LADSPA_Handle));
	state->buf_size = dsp_globals.buf_frames;
	if (state->n_in > 0) {
		state->in = calloc(state->n_in, sizeof(LADSPA_Data *));
		for (int i = 0; i < state->n_in; ++i) state->in[i] = calloc(state->buf_size, sizeof(LADSPA_Data));
	}
	state->out = calloc(state->n_out, sizeof(LADSPA_Data *));
	for (int i = 0; i < state->n_out; ++i) state->out[i] = calloc(state->buf_size, sizeof(LADSPA_Data));
	if (in_control_port_count + out_control_port_count > 0)
		state->control = calloc(in_control_port_count + out_control_port_count, sizeof(LADSPA_Data));
	total_output_channels = istream->channels + (state->n_out - ((state->n_in < 1) ? 1 : state->n_in)) * state->n_handles;

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
	for (int i = 0; i < state->n_handles; ++i) {
		if ((state->handles[i] = desc->instantiate(desc, istream->fs)) == NULL) {
			LOG_FMT(LL_ERROR, "%s: %s: %s: error: instantiate() failed", argv[0], path, argv[2]);
			goto fail;
		}
		int iport = 0, oport = 0, cport = 0;
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
		LOG_FMT(LL_VERBOSE, "%s: %s: %s: info: controls:", argv[0], path, argv[2]);
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
	e->run = (state->n_in <= 1) ? ladspa_host_effect_run_cloned : ladspa_host_effect_run;
	e->destroy = ladspa_host_effect_destroy;

	free(path);
	return e;

	fail:
	free(path);
	ladspa_host_effect_destroy(e);
	free(e);
	return NULL;
}
