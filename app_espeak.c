/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2009 - 2011, Lefteris Zafiris
 *
 * Lefteris Zafiris <zaf.000@gmail.com>
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
 * \brief Say text to the user, using eSpeak TTS engine.
 *
 * \author\verbatim Lefteris Zafiris <zaf.000@gmail.com> \endverbatim
 *
 * \extref eSpeak text to speech Synthesis System - http://espeak.sourceforge.net/
 *
 * \ingroup applications
 */

/*** MODULEINFO
	<defaultenabled>no</defaultenabled>
 ***/

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision: 00 $")
#include <stdio.h>
#include <espeak/speak_lib.h>
#include <sndfile.h>
#include <samplerate.h>
#include "asterisk/file.h"
#include "asterisk/channel.h"
#include "asterisk/module.h"
#include "asterisk/config.h"
#include "asterisk/app.h"
#include "asterisk/utils.h"
#include "asterisk/strings.h"

#define AST_MODULE "eSpeak"
#define ESPEAK_CONFIG "espeak.conf"
#define MAXLEN 2048
#define DEF_RATE 8000
#define DEF_SPEED 150
#define DEF_VOLUME 100
#define DEF_WORDGAP 1
#define DEF_PITCH 50
#define DEF_CAPIND 0
#define DEF_VOICE "default"
#define DEF_DIR "/tmp"
/* libsndfile formats */
#define RAW_PCM_S16LE 262146
#define WAV_PCM_S16LE 65538

static char *app = "eSpeak";
static char *synopsis = "Say text to the user, using eSpeak speech synthesizer.";
static char *descrip =
	"  eSpeak(text[,intkeys,language]):  This will invoke the eSpeak TTS engine,\n"
	"send a text string, get back the resulting waveform and play it to\n"
	"the user, allowing any given interrupt keys to immediately terminate\n"
	"and return.\n";

static struct ast_config *cfg;
static struct ast_flags config_flags = { 0 };
static const char *cachedir;
static int usecache;
static double target_sample_rate;
static int speed;
static int volume;
static int wordgap;
static int pitch;
static int capind;
static const char *def_voice;

static int read_config(void)
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
	capind = DEF_CAPIND;
	def_voice = DEF_VOICE;

	cfg = ast_config_load(ESPEAK_CONFIG, config_flags);

	if (!cfg || cfg == CONFIG_STATUS_FILEINVALID) {
		ast_log(LOG_WARNING,
				"eSpeak: Unable to read confing file %s. Using default settings\n",
				ESPEAK_CONFIG);
	} else {
		if ((temp = ast_variable_retrieve(cfg, "general", "usecache")))
			usecache = ast_true(temp);

		if ((temp = ast_variable_retrieve(cfg, "general", "cachedir")))
			cachedir = temp;

		if ((temp = ast_variable_retrieve(cfg, "general", "samplerate")))
			target_sample_rate = atoi(temp);

		if ((temp = ast_variable_retrieve(cfg, "voice", "speed")))
			speed = atoi(temp);

		if ((temp = ast_variable_retrieve(cfg, "voice", "wordgap")))
			wordgap = atoi(temp);

		if ((temp = ast_variable_retrieve(cfg, "voice", "volume")))
			volume = atoi(temp);

		if ((temp = ast_variable_retrieve(cfg, "voice", "pitch")))
			pitch = atoi(temp);

		if ((temp = ast_variable_retrieve(cfg, "voice", "capind")))
			capind = atoi(temp);

		if ((temp = ast_variable_retrieve(cfg, "voice", "voice")))
			def_voice = temp;
	}
	
	if (target_sample_rate != 8000 && target_sample_rate != 16000) {
		ast_log(LOG_WARNING,
				"eSpeak: Unsupported sample rate: %lf. Falling back to %d\n",
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

static int espeak_exec(struct ast_channel *chan, const char *data)
{
	int res = 0;
	SNDFILE *src_file;
	SF_INFO src_info;
	FILE *fl;
	int raw_fd;
	sf_count_t trun_frames = 0;
	sf_count_t dst_frames;
	SRC_DATA rate_change;
	espeak_ERROR espk_error;
	float *src_buff, *dst_buff;
	char *mydata;
	int writecache = 0;
	char MD5_name[33] = "";
	char cachefile[MAXLEN] = "";
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
			  "eSpeak:\nText passed: %s\nInterrupt key(s): %s\nLanguage: %s\nRate: %lf\n",
			  args.text, args.interrupt, voice, target_sample_rate);

	/*Cache mechanism */
	if (usecache) {
		ast_md5_hash(MD5_name, args.text);
		if (strlen(cachedir) + strlen(MD5_name) + 6 <= MAXLEN) {
			ast_debug(1, "eSpeak: Activating cache mechanism...\n");
			snprintf(cachefile, sizeof(cachefile), "%s/%s", cachedir, MD5_name);
			if (ast_fileexists(cachefile, NULL, NULL) <= 0) {
				ast_debug(1, "eSpeak: Cache file does not yet exist.\n");
				writecache = 1;
			} else {
				ast_debug(1, "eSpeak: Cache file exists.\n");
				if (chan->_state != AST_STATE_UP)
					ast_answer(chan);
				res = ast_streamfile(chan, cachefile, chan->language);
				if (res) {
					ast_log(LOG_ERROR, "eSpeak: ast_streamfile from cache failed on %s\n",
							chan->name);
				} else {
					res = ast_waitstream(chan, args.interrupt);
					ast_stopstream(chan);
					return res;
				}
			}
		}
	}

	/* Invoke eSpeak */
	sample_rate = espeak_Initialize(AUDIO_OUTPUT_SYNCHRONOUS, 2000, NULL, 0);

	if (sample_rate == -1) {
		ast_log(LOG_ERROR, "eSpeak: Internal espeak error, aborting.\n");
		return -1;
	}
	espeak_SetSynthCallback(synth_callback);
	espeak_SetVoiceByName(voice);
	espeak_SetParameter(espeakRATE, speed, 0);
	espeak_SetParameter(espeakVOLUME, volume, 0);
	espeak_SetParameter(espeakWORDGAP, wordgap, 0);
	espeak_SetParameter(espeakPITCH, pitch, 0);
	espeak_SetParameter(espeakCAPITALS, capind, 0);

	raw_fd = mkstemp(raw_name);

	if (raw_fd == -1) {
		ast_log(LOG_ERROR, "eSpeak: Failed to create audio file.\n");
		return -1;
	}

	fl = fdopen(raw_fd, "w+");
	if (fl == NULL) {
		ast_log(LOG_ERROR, "eSpeak: Failed to open audio file '%s'\n", raw_name);
		return -1;
	}

	espk_error =
		espeak_Synth(args.text, strlen(args.text), 0, POS_CHARACTER,
					 (int) strlen(args.text), espeakCHARS_AUTO, NULL, fl);
	espeak_Terminate();
	fclose(fl);
	if (espk_error != EE_OK) {
		ast_log(LOG_ERROR,
				"eSpeak: Failed to synthesize speech for the specified text.\n");
		ast_filedelete(raw_name, NULL);
		return -1;
	}

	/* Resample sound file */
	if (sample_rate != target_sample_rate) {
		memset(&src_info, 0, sizeof(src_info));
		src_info.samplerate = (int) sample_rate;
		src_info.channels = 1;
		src_info.format = RAW_PCM_S16LE;
		src_file = sf_open(raw_name, SFM_RDWR, &src_info);
		if (src_file == NULL) {
			ast_log(LOG_ERROR, "eSpeak: Failed to read raw audio data '%s'\n", raw_name);
			ast_filedelete(raw_name, NULL);
			return -1;
		}
		src_buff = (float *) ast_malloc(src_info.frames * sizeof(float));
		sf_readf_float(src_file, src_buff, src_info.frames);
		dst_frames =
			src_info.frames * (sf_count_t) target_sample_rate / (sf_count_t) sample_rate;
		dst_buff = (float *) ast_malloc(dst_frames * sizeof(float));

		rate_change.data_in = src_buff;
		rate_change.data_out = dst_buff;
		rate_change.input_frames = src_info.frames;
		rate_change.output_frames = dst_frames;
		rate_change.src_ratio = (double) (target_sample_rate / sample_rate);

		res = src_simple(&rate_change, SRC_SINC_FASTEST, 1);
		if (res) {
			ast_log(LOG_ERROR, "eSpeak: Failed to resample sound file '%s': '%s'\n",
					raw_name, src_strerror(res));
			sf_close(src_file);
			ast_free(src_buff);
			ast_free(dst_buff);
			ast_filedelete(raw_name, NULL);
			return -1;
		}
		src_info.frames = dst_frames;
		src_info.samplerate = target_sample_rate;
		sf_command(src_file, SFC_FILE_TRUNCATE, &trun_frames, sizeof(trun_frames));
		sf_writef_float(src_file, dst_buff, src_info.frames);
		sf_write_sync(src_file);
		sf_close(src_file);
		ast_free(src_buff);
		ast_free(dst_buff);
	}

	/* Create filenames */
	if (target_sample_rate == 8000)
		snprintf(slin_name, sizeof(slin_name), "%s.sln", raw_name);
	if (target_sample_rate == 16000)
		snprintf(slin_name, sizeof(slin_name), "%s.sln16", raw_name);
	rename(raw_name, slin_name);

	/* Save file to cache if set */
	if (writecache) {
		ast_debug(1, "eSpeak: Saving cache file %s\n", cachefile);
		ast_filecopy(raw_name, cachefile, NULL);
	}

	if (chan->_state != AST_STATE_UP)
		ast_answer(chan);
	res = ast_streamfile(chan, raw_name, chan->language);
	if (res) {
		ast_log(LOG_ERROR, "eSpeak: ast_streamfile failed on %s\n", chan->name);
	} else {
		res = ast_waitstream(chan, args.interrupt);
		ast_stopstream(chan);
	}

	ast_filedelete(raw_name, NULL);
	return res;
}

static int reload(void)
{
	ast_config_destroy(cfg);
	read_config();
	return 0;
}

static int unload_module(void)
{
	ast_config_destroy(cfg);
	return ast_unregister_application(app);
}

static int load_module(void)
{
	read_config();
	return ast_register_application(app, espeak_exec, synopsis, descrip) ?
		AST_MODULE_LOAD_DECLINE : AST_MODULE_LOAD_SUCCESS;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_DEFAULT, "eSpeak TTS Interface",
		.load = load_module,
		.unload = unload_module,
		.reload = reload,
			);
