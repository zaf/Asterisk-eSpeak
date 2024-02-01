/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2009 - 2016, Lefteris Zafiris
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
#include <sys/stat.h>
#include <espeak-ng/speak_lib.h>
#include <espeak-ng/espeak_ng.h>
#include <samplerate.h>
#include "asterisk/app.h"
#include "asterisk/channel.h"
#include "asterisk/module.h"
#include "asterisk/config.h"
#include "asterisk/utils.h"

#define AST_MODULE "eSpeak"
#define ESPEAK_CONFIG "espeak.conf"
#define MAXLEN 4096
#define DEF_RATE 8000
#define DEF_SPEED 150
#define DEF_VOLUME 100
#define DEF_WORDGAP 1
#define DEF_PITCH 50
#define DEF_VOICE "en-us"
#define DEF_DIR "/tmp"
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

static struct ast_config *cfg;
static struct ast_flags config_flags = { 0 };
static const char *cachedir;
static int usecache;
static int target_sample_rate;
static int speed;
static int volume;
static int wordgap;
static int pitch;
static const char *def_voice;

static int read_config(const char *espeak_conf)
{
	const char *temp;
	/* Setting defaut config values */
	cachedir = DEF_DIR;
	usecache = 0;
	target_sample_rate = DEF_RATE;
	speed = DEF_SPEED;
	volume = DEF_VOLUME;
	wordgap = DEF_WORDGAP;
	pitch = DEF_PITCH;
	def_voice = DEF_VOICE;

	cfg = ast_config_load(espeak_conf, config_flags);

	if (!cfg || cfg == CONFIG_STATUS_FILEINVALID) {
		ast_log(LOG_WARNING,
				"eSpeak: Unable to read confing file %s. Using default settings\n", espeak_conf);
	} else {
		if ((temp = ast_variable_retrieve(cfg, "general", "usecache")))
			usecache = ast_true(temp);
		if ((temp = ast_variable_retrieve(cfg, "general", "cachedir")))
			cachedir = temp;
		if ((temp = ast_variable_retrieve(cfg, "general", "samplerate"))) {
			target_sample_rate = (int) strtol(temp, NULL, 10);
			if (errno == ERANGE) {
				ast_log(LOG_WARNING, "eSpeak: Error reading samplerate from config file\n");
				target_sample_rate = DEF_RATE;
			}
		}
		if ((temp = ast_variable_retrieve(cfg, "voice", "speed"))) {
			speed = (int) strtol(temp, NULL, 10);
			if (errno == ERANGE) {
				ast_log(LOG_WARNING, "eSpeak: Error reading voice speed from config file\n");
				speed = DEF_SPEED;
			}
		}
		if ((temp = ast_variable_retrieve(cfg, "voice", "wordgap"))) {
			wordgap = (int) strtol(temp, NULL, 10);
			if (errno == ERANGE) {
				ast_log(LOG_WARNING, "eSpeak: Error reading wordgap from config file\n");
				wordgap = DEF_WORDGAP;
			}
		}
		if ((temp = ast_variable_retrieve(cfg, "voice", "volume"))) {
			volume = (int) strtol(temp, NULL, 10);
			if (errno == ERANGE) {
				ast_log(LOG_WARNING, "eSpeak: Error reading volume from config file\n");
				volume = DEF_VOLUME;
			}
		}
		if ((temp = ast_variable_retrieve(cfg, "voice", "pitch"))) {
			pitch = (int) strtol(temp, NULL, 10);
			if (errno == ERANGE) {
				ast_log(LOG_WARNING, "eSpeak: Error reading pitch from config file\n");
				pitch = DEF_PITCH;
			}
		}
		if ((temp = ast_variable_retrieve(cfg, "voice", "voice")))
			def_voice = temp;
	}

	if (target_sample_rate != 8000 && target_sample_rate != 16000) {
		ast_log(LOG_WARNING,
				"eSpeak: Unsupported sample rate: %d. Falling back to %d\n",
				target_sample_rate, DEF_RATE);
		target_sample_rate = DEF_RATE;
	}
	return 0;
}

/* espeak synthesis callback function */
static int synth_callback(short *wav, int numsamples, espeak_EVENT *events)
{
	if (wav) {
		if (fwrite(wav, sizeof(short), numsamples, events[0].user_data))
			return 0; /* Continue synthesis */
	}
	return 1; /* Stop synthesis */
}

/* Sound data resampling */
static int raw_resample(char *fname, double ratio)
{
	int res = 0;
	FILE *fl;
	struct stat st;
	int in_size;
	short *in_buff, *out_buff;
	long in_frames, out_frames;
	float *inp, *outp;
	SRC_DATA rate_change;

	if ((fl = fopen(fname, "r")) == NULL) {
		ast_log(LOG_ERROR, "eSpeak: Failed to open file for resampling.\n");
		return -1;
	}
	if ((stat(fname, &st) == -1)) {
		ast_log(LOG_ERROR, "eSpeak: Failed to stat file for resampling.\n");
		fclose(fl);
		return -1;
	}
	in_size = st.st_size;
	if ((in_buff = ast_malloc(in_size)) == NULL) {
		fclose(fl);
		return -1;
	}
	if ((fread(in_buff, 1, in_size, fl) != (size_t)in_size)) {
		ast_log(LOG_ERROR, "eSpeak: Failed to read file for resampling.\n");
		fclose(fl);
		res = -1;
		goto CLEAN1;
	}
	fclose(fl);
	in_frames = in_size / 2;

	if ((inp = (float *)(ast_malloc(in_frames * sizeof(float)))) == NULL) {
		res = -1;
		goto CLEAN1;
	}
	src_short_to_float_array(in_buff, inp, in_size/sizeof(short));
	out_frames = (long)((double)in_frames * ratio);
	if ((outp = (float *)(ast_malloc(out_frames * sizeof(float)))) == NULL) {
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

	if ((out_buff = ast_malloc(out_frames*sizeof(short))) == NULL) {
		res = -1;
		goto CLEAN3;
	}
	src_float_to_short_array(rate_change.data_out, out_buff, out_frames);
	if ((fl = fopen(fname, "w+")) != NULL) {
		if ((fwrite(out_buff, 1, 2*out_frames, fl)) != (size_t)(2*out_frames)) {
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
	if ( espeak_SetParameter(espeakRATE, speed, 0) != EE_OK ) {
		ast_log(LOG_ERROR, "eSpeak: Failed to set speed=%d.\n", speed);
		return -1;
	}
	if ( espeak_SetParameter(espeakVOLUME, volume, 0) != EE_OK ) {
		ast_log(LOG_ERROR, "eSpeak: Failed to set volume=%d.\n", volume);
		return -1;
	}
	if ( espeak_SetParameter(espeakWORDGAP, wordgap, 0) != EE_OK ) {
		ast_log(LOG_ERROR, "eSpeak: Failed to set wordgap=%d.\n", wordgap);
		return -1;
	}
	if ( espeak_SetParameter(espeakPITCH, pitch, 0) != EE_OK ) {
		ast_log(LOG_ERROR, "eSpeak: Failed to set pitch=%d.\n", pitch);
		return -1;
	}
	return 0;
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
	char raw_name[17] = "/tmp/espk_XXXXXX";
	char slin_name[23];
	int sample_rate;
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
	mydata = ast_strdupa(data);
	AST_STANDARD_APP_ARGS(args, mydata);

	if (args.interrupt && !strcasecmp(args.interrupt, "any"))
		args.interrupt = AST_DIGIT_ANY;

	if (!ast_strlen_zero(args.language)) {
		voice = args.language;
	} else {
		voice = def_voice;
	}

	args.text = ast_strip_quoted(args.text, "\"", "\"");
	if (ast_strlen_zero(args.text)) {
		ast_log(LOG_WARNING, "eSpeak: No text passed for synthesis.\n");
		return res;
	}

	ast_debug(1,
			  "eSpeak:\nText passed: %s\nInterrupt key(s): %s\nLanguage: %s\nRate: %d\n",
			  args.text, args.interrupt, voice, target_sample_rate);

	/* Cache mechanism */
	if (usecache) {
		char MD5_name[33];
		ast_md5_hash(MD5_name, args.text);
		if (strlen(cachedir) + strlen(MD5_name) + 6 <= MAXLEN) {
			ast_debug(1, "eSpeak: Activating cache mechanism...\n");
			snprintf(cachefile, sizeof(cachefile), "%s/%s", cachedir, MD5_name);
			if (ast_fileexists(cachefile, NULL, NULL) <= 0) {
				ast_debug(1, "eSpeak: Cache file does not yet exist.\n");
				writecache = 1;
			} else {
				ast_debug(1, "eSpeak: Cache file exists.\n");
				if (ast_channel_state(chan) != AST_STATE_UP)
					ast_answer(chan);
				res = ast_streamfile(chan, cachefile, ast_channel_language(chan));
				if (res) {
					ast_log(LOG_ERROR, "eSpeak: ast_streamfile from cache failed on %s\n",
							ast_channel_name(chan));
				} else {
					res = ast_waitstream(chan, args.interrupt);
					ast_stopstream(chan);
					return res;
				}
			}
		}
	}

	/* Set voice - language */
	if ( espeak_SetVoiceByName(voice) != EE_OK ) {
		ast_log(LOG_ERROR, "eSpeak: Failed to set voice=%s.\n", voice);
		return -1;
	}
	if ((raw_fd = mkstemp(raw_name)) == -1) {
		ast_log(LOG_ERROR, "eSpeak: Failed to create audio file.\n");
		return -1;
	}
	if ((fl = fdopen(raw_fd, "w+")) == NULL) {
		ast_log(LOG_ERROR, "eSpeak: Failed to open audio file '%s'\n", raw_name);
		return -1;
	}

	espk_error = espeak_Synth(args.text, strlen(args.text), 0, POS_CHARACTER,
			(int) strlen(args.text), espeakCHARS_AUTO, NULL, fl);
	fclose(fl);
	if (espk_error != EE_OK) {
		ast_log(LOG_ERROR,
				"eSpeak: Failed to synthesize speech for the specified text.\n");
		unlink(raw_name);
		return -1;
	}

	/* Resample sound file */
	sample_rate = espeak_ng_GetSampleRate();
	if (sample_rate != target_sample_rate) {
		double ratio = (double) target_sample_rate / (double) sample_rate;
		if ((res = raw_resample(raw_name, ratio)) != 0) {
			return -1;
		}
	}

	/* Create filenames */
	if (target_sample_rate == 16000) {
		format = "sln16";
	} else {
		format = "sln";
	}
	snprintf(slin_name, sizeof(slin_name), "%s.%s", raw_name, format);
	rename(raw_name, slin_name);

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
		ast_debug(1, "eSpeak: Saving cache file %s\n", cachefile);
		ast_filerename(raw_name, cachefile, format);
	} else {
		unlink(slin_name);
	}
	return res;
}

static int reload_module(void)
{
	ast_config_destroy(cfg);
	if (read_config(ESPEAK_CONFIG)) {
		return -1;
	}
	return configure_espeak();
}

static int unload_module(void)
{
	espeak_Terminate();
	ast_config_destroy(cfg);
	return ast_unregister_application(app);
}

static int load_module(void)
{
	if (read_config(ESPEAK_CONFIG)) {
		return AST_MODULE_LOAD_DECLINE;
	}
	if (espeak_Initialize(AUDIO_OUTPUT_SYNCHRONOUS, ESPK_BUFFER, NULL, 0) == -1) {
		ast_log(LOG_ERROR, "eSpeak: Internal espeak error, aborting.\n");
		ast_config_destroy(cfg);
		return AST_MODULE_LOAD_DECLINE;
	}
	espeak_SetSynthCallback(synth_callback);
	if (configure_espeak()) {
		ast_config_destroy(cfg);
		return AST_MODULE_LOAD_DECLINE;
	}
	if (ast_register_application(app, espeak_exec, NULL, NULL)) {
		ast_config_destroy(cfg);
		return AST_MODULE_LOAD_DECLINE;
	}
	return AST_MODULE_LOAD_SUCCESS;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_DEFAULT, "eSpeak TTS Interface",
	.load = load_module,
	.unload = unload_module,
	.reload = reload_module,
);
