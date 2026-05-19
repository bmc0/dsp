/*
 * This file is part of dsp.
 *
 * Copyright (c) 2014-2026 Michael Barbour <barbour.michael.0@gmail.com>
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

#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <locale.h>
#include <ladspa.h>

#include "dsp.h"
#include "effect.h"
#include "effects_chain.h"
#include "util.h"

#define DEFAULT_CONFIG_DIR     "/ladspa_dsp"
#define DEFAULT_XDG_CONFIG_DIR "/.config"
#define GLOBAL_CONFIG_DIR      "/etc"DEFAULT_CONFIG_DIR
#define DEFAULT_LOGLEVEL       LL_OPEN_ERROR

struct ladspa_dsp {
	sample_t *buf1, *buf2;
	size_t frames;
	int input_channels, output_channels;
	struct effects_chain chain;
	LADSPA_Data **ports;
};

struct ladspa_dsp_config {
	int input_channels, output_channels;
	char *name, *dir_path, *lc_n, *chain_str;
};

static const char default_name[] = "ladspa_dsp";
struct dsp_globals dsp_globals = {
	DEFAULT_LOGLEVEL,       /* loglevel */
	default_name,           /* prog_name */
};

static int n_configs = 0, is_init = 0;
static struct ladspa_dsp_config *configs = NULL;
static LADSPA_Descriptor *descriptors = NULL;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

void dsp_log_acquire(void)
{
	pthread_mutex_lock(&log_lock);
}

void dsp_log_release(void)
{
	pthread_mutex_unlock(&log_lock);
}

static void destroy_config(struct ladspa_dsp_config *config)
{
	free(config->name);
	free(config->dir_path);
	free(config->lc_n);
	free(config->chain_str);
}

static int init_config(struct ladspa_dsp_config *config, const char *file_name, const char *dir_path)
{
	memset(config, 0, sizeof(struct ladspa_dsp_config));
	config->input_channels = 1;
	config->output_channels = 1;
	if (strcmp(file_name, "config") != 0) {
		config->name = strdup(&file_name[7]);
		if (!config->name) goto fail;
	}
	config->dir_path = strdup(dir_path);
	config->lc_n = strdup("C");
	if (!config->dir_path || !config->lc_n) goto fail;
	return 0;

	fail:
	destroy_config(config);
	dsp_perror(DSP_ENOMEM, __func__, NULL);
	return 1;
}

static int read_config(struct ladspa_dsp_config *config, const char *path)
{
	char *c = get_file_contents(path);
	if (!c) return 2;

	char *key = c, *endptr;
	for (int i = 1; *key != '\0'; ++i) {
		while ((*key == ' ' || *key == '\t') && *key != '\n' && *key != '\0')
			++key;
		char *next = isolate(key, '\n');
		if (*key != '\0' && *key != '#') {
			if (strcmp(key, "[effects_chain]") == 0) {
				config->chain_str = next;
				break;
			}
			char *value = isolate(key, '=');
			if (strcmp(key, "input_channels") == 0) {
				config->input_channels = strtol(value, &endptr, 10);
				if (check_endptr(path, value, endptr, "input_channels")) goto fail_parse;
				if (config->input_channels <= 0) {
					LOG_FMT(LL_ERROR, "%s: error: input_channels must be > 0", path);
					goto fail_parse;
				}
			}
			else if (strcmp(key, "output_channels") == 0) {
				config->output_channels = strtol(value, &endptr, 10);
				if (check_endptr(path, value, endptr, "output_channels")) goto fail_parse;
				if (config->output_channels <= 0) {
					LOG_FMT(LL_ERROR, "%s: error: output_channels must be > 0", path);
					goto fail_parse;
				}
			}
			else if (strcmp(key, "LC_NUMERIC") == 0) {
				free(config->lc_n);
				if (strcmp(value, "none") == 0) config->lc_n = NULL;
				else {
					config->lc_n = strdup(value);
					if (!config->lc_n) goto fail_alloc;
				}
			}
			else if (strcmp(key, "effects_chain") == 0) {
				config->chain_str = value;
			}
			else {
				LOG_FMT(LL_ERROR, "%s: line %d: error: invalid option: %s", path, i, key);
				goto fail_parse;
			}
		}
		key = next;
	}
	if (!config->chain_str) LOG_FMT(LL_ERROR, "%s: warning: no effects chain specified", path);
	else {
		config->chain_str = strdup(config->chain_str);
		if (!config->chain_str) goto fail_alloc;
	}
	free(c);
	return 0;

	fail_parse:
	free(c);
	return 1;

	fail_alloc:
	free(c);
	dsp_perror(DSP_ENOMEM, __func__, NULL);
	return 3;
}

static int load_configs_in_path(char *path)
{
	while (path && *path != '\0') {
		char *next = isolate(path, ':');
		DIR *d = opendir(path);
		if (d) {
			LOG_FMT(LL_VERBOSE, "info: opened config dir: %s", path);
			struct dirent *d_ent;
			while((d_ent = readdir(d))) {
				if (strcmp(d_ent->d_name, "config") == 0
						|| (strncmp(d_ent->d_name, "config_", 7) == 0 && strlen(d_ent->d_name) > 7)) {
					struct ladspa_dsp_config *configs_tmp = realloc(configs, (n_configs + 1) * sizeof(struct ladspa_dsp_config));
					if (check_alloc(__func__, configs_tmp)) {
						fail:
						closedir(d);
						return 1;
					}
					configs = configs_tmp;
					int i = strlen(path) + strlen(d_ent->d_name) + 2, err;
					char *c_path = calloc(i, sizeof(char));
					if (check_alloc(__func__, c_path)) goto fail;
					snprintf(c_path, i, "%s/%s", path, d_ent->d_name);
					if (init_config(&configs[n_configs], d_ent->d_name, path)) {
						free(c_path);
						goto fail;
					}
					if ((err = read_config(&configs[n_configs], c_path))) {
						destroy_config(&configs[n_configs]);
						if (err == 2) LOG_FMT(LL_ERROR, "warning: failed to read config file: %s: %s", c_path, strerror(errno));
						else if (err == 1) LOG_FMT(LL_ERROR, "warning: failed to parse config file: %s", c_path);
						else {
							free(c_path);
							goto fail;
						}
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
		else LOG_FMT(LL_VERBOSE, "info: failed to open config dir: %s: %s", path, strerror(errno));
		path = next;
	}
	return 0;
}

static int load_configs(void)
{
	char *env, *path = NULL;

	if ((env = getenv("LADSPA_DSP_CONFIG_PATH"))) {
		path = strdup(env);
	}
	else {
		if ((env = getenv("XDG_CONFIG_HOME"))) {
			int i = strlen(env) + strlen(DEFAULT_CONFIG_DIR) + 1 + strlen(GLOBAL_CONFIG_DIR) + 1;
			path = calloc(i, sizeof(char));
			if (path) snprintf(path, i, "%s%s:%s", env, DEFAULT_CONFIG_DIR, GLOBAL_CONFIG_DIR);
		}
		else if ((env = getenv("HOME"))) {
			int i = strlen(env) + strlen(DEFAULT_XDG_CONFIG_DIR) + strlen(DEFAULT_CONFIG_DIR) + 1 + strlen(GLOBAL_CONFIG_DIR) + 1;
			path = calloc(i, sizeof(char));
			if (path) snprintf(path, i, "%s%s%s:%s", env, DEFAULT_XDG_CONFIG_DIR, DEFAULT_CONFIG_DIR, GLOBAL_CONFIG_DIR);
		}
		else {
			path = strdup(GLOBAL_CONFIG_DIR);
		}
	}
	if (check_alloc(__func__, path)) return 1;
	LOG_FMT(LL_VERBOSE, "info: config path: %s", path);
	int ret = load_configs_in_path(path);
	free(path);
	return ret;
}

static LADSPA_Handle instantiate_dsp(const LADSPA_Descriptor *desc, unsigned long fs)
{
	locale_t old_locale = 0, new_locale = 0;
	struct stream_info stream;
	struct ladspa_dsp_config *config = (struct ladspa_dsp_config *) desc->ImplementationData;
	struct ladspa_dsp *d = calloc(1, sizeof(struct ladspa_dsp));
	if (check_alloc(__func__, d)) return NULL;

	LOG_FMT(LL_VERBOSE, "info: using label: %s", desc->Label);
	d->input_channels = config->input_channels;
	d->output_channels = config->output_channels;
	d->chain = (struct effects_chain) EFFECTS_CHAIN_INITIALIZER;
	d->ports = calloc(d->input_channels + d->output_channels, sizeof(LADSPA_Data *));
	if (check_alloc(__func__, d->ports)) goto fail;
	stream.fs = fs;
	stream.channels = d->input_channels;
	LOG_S(LL_VERBOSE, "info: begin effects chain");
	if (config->lc_n != NULL) {
		LOG_FMT(LL_VERBOSE, "info: setting LC_NUMERIC to \"%s\"", config->lc_n);
		new_locale = duplocale(uselocale((locale_t) 0));
		if (new_locale == (locale_t) 0) {
			LOG_FMT(LL_ERROR, "error: duplocale() failed: %s", strerror(errno));
			goto fail;
		}
		new_locale = newlocale(LC_NUMERIC_MASK, config->lc_n, new_locale);
		if (new_locale == (locale_t) 0) {
			LOG_FMT(LL_ERROR, "error: newlocale() failed: %s", strerror(errno));
			goto fail;
		}
		old_locale = uselocale(new_locale);
	}
	int r = 0;
	if (config->chain_str)
		r = build_effects_chain_from_string(config->chain_str, config->name, &d->chain, &stream, NULL, config->dir_path);
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
	effects_chain_set_dither_params(&d->chain, 0, 0);  /* disable auto dither */
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
	struct ladspa_dsp *d = (struct ladspa_dsp *) inst;
	ssize_t w = s;

	if (s == 0) return;
	if (s > d->frames) {
		LOG_FMT(LL_VERBOSE, "info: frames=%zd", s);
		ssize_t buf_len = get_effects_chain_buffer_len(&d->chain, s, d->input_channels);
		sample_t *buf1_tmp = realloc(d->buf1, buf_len * sizeof(sample_t));
		sample_t *buf2_tmp = realloc(d->buf2, buf_len * sizeof(sample_t));
		if (!buf1_tmp || !buf2_tmp) {
			free((buf1_tmp) ? buf1_tmp : d->buf1);
			free((buf2_tmp) ? buf2_tmp : d->buf2);
			d->buf1 = d->buf2 = NULL;
			dsp_perror(DSP_ENOMEM, __func__, NULL);
		}
		else {
			d->buf1 = buf1_tmp;
			d->buf2 = buf2_tmp;
		}
		d->frames = s;
	}
	if (!d->buf1 || !d->buf2) {
		/* failed to allocate buffer(s); output silence */
		for (unsigned long k = d->input_channels; k < d->input_channels + d->output_channels; ++k)
			memset(d->ports[k], 0, s * sizeof(LADSPA_Data));
		return;
	}

	for (unsigned long i = 0, j = 0; i < s; ++i)
		for (unsigned long k = 0; k < d->input_channels; ++k)
			d->buf1[j++] = (sample_t) d->ports[k][i];

	sample_t *obuf = run_effects_chain(&d->chain, &w, d->buf1, d->buf2);

	for (unsigned long i = 0, j = 0; i < s; ++i)
		for (unsigned long k = d->input_channels; k < d->input_channels + d->output_channels; ++k)
			d->ports[k][i] = (LADSPA_Data) obuf[j++];
}

static void run_null(LADSPA_Handle inst, unsigned long s)
{
	struct ladspa_dsp *d = (struct ladspa_dsp *) inst;
	if (s > 0) memset(d->ports[1], 0, s * sizeof(LADSPA_Data));
}

static void cleanup_dsp(LADSPA_Handle inst)
{
	struct ladspa_dsp *d = (struct ladspa_dsp *) inst;
	LOG_S(LL_VERBOSE, "info: cleaning up...");
	destroy_effects_chain(&d->chain);
	#ifdef HAVE_FFTW3
		dsp_fftw_save_wisdom();
	#endif
	free(d->buf1);
	free(d->buf2);
	free(d->ports);
	free(d);
}

static char * make_port_name(const char *prefix, int idx)
{
	char buf[32] = {0};
	snprintf(buf, LENGTH(buf), "%s%d", prefix, idx);
	return strdup(buf);
}

void __attribute__((constructor)) ladspa_dsp_so_init()
{
	int is_fallback = 0;
	char *env = getenv("LADSPA_DSP_LOGLEVEL");
	if (env != NULL) {
		if (env[0] == '\0')
			dsp_globals.loglevel = DEFAULT_LOGLEVEL;
		else if (strcmp(env, "VERBOSE") == 0)
			dsp_globals.loglevel = LL_VERBOSE;
		else if (strcmp(env, "NORMAL") == 0)
			dsp_globals.loglevel = LL_NORMAL;
		else if (strcmp(env, "SILENT") == 0)
			dsp_globals.loglevel = LL_SILENT;
		else
			LOG_FMT(LL_ERROR, "warning: unrecognized loglevel: %s", env);
	}

	if (load_configs()) return;
	if (n_configs < 1) {
		LOG_S(LL_ERROR, "warning: no config files found; providing fallback 'null' plugin");
		free(configs);
		configs = calloc(1, sizeof(struct ladspa_dsp_config));
		if (check_alloc(__func__, configs)) return;
		if (init_config(&configs[0], "config_null", GLOBAL_CONFIG_DIR)) return;
		n_configs = is_fallback = 1;
	}
	descriptors = calloc(n_configs, sizeof(LADSPA_Descriptor));
	if (check_alloc(__func__, descriptors)) return;
	for (int k = 0; k < n_configs; ++k) {
		descriptors[k].UniqueID = 2378 + k;
		if (configs[k].name) {
			int i = LENGTH(default_name) + strlen(configs[k].name) + 1;
			char *tmp = calloc(i, sizeof(char));
			if (check_alloc(__func__, tmp)) return;
			snprintf(tmp, i, "%s:%s", default_name, configs[k].name);
			descriptors[k].Label = tmp;
		}
		else descriptors[k].Label = default_name;
		descriptors[k].Properties = 0;
		descriptors[k].Name = descriptors[k].Label;
		descriptors[k].Maker = "Michael Barbour";
		descriptors[k].Copyright = "ISC";
		descriptors[k].PortCount = configs[k].input_channels + configs[k].output_channels;
		LADSPA_PortDescriptor *pd = calloc(descriptors[k].PortCount, sizeof(LADSPA_PortDescriptor));
		if (check_alloc(__func__, pd)) return;
		descriptors[k].PortDescriptors = pd;
		for (int i = 0; i < configs[k].input_channels; ++i)
			pd[i] = LADSPA_PORT_INPUT | LADSPA_PORT_AUDIO;
		for (int i = configs[k].input_channels; i < configs[k].input_channels + configs[k].output_channels; ++i)
			pd[i] = LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO;
		char **pn = calloc(descriptors[k].PortCount, sizeof(char *));
		if (check_alloc(__func__, pn)) return;
		descriptors[k].PortNames = (const char **) pn;
		for (int i = 0; i < configs[k].input_channels + configs[k].output_channels; ++i) {
			if (i < configs[k].input_channels) pn[i] = make_port_name("Input", i);
			else pn[i] = make_port_name("Output", i - configs[k].input_channels);
			if (check_alloc(__func__, pn[i])) return;
		}
		LADSPA_PortRangeHint *ph = calloc(descriptors[k].PortCount, sizeof(LADSPA_PortRangeHint));
		if (check_alloc(__func__, ph)) return;
		descriptors[k].PortRangeHints = ph;
		for (int i = 0; i < descriptors[k].PortCount; ++i)
			ph[i].HintDescriptor = 0;
		descriptors[k].instantiate = instantiate_dsp;
		descriptors[k].connect_port = connect_port_to_dsp;
		descriptors[k].run = (is_fallback) ? run_null : run_dsp;
		descriptors[k].run_adding = NULL;
		descriptors[k].set_run_adding_gain = NULL;
		descriptors[k].deactivate = NULL;
		descriptors[k].cleanup = cleanup_dsp;
		descriptors[k].ImplementationData = &configs[k];
	}
	is_init = 1;
}

void __attribute__((destructor)) ladspa_dsp_so_fini()
{
	if (descriptors) {
		for (int k = 0; k < n_configs; ++k) {
			if (descriptors[k].Label != default_name)
				free((char *) descriptors[k].Label); /* note: dsp_descriptor->Name is the same data */
			free((LADSPA_PortDescriptor *) descriptors[k].PortDescriptors);
			if (descriptors[k].PortNames) {
				for (int i = 0; i < configs[k].input_channels + configs[k].output_channels; ++i)
					free((char *) descriptors[k].PortNames[i]);
				free((char **) descriptors[k].PortNames);
			}
			free((LADSPA_PortRangeHint *) descriptors[k].PortRangeHints);
		}
		free(descriptors);
	}
	if (configs) {
		for (int k = 0; k < n_configs; ++k)
			destroy_config(&configs[k]);
		free(configs);
	}
}

const LADSPA_Descriptor * ladspa_descriptor(unsigned long i)
{
	if (is_init && i < n_configs) return &descriptors[i];
	else return NULL;
}
