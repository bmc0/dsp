#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <math.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <locale.h>
#include <ladspa.h>

#include "dsp.h"
#include "effect.h"
#include "util.h"

#define DEFAULT_CONFIG_DIR "/ladspa_dsp"
#define DEFAULT_XDG_CONFIG_DIR "/.config"
#define GLOBAL_CONFIG_DIR "/etc"DEFAULT_CONFIG_DIR

struct ladspa_dsp {
	sample_t *buf1, *buf2;
	size_t buf_frames;
	int input_channels, output_channels;
	struct effects_chain chain;
	double max_ratio;
	LADSPA_Data **ports;
};

struct ladspa_dsp_config {
	int input_channels, output_channels, chain_argc;
	char *name, *lc_n, **chain_argv;
};

struct dsp_globals dsp_globals = {
	0,                      /* clip_count */
	0,                      /* peak */
	LL_NORMAL,              /* loglevel */
	DEFAULT_BUF_FRAMES,     /* buf_frames */
	DEFAULT_MAX_BUF_RATIO,  /* max_buf_ratio */
};

static char *config_dir = NULL;
static int n_configs = 0;
static struct ladspa_dsp_config *configs = NULL;
static LADSPA_Descriptor *descriptors = NULL;

static char * isolate(char *s, char c)
{
	while (*s && *s != c) ++s;
	if (*s != '\0') *s++ = '\0';
	return s;
}

static DIR * try_dir(const char *path)
{
	DIR *d;
	if (!(d = opendir(path)))
		LOG(LL_VERBOSE, "ladspa_dsp: info: failed to open config dir: %s: %s\n", path, strerror(errno));
	return d;
}

static int read_config(const char *path, const char *name, struct ladspa_dsp_config *config)
{
	int i, k;
	char *c, *key, *value, *next;

	c = get_file_contents(path);
	if (!c)
		return 1;
		
	key = c;
	for (i = 1; *key != '\0'; ++i) {
		while ((*key == ' ' || *key == '\t') && *key != '\n' && *key != '\0')
			++key;
		next = isolate(key, '\n');
		if (*key != '\0' && *key != '#') {
			value = isolate(key, '=');
			if (strcmp(key, "input_channels") == 0)
				config->input_channels = atoi(value);
			else if (strcmp(key, "output_channels") == 0)
				config->output_channels = atoi(value);
			else if (strcmp(key, "LC_NUMERIC") == 0) {
				free(config->lc_n);
				config->lc_n = strdup(value);
			}
			else if (strcmp(key, "effects_chain") == 0) {
				for (k = 0; k < config->chain_argc; ++k)
					free(config->chain_argv[k]);
				free(config->chain_argv);
				gen_argv_from_string(value, &config->chain_argc, &config->chain_argv);
			}
			else
				LOG(LL_ERROR, "ladspa_dsp: warning: line %d: invalid option: %s\n", i, key);
		}
		key = next;
	}
	free(c);
	free(config->name);
	config->name = (name) ? strdup(name) : NULL;
	return 0;
}

static void load_configs(void)
{
	int i;
	DIR *d = NULL;
	char *c_path, *env;
	struct dirent *d_ent;

	/* Use environment variable, if present */
	if ((env = getenv("LADSPA_DSP_DIR"))) {
		config_dir = strdup(env);
		d = try_dir(config_dir);
	}
	else {
		/* Build local config dir path */
		if ((env = getenv("XDG_CONFIG_HOME"))) {
			i = strlen(env) + strlen(DEFAULT_CONFIG_DIR) + 1;
			config_dir = calloc(i, sizeof(char));
			snprintf(config_dir, i, "%s%s", env, DEFAULT_CONFIG_DIR);
		}
		else if ((env = getenv("HOME"))) {
			i = strlen(env) + strlen(DEFAULT_XDG_CONFIG_DIR) + strlen(DEFAULT_CONFIG_DIR) + 1;
			config_dir = calloc(i, sizeof(char));
			snprintf(config_dir, i, "%s%s%s", env, DEFAULT_XDG_CONFIG_DIR, DEFAULT_CONFIG_DIR);
		}

		if (config_dir)
			d = try_dir(config_dir);
		if (!d) {
			free(config_dir);
			config_dir = strdup(GLOBAL_CONFIG_DIR);
			d = try_dir(config_dir);
		}
	}

	if (d) {
		LOG(LL_VERBOSE, "ladspa_dsp: info: selected config dir: %s\n", config_dir);
		while((d_ent = readdir(d))) {
			if (strcmp(d_ent->d_name, "config") == 0
					|| (strncmp(d_ent->d_name, "config_", 7) == 0 && strlen(d_ent->d_name) > 7)) {
				i = strlen(config_dir) + strlen(d_ent->d_name) + 2;
				c_path = calloc(i, sizeof(char));
				snprintf(c_path, i, "%s/%s", config_dir, d_ent->d_name);
				configs = realloc(configs, (n_configs + 1) * sizeof(struct ladspa_dsp_config));
				memset(&configs[n_configs], 0, sizeof(struct ladspa_dsp_config));
				configs[n_configs].input_channels = configs[n_configs].output_channels = 1;
				if (read_config(c_path, (strcmp(d_ent->d_name, "config") == 0) ? NULL : &d_ent->d_name[7], &configs[n_configs]))
					LOG(LL_ERROR, "ladspa_dsp: warning: failed to read config file: %s: %s\n", c_path, strerror(errno));
				else {
					LOG(LL_VERBOSE, "ladspa_dsp: info: read config file: %s\n", c_path);
					++n_configs;
				}
				free(c_path);
			}
		}
		closedir(d);
	}
}

static LADSPA_Handle instantiate_dsp(const LADSPA_Descriptor *desc, unsigned long fs)
{
	char *lc_n_old = NULL;
	struct stream_info stream;
	struct ladspa_dsp_config *config = (struct ladspa_dsp_config *) desc->ImplementationData;
	struct ladspa_dsp *d = calloc(1, sizeof(struct ladspa_dsp));

	LOG(LL_VERBOSE, "ladspa_dsp: info: using label: %s\n", desc->Label);
	d->input_channels = config->input_channels;
	d->output_channels = config->output_channels;
	d->ports = calloc(d->input_channels + d->output_channels, sizeof(LADSPA_Data *));
	stream.fs = fs;
	stream.channels = d->input_channels;
	LOG(LL_VERBOSE, "ladspa_dsp: info: begin effects chain\n");
	lc_n_old = strdup(setlocale(LC_NUMERIC, NULL));
	setlocale(LC_NUMERIC, config->lc_n);
	if (build_effects_chain(config->chain_argc, config->chain_argv, &d->chain, &stream, NULL, config_dir)) {
		setlocale(LC_NUMERIC, lc_n_old);
		free(lc_n_old);
		goto fail;
	}
	setlocale(LC_NUMERIC, lc_n_old);
	free(lc_n_old);
	LOG(LL_VERBOSE, "ladspa_dsp: info: end effects chain\n");
	if (stream.channels != d->output_channels) {
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

static void connect_port_to_dsp(LADSPA_Handle inst, unsigned long port, LADSPA_Data *data)
{
	struct ladspa_dsp *d = (struct ladspa_dsp *) inst;
	if (port < d->input_channels + d->output_channels)
		d->ports[port] = data;
}

static void run_dsp(LADSPA_Handle inst, unsigned long s)
{
	unsigned long i, j, k;
	sample_t *obuf;
	ssize_t w = s;
	struct ladspa_dsp *d = (struct ladspa_dsp *) inst;

	if (s > d->buf_frames) {
		d->buf_frames = s;
		d->buf1 = realloc(d->buf1, (size_t) ceil(s * d->input_channels * d->max_ratio) * sizeof(sample_t));
		d->buf2 = realloc(d->buf2, (size_t) ceil(s * d->input_channels * d->max_ratio) * sizeof(sample_t));
		LOG(LL_VERBOSE, "ladspa_dsp: info: buf_frames=%zd\n", d->buf_frames);
	}

	for (i = j = 0; i < s; i++)
		for (k = 0; k < d->input_channels; ++k)
			d->buf1[j++] = (sample_t) d->ports[k][i];

	obuf = run_effects_chain(&d->chain, &w, d->buf1, d->buf2);

	for (i = j = 0; i < s; i++)
		for (k = d->input_channels; k < d->input_channels + d->output_channels; ++k)
			d->ports[k][i] = (LADSPA_Data) obuf[j++];
}

static void cleanup_dsp(LADSPA_Handle inst)
{
	struct ladspa_dsp *d = (struct ladspa_dsp *) inst;
	LOG(LL_VERBOSE, "ladspa_dsp: info: cleaning up...\n");
	free(d->buf1);
	free(d->buf2);
	destroy_effects_chain(&d->chain);
	free(d->ports);
	free(d);
}

void __attribute__((constructor)) ladspa_dsp_init()
{
	int i, k;
	char **pn, *env, *tmp;
	LADSPA_PortDescriptor *pd;
	LADSPA_PortRangeHint *ph;

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

	load_configs();
	if (n_configs > 0)
		descriptors = calloc(n_configs, sizeof(LADSPA_Descriptor));
	else {
		LOG(LL_ERROR, "ladspa_dsp: error: no config files found\n");
		return;
	}
	for (k = 0; k < n_configs; ++k) {
		descriptors[k].UniqueID = 2378 + k;
		if (configs[k].name) {
			i = strlen("ladspa_dsp") + strlen(configs[k].name) + 2;
			tmp = calloc(i, sizeof(char));
			snprintf(tmp, i, "%s:%s", "ladspa_dsp", configs[k].name);
			descriptors[k].Label = tmp;
		}
		else
			descriptors[k].Label = strdup("ladspa_dsp");
		descriptors[k].Properties = 0;
		descriptors[k].Name = descriptors[k].Label;
		descriptors[k].Maker = strdup("Michael Barbour");
		descriptors[k].Copyright = strdup("ISC");
		descriptors[k].PortCount = configs[k].input_channels + configs[k].output_channels;
		pd = calloc(configs[k].input_channels + configs[k].output_channels, sizeof(LADSPA_PortDescriptor));
		descriptors[k].PortDescriptors = pd;
		for (i = 0; i < configs[k].input_channels; ++i)
			pd[i] = LADSPA_PORT_INPUT | LADSPA_PORT_AUDIO;
		for (i = configs[k].input_channels; i < configs[k].input_channels + configs[k].output_channels; ++i)
			pd[i] = LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO;
		pn = calloc(configs[k].input_channels + configs[k].output_channels, sizeof(char *));
		descriptors[k].PortNames = (const char **) pn;
		for (i = 0; i < configs[k].input_channels; ++i)
			pn[i] = strdup("Input");
		for (i = configs[k].input_channels; i < configs[k].input_channels + configs[k].output_channels; ++i)
			pn[i] = strdup("Output");
		ph = calloc(configs[k].input_channels + configs[k].output_channels, sizeof(LADSPA_PortRangeHint));
		descriptors[k].PortRangeHints = ph;
		for (i = 0; i < configs[k].input_channels + configs[k].output_channels; ++i)
			ph[i].HintDescriptor = 0;
		descriptors[k].instantiate = instantiate_dsp;
		descriptors[k].connect_port = connect_port_to_dsp;
		descriptors[k].run = run_dsp;
		descriptors[k].run_adding = NULL;
		descriptors[k].set_run_adding_gain = NULL;
		descriptors[k].deactivate = NULL;
		descriptors[k].cleanup = cleanup_dsp;
		descriptors[k].ImplementationData = &configs[k];
	}
}

void __attribute__((destructor)) ladspa_dsp_fini() {
	int i, k;
	for (k = 0; k < n_configs; ++k) {
		free((char *) descriptors[k].Label); /* note: dsp_descriptor->Name is the same data */
		free((char *) descriptors[k].Maker);
		free((char *) descriptors[k].Copyright);
		free((LADSPA_PortDescriptor *) descriptors[k].PortDescriptors);
		for (i = 0; i < configs[k].input_channels + configs[k].output_channels; ++i)
			free((char *) descriptors[k].PortNames[i]);
		free((char **) descriptors[k].PortNames);
		free((LADSPA_PortRangeHint *) descriptors[k].PortRangeHints);
		for (i = 0; i < configs[k].chain_argc; ++i)
			free(configs[k].chain_argv[i]);
		free(configs[k].chain_argv);
		free(configs[k].lc_n);
		free(configs[k].name);
	}
	free(descriptors);
	free(configs);
	free(config_dir);
}

const LADSPA_Descriptor *ladspa_descriptor(unsigned long i)
{
	if (i < n_configs)
		return &descriptors[i];
	else
		return NULL;
}
