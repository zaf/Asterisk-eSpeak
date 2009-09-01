/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2009, Lefteris Zafiris
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
 * the GNU General Public License Version 2. See the LICENSE file
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
#include <libresample.h>
#include "asterisk/file.h"
#include "asterisk/channel.h"
#include "asterisk/module.h"
#include "asterisk/config.h"
#include "asterisk/app.h"
#include "asterisk/utils.h"

#define AST_MODULE "eSpeak"
#define ESPEAK_CONFIG "espeak.conf"
#define MAXLEN 2048

static char *app = "eSpeak";

static char *synopsis = "Say text to the user, using eSpeak speech synthesizer.";

static char *descrip =
"  eSpeak(text[,intkeys,language]):  This will invoke the eSpeak TTS engine,\n"
"send a text string, get back the resulting waveform and play it to\n"
"the user, allowing any given interrupt keys to immediately terminate\n"
"and return.\n";

static int synth_callback(short *wav, int numsamples, espeak_EVENT * events) {
	if (wav) {
		fwrite(wav, sizeof(short), numsamples, events[0].user_data);
	}
	return 0;
}

static int app_exec(struct ast_channel *chan, void *data)
{
	int res = 0;
	SNDFILE *src_file, *dst_file;
	SF_INFO src_info, dst_info;
	FILE *fl;
	int src_len, dst_len;
	float *src, *dst;
	void *handle;
	double ratio;
	const char *mydata;
	const char *cachedir = "";
	const char *temp;
	int usecache = 0;
	int writecache = 0;
	char MD5_name[33] = "";
	char cachefile[MAXLEN] = "";
	char tmp_name[23];
	char wav_or_name[30];
	char wav_name[27];
	int bits = 16;
	int *offset, sample_rate, pos, bufferpos, outcount;
	float file_size;
	int speed = 150;
	int volume = 100;
	int wordgap = 1;
	int pitch = 50;
	int capind = 0;
	const char *voice = "default";
	static unsigned char wave_hdr[44] = {
		'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
		0x10, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		2, 0, 0x10, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
	struct ast_config *cfg;
	struct ast_flags config_flags = { 0 };
	AST_DECLARE_APP_ARGS(args,
		AST_APP_ARG(text);
		AST_APP_ARG(interrupt);
		AST_APP_ARG(language);
	);

	if (ast_strlen_zero(data)) {
		ast_log(AST_LOG_ERROR, "eSpeak requires an argument (text)\n");
		return -1;
	}

	cfg = ast_config_load(ESPEAK_CONFIG, config_flags);

	if (!cfg) {
		ast_log(LOG_WARNING,
				"eSpeak: No such configuration file %s using default settings\n",
				ESPEAK_CONFIG);
	} else {
		if ((temp = ast_variable_retrieve(cfg, "general", "usecache")))
			usecache = ast_true(temp);

		if (!(cachedir = ast_variable_retrieve(cfg, "general", "cachedir")))
			cachedir = "/tmp";

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
			voice = temp;
	}

	mydata = ast_strdupa(data);
	AST_STANDARD_APP_ARGS(args, mydata);

	if (args.interrupt && !strcasecmp(args.interrupt, "any"))
		args.interrupt = AST_DIGIT_ANY;
	
	if (strcasecmp(args.language, ""))
		voice = args.language;

	ast_debug(1, "eSpeak: Text passed: %s\nInterrupt key(s): %s\nLanguage: %s\n", args.text,
				args.interrupt, voice);

	/*Cache mechanism */
	if (usecache) {
		ast_md5_hash(MD5_name, args.text);
		if (strlen(cachedir) + strlen(MD5_name) + 5 <= MAXLEN) {
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
					ast_log(AST_LOG_ERROR, "eSpeak: ast_streamfile failed on %s\n", 
							chan->name);
				} else {
					res = ast_waitstream(chan, args.interrupt);
					ast_stopstream(chan);
					ast_config_destroy(cfg);
					return res;
				}
			}
		}
	}

	/* Create temp filenames */
	snprintf(tmp_name, sizeof(tmp_name), "/tmp/eSpeak_%li", ast_random());
	snprintf(wav_name, sizeof(wav_name), "%s.wav", tmp_name);
	snprintf(wav_or_name, sizeof(wav_or_name), "%s_or.wav", tmp_name);

	/* Invoke eSpeak */
	sample_rate = espeak_Initialize(AUDIO_OUTPUT_SYNCHRONOUS, 2000, NULL, 0);
	espeak_SetSynthCallback(synth_callback);
	espeak_SetVoiceByName(voice);
	espeak_SetParameter(espeakRATE, speed, 0);
	espeak_SetParameter(espeakVOLUME, volume, 0);
	espeak_SetParameter(espeakWORDGAP, wordgap, 0);
	espeak_SetParameter(espeakPITCH, pitch, 0);
	espeak_SetParameter(espeakCAPITALS, capind, 0);
	
	fl = fopen(wav_or_name, "w+");
	if (fl = NULL) {
		ast_log(LOG_ERROR, "eSpeak: Failed to create audio buffer file '%s'\n", wav_or_name);
		ast_config_destroy(cfg);
		return -1;
	} else {
		/* Copy wav header */
		offset = (int *) (&wave_hdr[24]);
		offset[0] = sample_rate;
		offset[1] = sample_rate * (bits/8);
		fwrite(wave_hdr, 1, sizeof(wave_hdr), fl);
	}

	espeak_Synth(args.text, strlen(args.text) + 1, 0, POS_CHARACTER, 0, espeakCHARS_AUTO, NULL, fl);
	espeak_Terminate();

	/* Fix file header */
	file_size = ftell(fl);
	offset = (int *) (&wave_hdr[4]);
	offset[0] = (int) file_size - 8;
	offset = (int *) (&wave_hdr[40]);
	offset[0] = (int) file_size - 44;
	fseek(fl, 0, SEEK_SET);
	fwrite(wave_hdr, 1, sizeof(wave_hdr), fl);
	fclose(fl);

	/* Resample file */
	ratio = (double) ((double) 8000 / (double) sample_rate);
	memset(&src_info, 0, sizeof(src_info));
	memset(&dst_info, 0, sizeof(dst_info));
	src_file = sf_open(wav_or_name, SFM_READ, &src_info);
	if (!src_file) {
		ast_log(LOG_ERROR, "eSpeak: Failed to read audio file '%s'\n", wav_or_name);
		ast_config_destroy(cfg);
		remove(wav_or_name);
		return -1;
	}
	
	memcpy(&dst_info, &src_info, sizeof(SF_INFO));
	dst_info.samplerate = 8000;
	dst_file = sf_open(wav_name, SFM_WRITE, &dst_info);
	if (!dst_file) {
		ast_log(LOG_ERROR, "eSpeak: Failed to create audio file '%s'\n", wav_name);
		ast_config_destroy(cfg);
		sf_close(src_file);
		remove(wav_or_name);
		return -1;
	}
	dst_info.frames = src_info.frames * (sf_count_t)8000 / (sf_count_t)sample_rate;

	src_len = 16384;
	dst_len = (src_len * ratio + 1000);
	src = (float *) malloc(src_len * sizeof(float));
	dst = (float *) malloc(dst_len * sizeof(float));

	handle = (void *) malloc(sizeof(void *));
	handle = resample_open(1, ratio, ratio);
	if (!handle) {
		ast_log(LOG_ERROR, "eSpeak: Failed open resampler\n");
		ast_config_destroy(cfg);
		sf_close(src_file);
		sf_close(dst_file);
		free(src);
		free(dst);
		remove(wav_or_name);
		return -1;
	}
	
	pos = 0;
	bufferpos = 0;
	outcount = 0;
	
	while (pos < src_info.frames) {
		int block = MIN(src_len - bufferpos, src_info.frames - pos);
		int last_flag = (pos + block == src_info.frames);
		int in_used, out = 0;
		sf_readf_float(src_file, &src[bufferpos], block);
		block += bufferpos;
		in_used = 0;
		out = resample_process(handle, ratio, src, block, last_flag, &in_used, dst, dst_len);
		sf_writef_float(dst_file, dst, out);
		bufferpos = block - in_used;
		pos += in_used;
		outcount += out;
	}
	
	resample_close(handle);
	sf_write_sync(dst_file);
	sf_close(src_file);
	sf_close(dst_file);
	free(src);
	free(dst);
	remove(wav_or_name);

	/* Save file to cache if set */
	if (writecache) {
		ast_debug(1, "eSpeak: Saving cache file %s\n", cachefile);
		ast_filecopy(tmp_name, cachefile, NULL);
	}

	if (chan->_state != AST_STATE_UP)
		ast_answer(chan);
	res = ast_streamfile(chan, tmp_name, chan->language);
	if (res) {
		ast_log(LOG_ERROR, "eSpeak: ast_streamfile failed on %s\n", chan->name);
	} else {
		res = ast_waitstream(chan, args.interrupt);
		ast_stopstream(chan);
	}

	ast_filedelete(tmp_name, NULL);
	ast_config_destroy(cfg);
	return res;
}

static int unload_module(void)
{
	return ast_unregister_application(app);
}

static int load_module(void)
{
	return ast_register_application(app, app_exec, synopsis, descrip) ?
		AST_MODULE_LOAD_DECLINE : AST_MODULE_LOAD_SUCCESS;
}

AST_MODULE_INFO_STANDARD(ASTERISK_GPL_KEY, "eSpeak TTS Interface");
