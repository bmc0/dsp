#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <ladspa.h>

#include "dsp.h"
#include "effect.h"
#include "util.h"

#define DEFAULT_CONFIG_LOCATION "/ladspa_dsp/config"
#define DEFAULT_XDG_CONFIG_DIR "/.config"

#define MAX(a, b) ((a > b) ? a : b)

struct ladspa_dsp {
	sample_t *buf1, *buf2;
	size_t buf_len;
	struct effects_chain chain;
	double max_ratio;
	LADSPA_Data **ports;
};

struct dsp_globals dsp_globals = {
	0,                      /* clip_count */
	0,                      /* peak */
	LL_NORMAL,              /* loglevel */
	DEFAULT_BUF_FRAMES,     /* buf_frames */
	DEFAULT_MAX_BUF_RATIO,  /* max_buf_ratio */
};

static int input_channels = 1;
static int output_channels = 1;
static int chain_argc = 0;
static char **chain_argv = NULL;

LADSPA_Handle instantiate_dsp(const LADSPA_Descriptor *Descriptor, unsigned long fs)
{
	struct stream_info stream;
	struct ladspa_dsp *d = calloc(1, sizeof(struct ladspa_dsp));

	d->ports = calloc(input_channels + output_channels, sizeof(LADSPA_Data *));
	stream.fs = fs;
	stream.channels = input_channels;
	LOG(LL_VERBOSE, "ladspa_dsp: info: begin effects chain\n");
	if (build_effects_chain(chain_argc, chain_argv, &d->chain, &stream, NULL, NULL))
		goto fail;
	LOG(LL_VERBOSE, "ladspa_dsp: info: end effects chain\n");
	if (stream.channels != output_channels) {
		LOG(LL_ERROR, "ladspa_dsp: error: output channels mismatch\n");
		goto fail;
	}
	if (stream.fs != fs) {
		LOG(LL_ERROR, "ladspa_dsp: error: sample rate mismatch\n");
		goto fail;
	}
	d->max_ratio = get_effects_chain_max_ratio(&d->chain);
	return d;

	fail:
	destroy_effects_chain(&d->chain);
	free(d->ports);
	free(d);
	return NULL;
}

void connect_port_to_dsp(LADSPA_Handle inst, unsigned long port, LADSPA_Data *data)
{
	struct ladspa_dsp *d = (struct ladspa_dsp *) inst;
	if (port < input_channels + output_channels)
		d->ports[port] = data;
}

void run_dsp(LADSPA_Handle inst, unsigned long s)
{
	unsigned long i, j, k;
	sample_t *obuf;
	ssize_t w = s;
	struct ladspa_dsp *d = (struct ladspa_dsp *) inst;

	i = s * MAX(input_channels, output_channels);
	if (i > d->buf_len) {
		d->buf_len = i;
		d->buf1 = realloc(d->buf1, d->buf_len * d->max_ratio * sizeof(sample_t));
		d->buf2 = realloc(d->buf2, d->buf_len * d->max_ratio * sizeof(sample_t));
		LOG(LL_VERBOSE, "ladspa_dsp: info: buf_len=%zd frames=%ld\n", d->buf_len, s);
	}

	for (i = j = 0; i < s; i++)
		for (k = 0; k < input_channels; ++k)
			d->buf1[j++] = (sample_t) d->ports[k][i];
	
	obuf = run_effects_chain(&d->chain, &w, d->buf1, d->buf2);

	for (i = j = 0; i < s; i++)
		for (k = input_channels; k < input_channels + output_channels; ++k)
			d->ports[k][i] = (LADSPA_Data) obuf[j++];
}

void cleanup_dsp(LADSPA_Handle inst)
{
	struct ladspa_dsp *d = (struct ladspa_dsp *) inst;
	free(d->buf1);
	free(d->buf2);
	destroy_effects_chain(&d->chain);
	free(d->ports);
	free(d);
}

LADSPA_Descriptor *dsp_descriptor = NULL;
 
static char * isolate(char *s, char c)
{
	while (*s && *s != c) ++s;
	*s = '\0';
	return s + 1;
}

void _init()
{
	LADSPA_PortDescriptor *pd;
	char **pn;
	LADSPA_PortRangeHint *ph;
	int i, k, fd;
	char *env, *config_file, *contents = NULL, *key, *value, *next;
	off_t file_size;

	env = getenv("LADSPA_DSP_LOGLEVEL");
	if (env != NULL) {
		if (strcmp(env, "VERBOSE") == 0)
			dsp_globals.loglevel = LL_VERBOSE;
		else if (strcmp(env, "NORMAL") == 0)
			dsp_globals.loglevel = LL_NORMAL;
		else if (strcmp(env, "SILENT") == 0)
			dsp_globals.loglevel = LL_SILENT;
		else
			LOG(LL_ERROR, "ladspa_dsp: warning: unrecognized loglevel: %s\n", env);
	}

	env = getenv("LADSPA_DSP_CONFIG");
	if (env == NULL) {
		if ((env = getenv("XDG_CONFIG_HOME"))) {
			i = strlen(env) + strlen(DEFAULT_CONFIG_LOCATION) + 1;
			config_file = calloc(i, sizeof(char));
			snprintf(config_file, i, "%s%s", env, DEFAULT_CONFIG_LOCATION);
		}
		else {
			env = getenv("HOME");
			i = strlen(env) + strlen(DEFAULT_XDG_CONFIG_DIR) + strlen(DEFAULT_CONFIG_LOCATION) + 1;
			config_file = calloc(i, sizeof(char));
			snprintf(config_file, i, "%s%s%s", env, DEFAULT_XDG_CONFIG_DIR, DEFAULT_CONFIG_LOCATION);
		}
	}
	else
		config_file = strdup(env);
	LOG(LL_VERBOSE, "ladspa_dsp: info: config_file=%s\n", config_file);
	if ((fd = open(config_file, O_RDONLY)) == -1)
		LOG(LL_ERROR, "ladspa_dsp: warning: failed to open config file: %s\n", config_file);
	else {
		if ((file_size = lseek(fd, 0, SEEK_END)) == -1) {
			LOG(LL_ERROR, "ladspa_dsp: error: failed to determine file size: %s: %s\n", config_file, strerror(errno));
			goto fail;
		}
		lseek(fd, 0, SEEK_SET);
		contents = malloc(file_size + 1);
		if (read(fd, contents, file_size) != file_size) {
			LOG(LL_ERROR, "ladspa_dsp: error: short read: %s\n", config_file);
			goto fail;
		}
		contents[file_size] = '\0';
		key = contents;
		for (i = 1; key[0] != '\0'; ++i) {
			next = isolate(key, '\n');
			if (key[0] == '\n' || key[0] == '#');
			else {
				value = isolate(key, '=');
				if (strcmp(key, "input_channels") == 0)       input_channels = atoi(value);
				else if (strcmp(key, "output_channels") == 0) output_channels = atoi(value);
				else if (strcmp(key, "effects_chain") == 0) {
					for (k = 0; k < chain_argc; ++k)
						free(chain_argv[k]);
					free(chain_argv);
					gen_argv_from_string(value, &chain_argc, &chain_argv);
				}
				else LOG(LL_ERROR, "ladspa_dsp: warning: line %d: invalid option: %s\n", i, key);
			}
			key = next;
		}
		fail:
		free(contents);
		close(fd);
	}
	free(config_file);

	dsp_descriptor = calloc(1, sizeof(LADSPA_Descriptor));
	if (dsp_descriptor != NULL) {
		dsp_descriptor->UniqueID = 2378;
		dsp_descriptor->Label = strdup("ladspa_dsp");
		dsp_descriptor->Properties = 0;
		dsp_descriptor->Name = strdup("ladspa_dsp");
		dsp_descriptor->Maker = strdup("Michael Barbour");
		dsp_descriptor->Copyright = strdup("ISC");
		dsp_descriptor->PortCount = input_channels + output_channels;
		pd = calloc(input_channels + output_channels, sizeof(LADSPA_PortDescriptor));
		dsp_descriptor->PortDescriptors = pd;
		for (i = 0; i < input_channels; ++i)
			pd[i] = LADSPA_PORT_INPUT | LADSPA_PORT_AUDIO;
		for (i = input_channels; i < input_channels + output_channels; ++i)
			pd[i] = LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO;
		pn = calloc(input_channels + output_channels, sizeof(char *));
		dsp_descriptor->PortNames = (const char **) pn;
		for (i = 0; i < input_channels; ++i)
			pn[i] = strdup("Input");
		for (i = input_channels; i < input_channels + output_channels; ++i)
			pn[i] = strdup("Output");
		ph = calloc(input_channels + output_channels, sizeof(LADSPA_PortRangeHint));
		dsp_descriptor->PortRangeHints = ph;
		for (i = 0; i < input_channels + output_channels; ++i)
			ph[i].HintDescriptor = 0;
		dsp_descriptor->instantiate = instantiate_dsp;
		dsp_descriptor->connect_port = connect_port_to_dsp;
		dsp_descriptor->run = run_dsp;
		dsp_descriptor->run_adding = NULL;
		dsp_descriptor->set_run_adding_gain = NULL;
		dsp_descriptor->deactivate = NULL;
		dsp_descriptor->cleanup = cleanup_dsp;
	}
}

void _fini() {
	int i;
	free((char *) dsp_descriptor->Label);
	free((char *) dsp_descriptor->Name);
	free((char *) dsp_descriptor->Maker);
	free((char *) dsp_descriptor->Copyright);
	free((LADSPA_PortDescriptor *) dsp_descriptor->PortDescriptors);
	for (i = 0; i < input_channels + output_channels; ++i)
		free((char *) dsp_descriptor->PortNames[i]);
	free((char **) dsp_descriptor->PortNames);
	free((LADSPA_PortRangeHint *) dsp_descriptor->PortRangeHints);
	free(dsp_descriptor);
	for (i = 0; i < chain_argc; ++i)
		free(chain_argv[i]);
	free(chain_argv);
}

const LADSPA_Descriptor *ladspa_descriptor(unsigned long i)
{
	switch(i) {
		case 0:
			return dsp_descriptor;
		default:
			return NULL;
	}
}
