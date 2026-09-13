/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2009 - 2026, Lefteris Zafiris
 *
 * Lefteris Zafiris <zaf@fastmail.com>
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the COPYING file
 * at the top of the source tree.
 */

/*! \file
 *
 * \brief Say text to the user, using eSpeak-ng TTS engine.
 *
 * \author\verbatim Lefteris Zafiris <zaf@fastmail.com> \endverbatim
 *
 * \extref eSpeak-ng text to speech Synthesis System - https://github.com/espeak-ng/espeak-ng
 *
 * \ingroup applications
 */

/*** MODULEINFO
	<defaultenabled>no</defaultenabled>
 ***/

# define AST_MODULE_SELF_SYM __internal_app_espeak_self

#include "asterisk.h"

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <espeak-ng/speak_lib.h>
#include <espeak-ng/espeak_ng.h>
#include <samplerate.h>
#include "asterisk/app.h"
#include "asterisk/channel.h"
#include "asterisk/module.h"
#include "asterisk/config.h"
#include "asterisk/utils.h"
#include "asterisk/lock.h"

#define AST_MODULE "eSpeak"
#define ESPEAK_CONFIG "espeak.conf"
#define MAXLEN 4096
#define MAXTEXT 32768
#define DEF_MAXTEXT 4096
#define DEF_RATE 8000
#define DEF_SPEED 150
#define DEF_VOLUME 100
#define DEF_WORDGAP 1
#define DEF_PITCH 50
#define DEF_VOICE "en-us"
#define DEF_DIR "/var/lib/asterisk/espeakcache"
#define ESPK_BUFFER 4096

/*** DOCUMENTATION
	<application name="eSpeak" language="en_US">
		<synopsis>
			Say text to the user, using eSpeak-ng speech synthesizer.
		</synopsis>
		<syntax>
			<parameter name="text" required="true" />
			<parameter name="intkeys" />
			<parameter name="language" />
		</syntax>
		<description>
			<para>eSpeak(text[,intkeys,language]):  This will invoke the eSpeak-ng TTS engine,
			send a text string, get back the resulting waveform and play it to the user,
			allowing any given interrupt keys to immediately terminate and return.</para>
		</description>
	</application>
 ***/

static const char *app = "eSpeak";

static char cachedir[MAXLEN];
static char def_voice[64];
static int usecache;
static int target_sample_rate;
static int speed;
static int volume;
static int wordgap;
static int pitch;
static int maxtext;

/* Protects the config values above. */
AST_RWLOCK_DEFINE_STATIC(cfg_lock);

/* Serializes all espeak engine access: the library keeps global state
 * and is not thread safe. */
AST_MUTEX_DEFINE_STATIC(espk_lock);

static int parse_int(const char *val, int def, int min, int max, const char *name)
{
	char *end;
	long n;

	errno = 0;
	n = strtol(val, &end, 10);
	if (errno || end == val || *end || n < min || n > max) {
		ast_log(LOG_WARNING,
				"eSpeak: Invalid value '%s' for %s (valid: %d-%d), using default %d\n",
				val, name, min, max, def);
		return def;
	}
	return (int) n;
}

static int read_config(const char *espeak_conf)
{
	const char *temp;
	struct ast_config *cfg;
	struct ast_flags config_flags = { 0 };

	cfg = ast_config_load(espeak_conf, config_flags);
	if (!cfg || cfg == CONFIG_STATUS_FILEINVALID) {
		ast_log(LOG_WARNING,
				"eSpeak: Unable to read config file %s. Using default settings\n", espeak_conf);
		cfg = NULL;
	}

	ast_rwlock_wrlock(&cfg_lock);
	/* Setting default config values */
	ast_copy_string(cachedir, DEF_DIR, sizeof(cachedir));
	ast_copy_string(def_voice, DEF_VOICE, sizeof(def_voice));
	usecache = 0;
	target_sample_rate = DEF_RATE;
	speed = DEF_SPEED;
	volume = DEF_VOLUME;
	wordgap = DEF_WORDGAP;
	pitch = DEF_PITCH;
	maxtext = DEF_MAXTEXT;

	if (cfg) {
		if ((temp = ast_variable_retrieve(cfg, "general", "usecache")))
			usecache = ast_true(temp);
		temp = ast_variable_retrieve(cfg, "general", "cachedir");
		if (!ast_strlen_zero(temp))
			ast_copy_string(cachedir, temp, sizeof(cachedir));
		if ((temp = ast_variable_retrieve(cfg, "general", "samplerate")))
			target_sample_rate = parse_int(temp, DEF_RATE, 8000, 16000, "samplerate");
		if ((temp = ast_variable_retrieve(cfg, "general", "maxtext")))
			maxtext = parse_int(temp, DEF_MAXTEXT, 1, MAXTEXT, "maxtext");
		if ((temp = ast_variable_retrieve(cfg, "voice", "speed")))
			speed = parse_int(temp, DEF_SPEED, 80, 450, "speed");
		if ((temp = ast_variable_retrieve(cfg, "voice", "wordgap")))
			wordgap = parse_int(temp, DEF_WORDGAP, 0, 1000, "wordgap");
		if ((temp = ast_variable_retrieve(cfg, "voice", "volume")))
			volume = parse_int(temp, DEF_VOLUME, 0, 200, "volume");
		if ((temp = ast_variable_retrieve(cfg, "voice", "pitch")))
			pitch = parse_int(temp, DEF_PITCH, 0, 99, "pitch");
		temp = ast_variable_retrieve(cfg, "voice", "voice");
		if (!ast_strlen_zero(temp))
			ast_copy_string(def_voice, temp, sizeof(def_voice));
	}

	if (target_sample_rate != 8000 && target_sample_rate != 16000) {
		ast_log(LOG_WARNING,
				"eSpeak: Unsupported sample rate: %d. Falling back to %d\n",
				target_sample_rate, DEF_RATE);
		target_sample_rate = DEF_RATE;
	}
	if (usecache) {
		struct stat st;
		if (stat(cachedir, &st) && (ast_mkdir(cachedir, 0700) || stat(cachedir, &st))) {
			ast_log(LOG_ERROR,
					"eSpeak: Failed to create cache directory %s, caching disabled\n", cachedir);
			usecache = 0;
		} else if (!S_ISDIR(st.st_mode) || st.st_uid != geteuid()
				|| (st.st_mode & (S_IWGRP | S_IWOTH))) {
			ast_log(LOG_ERROR,
					"eSpeak: Cache directory %s must be owned by the Asterisk user "
					"and not group/world writable, caching disabled\n", cachedir);
			usecache = 0;
		}
	}
	ast_rwlock_unlock(&cfg_lock);
	if (cfg)
		ast_config_destroy(cfg);
	return 0;
}

struct synth_data {
	FILE *fl;
	int err;
};

static int synth_callback(short *wav, int numsamples, espeak_EVENT *events)
{
	struct synth_data *sd = events[0].user_data;

	if (wav && numsamples > 0) {
		if (fwrite(wav, sizeof(short), (size_t) numsamples, sd->fl)
				!= (size_t) numsamples) {
			sd->err = 1;
			return 1; /* Write error, stop synthesis */
		}
	}
	return 0; /* Continue synthesis */
}

/* Sound data resampling */
static int raw_resample(char *fname, double ratio)
{
	int res = 0;
	FILE *fl;
	struct stat st;
	size_t in_size;
	short *in_buff, *out_buff;
	long in_frames, out_frames;
	float *inp, *outp;
	SRC_DATA rate_change;

	if ((fl = fopen(fname, "r")) == NULL) {
		ast_log(LOG_ERROR, "eSpeak: Failed to open file for resampling.\n");
		return -1;
	}
	if (fstat(fileno(fl), &st) == -1 || st.st_size == 0) {
		ast_log(LOG_ERROR, "eSpeak: Failed to stat file for resampling.\n");
		fclose(fl);
		return -1;
	}
	in_size = (size_t) st.st_size;
	in_frames = (long) (in_size / sizeof(short));
	if ((in_buff = ast_malloc(in_size)) == NULL) {
		fclose(fl);
		return -1;
	}
	if (fread(in_buff, 1, in_size, fl) != in_size) {
		ast_log(LOG_ERROR, "eSpeak: Failed to read file for resampling.\n");
		fclose(fl);
		res = -1;
		goto CLEAN1;
	}
	fclose(fl);

	if ((inp = ast_malloc((size_t) in_frames * sizeof(float))) == NULL) {
		res = -1;
		goto CLEAN1;
	}
	src_short_to_float_array(in_buff, inp, (int) in_frames);
	/* +1 headroom is required and sufficient for src_simple: measured
	 * minimum slack is exactly one frame. Revisit if the converter changes. */
	out_frames = (long)((double) in_frames * ratio) + 1;
	if ((outp = ast_malloc((size_t) out_frames * sizeof(float))) == NULL) {
		res = -1;
		goto CLEAN2;
	}
	rate_change.data_in = inp;
	rate_change.data_out = outp;
	rate_change.input_frames = in_frames;
	rate_change.output_frames = out_frames;
	rate_change.src_ratio = ratio;

	if ((res = src_simple(&rate_change, SRC_SINC_FASTEST, 1)) != 0) {
		ast_log(LOG_ERROR, "eSpeak: Failed to resample sound file '%s': '%s'\n",
				fname, src_strerror(res));
		res = -1;
		goto CLEAN3;
	}

	out_frames = rate_change.output_frames_gen;
	if ((out_buff = ast_malloc((size_t) out_frames * sizeof(short))) == NULL) {
		res = -1;
		goto CLEAN3;
	}
	src_float_to_short_array(outp, out_buff, (int) out_frames);
	if ((fl = fopen(fname, "w")) != NULL) {
		if (fwrite(out_buff, sizeof(short), (size_t) out_frames, fl) != (size_t) out_frames) {
			ast_log(LOG_ERROR, "eSpeak: Failed to write resampled output file.\n");
			res = -1;
		}
		fclose(fl);
	} else {
		ast_log(LOG_ERROR, "eSpeak: Failed to open output file for resampling.\n");
		res = -1;
	}
	ast_free(out_buff);
CLEAN3:
	ast_free(outp);
CLEAN2:
	ast_free(inp);
CLEAN1:
	ast_free(in_buff);
	return res;
}

static int configure_espeak(void)
{
	int res = -1;

	ast_mutex_lock(&espk_lock);
	if (espeak_SetParameter(espeakRATE, speed, 0) != EE_OK)
		ast_log(LOG_ERROR, "eSpeak: Failed to set speed=%d.\n", speed);
	else if (espeak_SetParameter(espeakVOLUME, volume, 0) != EE_OK)
		ast_log(LOG_ERROR, "eSpeak: Failed to set volume=%d.\n", volume);
	else if (espeak_SetParameter(espeakWORDGAP, wordgap, 0) != EE_OK)
		ast_log(LOG_ERROR, "eSpeak: Failed to set wordgap=%d.\n", wordgap);
	else if (espeak_SetParameter(espeakPITCH, pitch, 0) != EE_OK)
		ast_log(LOG_ERROR, "eSpeak: Failed to set pitch=%d.\n", pitch);
	else
		res = 0;
	ast_mutex_unlock(&espk_lock);
	return res;
}

static int espeak_exec(struct ast_channel *chan, const char *data)
{
	int res = 0;
	FILE *fl;
	int raw_fd;
	espeak_ERROR espk_error;
	char *mydata, *format;
	int writecache = 0;
	char cachefile[MAXLEN];
	char raw_name[MAXLEN + 16];
	char slin_name[MAXLEN + 24];
	int sample_rate;
	struct synth_data sd = { NULL, 0 };
	int use_cache, t_rate, l_maxtext;
	int l_speed, l_volume, l_wordgap, l_pitch;
	char l_cachedir[MAXLEN];
	char l_voice[64];
	const char *voice;
	AST_DECLARE_APP_ARGS(args,
		AST_APP_ARG(text);
		AST_APP_ARG(interrupt);
		AST_APP_ARG(language);
	);

	if (ast_strlen_zero(data)) {
		ast_log(LOG_ERROR, "eSpeak requires arguments (text and options)\n");
		return -1;
	}

	ast_rwlock_rdlock(&cfg_lock);
	use_cache = usecache;
	t_rate = target_sample_rate;
	l_speed = speed;
	l_volume = volume;
	l_wordgap = wordgap;
	l_pitch = pitch;
	l_maxtext = maxtext;
	ast_copy_string(l_cachedir, cachedir, sizeof(l_cachedir));
	ast_copy_string(l_voice, def_voice, sizeof(l_voice));
	ast_rwlock_unlock(&cfg_lock);

	/* Check before ast_strdupa() copies the data onto the stack. */
	if (strlen(data) > (size_t) l_maxtext) {
		ast_log(LOG_WARNING, "eSpeak: Text too long (max %d bytes).\n", l_maxtext);
		return -1;
	}
	mydata = ast_strdupa(data);
	AST_STANDARD_APP_ARGS(args, mydata);

	if (args.interrupt && !strcasecmp(args.interrupt, "any"))
		args.interrupt = AST_DIGIT_ANY;

	if (!ast_strlen_zero(args.language)) {
		if (strlen(args.language) >= sizeof(l_voice)) {
			ast_log(LOG_WARNING, "eSpeak: Language argument too long.\n");
			return -1;
		}
		voice = args.language;
	} else {
		voice = l_voice;
	}

	args.text = ast_strip_quoted(args.text, "\"", "\"");
	if (ast_strlen_zero(args.text)) {
		ast_log(LOG_WARNING, "eSpeak: No text passed for synthesis.\n");
		return res;
	}

	ast_debug(1,
			  "eSpeak:\nText passed: %s\nInterrupt key(s): %s\nLanguage: %s\nRate: %d\n",
			  args.text, S_OR(args.interrupt, "none"), voice, t_rate);

	if (t_rate == 16000)
		format = "sln16";
	else
		format = "sln";

	/* Cache mechanism */
	if (use_cache) {
		char text_hash[41], key[192], hash[41];
		struct stat st;

		ast_sha1_hash(text_hash, args.text);
		snprintf(key, sizeof(key), "%s|%s|%d|%d|%d|%d|%d", text_hash, voice,
				t_rate, l_speed, l_volume, l_wordgap, l_pitch);
		if (strlen(l_cachedir) + sizeof(hash) + 6 <= MAXLEN) {
			char cachepath[MAXLEN + 8];

			ast_sha1_hash(hash, key);
			snprintf(cachefile, sizeof(cachefile), "%s/%s", l_cachedir, hash);
			snprintf(cachepath, sizeof(cachepath), "%s.%s", cachefile, format);
			writecache = 1;
			if (lstat(cachepath, &st) == 0 && S_ISREG(st.st_mode)
					&& st.st_uid == geteuid() && st.st_size > 0) {
				ast_debug(1, "eSpeak: Serving from cache file %s\n", cachepath);
				if (ast_channel_state(chan) != AST_STATE_UP)
					ast_answer(chan);
				res = ast_streamfile(chan, cachefile, ast_channel_language(chan));
				if (!res) {
					res = ast_waitstream(chan, args.interrupt);
					ast_stopstream(chan);
					return res;
				}
				ast_log(LOG_WARNING, "eSpeak: Bad cache entry %s, regenerating\n", cachepath);
			}
		}
	}

	/* Create the temp file in the cache dir when a cache write is pending,
	 * so the final rename is atomic and on the same filesystem. */
	if (writecache)
		snprintf(raw_name, sizeof(raw_name), "%s/espk_XXXXXX", l_cachedir);
	else
		ast_copy_string(raw_name, "/tmp/espk_XXXXXX", sizeof(raw_name));
	if ((raw_fd = mkstemp(raw_name)) == -1 && writecache) {
		ast_copy_string(raw_name, "/tmp/espk_XXXXXX", sizeof(raw_name));
		raw_fd = mkstemp(raw_name);
	}
	if (raw_fd == -1) {
		ast_log(LOG_ERROR, "eSpeak: Failed to create audio file.\n");
		return -1;
	}
	if ((fl = fdopen(raw_fd, "w+")) == NULL) {
		ast_log(LOG_ERROR, "eSpeak: Failed to open audio file '%s'\n", raw_name);
		close(raw_fd);
		unlink(raw_name);
		return -1;
	}

	ast_mutex_lock(&espk_lock);
	if (espeak_SetVoiceByName(voice) != EE_OK) {
		ast_mutex_unlock(&espk_lock);
		ast_log(LOG_ERROR, "eSpeak: Failed to set voice=%s.\n", voice);
		fclose(fl);
		unlink(raw_name);
		return -1;
	}
	sd.fl = fl;
	espk_error = espeak_Synth(args.text, strlen(args.text) + 1, 0, POS_CHARACTER,
			0, espeakCHARS_AUTO, NULL, &sd);
	sample_rate = espeak_ng_GetSampleRate();
	ast_mutex_unlock(&espk_lock);
	if (fclose(fl))
		sd.err = 1;
	if (espk_error != EE_OK || sd.err) {
		ast_log(LOG_ERROR,
				"eSpeak: Failed to synthesize speech for the specified text.\n");
		unlink(raw_name);
		return -1;
	}

	/* Resample sound file */
	if (sample_rate != t_rate) {
		double ratio = (double) t_rate / (double) sample_rate;
		if (raw_resample(raw_name, ratio) != 0) {
			unlink(raw_name);
			return -1;
		}
	}

	snprintf(slin_name, sizeof(slin_name), "%s.%s", raw_name, format);
	if (rename(raw_name, slin_name)) {
		char ebuf[128];
		ast_log(LOG_ERROR, "eSpeak: Failed to rename audio file: %s\n",
				strerror_r(errno, ebuf, sizeof(ebuf)));
		unlink(raw_name);
		return -1;
	}

	if (ast_channel_state(chan) != AST_STATE_UP)
		ast_answer(chan);
	res = ast_streamfile(chan, raw_name, ast_channel_language(chan));
	if (res) {
		ast_log(LOG_ERROR, "eSpeak: ast_streamfile failed on %s\n", ast_channel_name(chan));
	} else {
		res = ast_waitstream(chan, args.interrupt);
		ast_stopstream(chan);
	}

	/* Save file to cache if set */
	if (writecache) {
		int cfd;
		ast_debug(1, "eSpeak: Saving cache file %s\n", cachefile);
		if ((cfd = open(slin_name, O_RDONLY)) != -1) {
			fsync(cfd);
			close(cfd);
		}
		if (ast_filerename(raw_name, cachefile, format)) {
			ast_log(LOG_WARNING, "eSpeak: Failed to save cache file %s\n", cachefile);
			unlink(slin_name);
		}
	} else {
		unlink(slin_name);
	}
	return res;
}

static int reload_module(void)
{
	read_config(ESPEAK_CONFIG);
	return configure_espeak();
}

static int unload_module(void)
{
	int res = ast_unregister_application(app);

	ast_mutex_lock(&espk_lock);
	espeak_Terminate();
	ast_mutex_unlock(&espk_lock);
	return res;
}

static int load_module(void)
{
	espeak_ng_STATUS result;

	read_config(ESPEAK_CONFIG);
	espeak_ng_InitializePath(NULL);
	if ((result = espeak_ng_Initialize(NULL)) != ENS_OK) {
		ast_log(LOG_ERROR, "eSpeak: Failed to initialize espeak-ng (status %d), aborting.\n",
				result);
		return AST_MODULE_LOAD_DECLINE;
	}
	if ((result = espeak_ng_InitializeOutput(ENOUTPUT_MODE_SYNCHRONOUS, ESPK_BUFFER, NULL)) != ENS_OK) {
		ast_log(LOG_ERROR, "eSpeak: Failed to initialize output (status %d), aborting.\n",
				result);
		espeak_ng_Terminate();
		return AST_MODULE_LOAD_DECLINE;
	}
	espeak_SetSynthCallback(synth_callback);
	if (configure_espeak()) {
		espeak_Terminate();
		return AST_MODULE_LOAD_DECLINE;
	}
	if (ast_register_application_xml(app, espeak_exec)) {
		espeak_Terminate();
		return AST_MODULE_LOAD_DECLINE;
	}
	return AST_MODULE_LOAD_SUCCESS;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_DEFAULT, "eSpeak TTS Interface",
	.load = load_module,
	.unload = unload_module,
	.reload = reload_module,
);
