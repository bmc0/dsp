#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <locale.h>
#include <ladspa.h>

#include "dsp.h"
#include "effect.h"
#include "util.h"

#define DEFAULT_CONFIG_DIR     "/ladspa_dsp"
#define DEFAULT_XDG_CONFIG_DIR "/.config"
#define GLOBAL_CONFIG_DIR      "/etc"DEFAULT_CONFIG_DIR

struct ladspa_dsp {
	sample_t *buf1, *buf2;
	size_t frames;
	int input_channels, output_channels;
	struct effects_chain chain;
	LADSPA_Data **ports;
};

struct ladspa_dsp_config {
	int input_channels, output_channels, chain_argc;
	char *name, *dir_path, *lc_n, **chain_argv;
};

struct dsp_globals dsp_globals = {
	0,                      /* clip_count */
	0,                      /* peak */
	LL_NORMAL,              /* loglevel */
	DEFAULT_BUF_FRAMES,     /* buf_frames */
	DEFAULT_MAX_BUF_RATIO,  /* max_buf_ratio */
	"ladspa_dsp",           /* prog_name */
};

static int n_configs = 0;
static struct ladspa_dsp_config *configs = NULL;
static LADSPA_Descriptor *descriptors = NULL;

static void init_config(struct ladspa_dsp_config *config, const char *file_name, const char *dir_path)
{
	memset(config, 0, sizeof(struct ladspa_dsp_config));
	config->input_channels = 1;
	config->output_channels = 1;
	if (strcmp(file_name, "config") != 0)
		config->name = strdup(&file_name[7]);
	config->dir_path = strdup(dir_path);
}

static int read_config(struct ladspa_dsp_config *config, const char *path)
{
	int i, k;
	char *c, *key, *value, *next, *endptr;

	c = get_file_contents(path);
	if (!c) return 2;

	key = c;
	for (i = 1; *key != '\0'; ++i) {
		while ((*key == ' ' || *key == '\t') && *key != '\n' && *key != '\0')
			++key;
		next = isolate(key, '\n');
		if (*key != '\0' && *key != '#') {
			value = isolate(key, '=');
			if (strcmp(key, "input_channels") == 0) {
				config->input_channels = strtol(value, &endptr, 10);
				if (check_endptr(path, value, endptr, "input_channels")) goto parse_fail;
				if (config->input_channels <= 0) {
					LOG_S(LL_ERROR, "error: input_channels must be > 0");
					goto parse_fail;
				}
			}
			else if (strcmp(key, "output_channels") == 0) {
				config->output_channels = strtol(value, &endptr, 10);
				if (check_endptr(path, value, endptr, "output_channels")) goto parse_fail;
				if (config->output_channels <= 0) {
					LOG_S(LL_ERROR, "error: output_channels must be > 0");
					goto parse_fail;
				}
			}
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
			else {
				LOG_FMT(LL_ERROR, "error: line %d: invalid option: %s", i, key);
				goto parse_fail;
			}
		}
		key = next;
	}
	free(c);
	return 0;

	parse_fail:
	free(c);
	return 1;
}

static void load_configs_in_path(char *path)
{
	int i, err;
	DIR *d = NULL;
	struct dirent *d_ent;
	char *next, *c_path;

	while (path && *path != '\0') {
		next = isolate(path, ':');
		d = opendir(path);
		if (d) {
			LOG_FMT(LL_VERBOSE, "info: opened config dir: %s", path);
			while((d_ent = readdir(d))) {
				if (strcmp(d_ent->d_name, "config") == 0
						|| (strncmp(d_ent->d_name, "config_", 7) == 0 && strlen(d_ent->d_name) > 7)) {
					i = strlen(path) + strlen(d_ent->d_name) + 2;
					c_path = calloc(i, sizeof(char));
					snprintf(c_path, i, "%s/%s", path, d_ent->d_name);
					configs = realloc(configs, (n_configs + 1) * sizeof(struct ladspa_dsp_config));
					init_config(&configs[n_configs], d_ent->d_name, path);
					if ((err = read_config(&configs[n_configs], c_path))) {
						if (err == 2) LOG_FMT(LL_ERROR, "warning: failed to read config file: %s: %s", c_path, strerror(errno));
						else LOG_FMT(LL_ERROR, "warning: failed to parse config file: %s", c_path);
					}
					else {
						LOG_FMT(LL_VERBOSE, "info: read config file: %s", c_path);
						++n_configs;
					}
					free(c_path);
				}
			}
			closedir(d);
		}
		else {
			LOG_FMT(LL_VERBOSE, "info: failed to open config dir: %s: %s", path, strerror(errno));
		}
		path = next;
	}
}

static void load_configs(void)
{
	int i;
	char *env, *path = NULL;

	if ((env = getenv("LADSPA_DSP_CONFIG_PATH"))) {
		path = strdup(env);
	}
	else {
		if ((env = getenv("XDG_CONFIG_HOME"))) {
			i = strlen(env) + strlen(DEFAULT_CONFIG_DIR) + 1 + strlen(GLOBAL_CONFIG_DIR) + 1;
			path = calloc(i, sizeof(char));
			snprintf(path, i, "%s%s:%s", env, DEFAULT_CONFIG_DIR, GLOBAL_CONFIG_DIR);
		}
		else if ((env = getenv("HOME"))) {
			i = strlen(env) + strlen(DEFAULT_XDG_CONFIG_DIR) + strlen(DEFAULT_CONFIG_DIR) + 1 + strlen(GLOBAL_CONFIG_DIR) + 1;
			path = calloc(i, sizeof(char));
			snprintf(path, i, "%s%s%s:%s", env, DEFAULT_XDG_CONFIG_DIR, DEFAULT_CONFIG_DIR, GLOBAL_CONFIG_DIR);
		}
	}
	LOG_FMT(LL_VERBOSE, "info: config path: %s", path);
	load_configs_in_path(path);
	free(path);
}

static LADSPA_Handle instantiate_dsp(const LADSPA_Descriptor *desc, unsigned long fs)
{
	int r;
	locale_t old_locale = 0, new_locale = 0;
	struct stream_info stream;
	struct ladspa_dsp_config *config = (struct ladspa_dsp_config *) desc->ImplementationData;
	struct ladspa_dsp *d = calloc(1, sizeof(struct ladspa_dsp));

	LOG_FMT(LL_VERBOSE, "info: using label: %s", desc->Label);
	d->input_channels = config->input_channels;
	d->output_channels = config->output_channels;
	d->ports = calloc(d->input_channels + d->output_channels, sizeof(LADSPA_Data *));
	stream.fs = fs;
	stream.channels = d->input_channels;
	LOG_S(LL_VERBOSE, "info: begin effects chain");
	if (config->lc_n != NULL) {
		LOG_FMT(LL_VERBOSE, "info: setting LC_NUMERIC to \"%s\"", config->lc_n);
		new_locale = duplocale(uselocale((locale_t) 0));
		if (new_locale == (locale_t) 0) {
			LOG_S(LL_ERROR, "error: duplocale() failed");
			goto fail;
		}
		new_locale = newlocale(LC_NUMERIC_MASK, config->lc_n, new_locale);
		if (new_locale == (locale_t) 0) {
			LOG_S(LL_ERROR, "error: newlocale() failed");
			goto fail;
		}
		old_locale = uselocale(new_locale);
	}
	r = build_effects_chain(config->chain_argc, config->chain_argv, &d->chain, &stream, NULL, config->dir_path);
	if (old_locale != (locale_t) 0) {
		LOG_S(LL_VERBOSE, "info: resetting locale");
		uselocale(old_locale);
	}
	if (new_locale != (locale_t) 0) freelocale(new_locale);
	if (r) goto fail;
	LOG_S(LL_VERBOSE, "info: end effects chain");
	if (stream.channels != d->output_channels) {
		LOG_S(LL_ERROR, "error: output channels mismatch");
		goto fail;
	}
	if (stream.fs != fs) {
		LOG_S(LL_ERROR, "error: sample rate mismatch");
		goto fail;
	}
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
	ssize_t w = s, buf_len;
	struct ladspa_dsp *d = (struct ladspa_dsp *) inst;

	if (s == 0) return;
	if (s > d->frames) {
		d->frames = s;
		buf_len = get_effects_chain_buffer_len(&d->chain, s, d->input_channels);
		d->buf1 = realloc(d->buf1, buf_len * sizeof(sample_t));
		d->buf2 = realloc(d->buf2, buf_len * sizeof(sample_t));
		LOG_FMT(LL_VERBOSE, "info: frames=%zd", d->frames);
	}

	for (i = j = 0; i < s; i++)
		for (k = 0; k < d->input_channels; ++k)
			d->buf1[j++] = (sample_t) d->ports[k][i];

	obuf = run_effects_chain(d->chain.head, &w, d->buf1, d->buf2);

	for (i = j = 0; i < s; i++)
		for (k = d->input_channels; k < d->input_channels + d->output_channels; ++k)
			d->ports[k][i] = (LADSPA_Data) obuf[j++];
}

static void cleanup_dsp(LADSPA_Handle inst)
{
	struct ladspa_dsp *d = (struct ladspa_dsp *) inst;
	LOG_S(LL_VERBOSE, "info: cleaning up...");
	free(d->buf1);
	free(d->buf2);
	destroy_effects_chain(&d->chain);
	free(d->ports);
	free(d);
}

void __attribute__((constructor)) ladspa_dsp_so_init()
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
			LOG_FMT(LL_ERROR, "warning: unrecognized loglevel: %s", env);
	}

	load_configs();
	if (n_configs > 0)
		descriptors = calloc(n_configs, sizeof(LADSPA_Descriptor));
	else {
		LOG_S(LL_ERROR, "error: no config files found");
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

void __attribute__((destructor)) ladspa_dsp_so_fini()
{
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
		free(configs[k].dir_path);
		free(configs[k].name);
	}
	free(descriptors);
	free(configs);
}

const LADSPA_Descriptor * ladspa_descriptor(unsigned long i)
{
	if (i < n_configs)
		return &descriptors[i];
	else
		return NULL;
}
