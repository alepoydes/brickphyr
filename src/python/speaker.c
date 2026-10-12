// SPDX-License-Identifier: MIT
// Copyright (c) 2018-2023 The Pybricks Authors
// Adapted from pybricks/common/pb_type_speaker.c.
#include "devices.h"
#include "py/mphal.h"
#include "py/objstr.h"
#include <brickphyr.h>
#include <errno.h>
#include <pybricks/tools.h>
#include <pybricks/tools/pb_type_async.h>
#include <pybricks/util_mp/pb_kwarg_helper.h>
#include <pybricks/util_mp/pb_obj_helper.h>

typedef struct {
	mp_obj_base_t base;
	pb_type_async_t *iter;
	mp_obj_t notes;
	int64_t deadline;
	uint32_t whole_ms, operation;
	uint8_t volume;
	bool active;
} speaker_obj_t;

static void stop_sound(void)
{
	python_device_check(bp_sound_stop());
	struct bp_sound_state sound;
	for (;;) {
		python_device_check(bp_sound_get(&sound));
		if (sound.owner != BP_SOUND_APPLICATION) {
			return;
		}
		if (sound.phase == BP_SOUND_FAILED) {
			python_device_check(sound.cleanup_error);
		}
		mp_hal_delay_ms(1);
	}
}

static mp_obj_t speaker_close(mp_obj_t self_in)
{
	speaker_obj_t *self = MP_OBJ_TO_PTR(self_in);
	if (self->active) {
		pb_type_async_schedule_stop_iteration(self->iter);
		self->notes = MP_OBJ_NULL;
		stop_sound();
		self->active = false;
	}
	return mp_const_none;
}

static void begin(speaker_obj_t *self)
{
	struct bp_sound_state sound;
	python_device_check(bp_sound_get(&sound));
	if (sound.owner != BP_SOUND_FREE && sound.owner != BP_SOUND_APPLICATION) {
		python_device_check(-EBUSY);
	}
	pb_type_async_schedule_stop_iteration(self->iter);
	self->notes = MP_OBJ_NULL;
	if (sound.owner == BP_SOUND_APPLICATION) {
		stop_sound();
	}
	self->active = false;
	python_device_check(bp_sound_reserve());
	self->operation++;
	self->active = true;
	self->deadline = 0;
}

/* The reference's equal-temperament table, octave range and articulation.
 * Bound every read, reject zero divisors and consume the whole note token. */
static void parse_note(mp_obj_t item, uint32_t whole_ms,
	uint32_t *frequency, uint32_t *total_ms, uint32_t *on_ms)
{
	size_t length;
	const byte *text = (const byte *)mp_obj_str_get_data(item, &length);
	if (length < 3 || length > 8) {
		goto invalid;
	}
	size_t pos = 1;
	*frequency = 0;
	if (text[0] != 'R') {
		static const uint8_t semitones[] = {9, 11, 0, 2, 4, 5, 7}; /* A..G */
		static const uint16_t hundredths[] = {
			1635, 1732, 1835, 1945, 2060, 2183, 2312, 2450, 2596, 2750, 2914, 3087,
		};
		if (text[0] < 'A' || text[0] > 'G') goto invalid;
		unsigned int pitch = semitones[text[0] - 'A'];
		if (text[pos] == '#') {
			if (text[0] == 'E' || text[0] == 'B') goto invalid;
			pitch++;
			pos++;
		} else if (text[pos] == 'b') {
			if (text[0] == 'C' || text[0] == 'F') goto invalid;
			pitch--;
			pos++;
		}
		if (pos >= length || text[pos] < '2' || text[pos] > '8') goto invalid;
		*frequency = ((uint32_t)hundredths[pitch] << (text[pos++] - '0')) / 100;
	}
	if (pos >= length || text[pos++] != '/') goto invalid;
	if (pos >= length || text[pos] < '0' || text[pos] > '9') goto invalid;
	unsigned int fraction = text[pos++] - '0';
	if (pos < length && text[pos] >= '0' && text[pos] <= '9') {
		fraction = 10 * fraction + text[pos++] - '0';
	}
	if (!fraction) goto invalid;
	*total_ms = whole_ms / fraction;
	if (pos < length && text[pos] == '.') {
		*total_ms = 3 * *total_ms / 2;
		pos++;
	}
	bool slur = pos < length && text[pos] == '_';
	pos += slur;
	if (pos != length) goto invalid;
	*on_ms = slur ? *total_ms : 7 * *total_ms / 8;
	return;
invalid:
	mp_raise_ValueError(MP_ERROR_TEXT("invalid note: expected A-G[#,b]2-8/N[.][_] or R/N[.][_]"));
}

static pbio_error_t advance(speaker_obj_t *self)
{
	if (!self->active) {
		return PBIO_SUCCESS;
	}
	struct bp_sound_state sound;
	python_device_check(bp_sound_get(&sound));
	python_device_check(sound.error);
	python_device_check(sound.cleanup_error);
	if (sound.owner != BP_SOUND_APPLICATION) {
		python_device_check(-EACCES);
	}
	if (sound.phase != BP_SOUND_IDLE || bp_time_ms() < self->deadline) {
		return PBIO_ERROR_AGAIN;
	}
	if (self->notes != MP_OBJ_NULL) {
		uint32_t operation = self->operation;
		mp_obj_t item = mp_iternext(self->notes);
		/* A generator may itself start newer playback. */
		if (operation != self->operation) return PBIO_SUCCESS;
		if (item != MP_OBJ_STOP_ITERATION) {
			uint32_t frequency, total_ms, on_ms;
			parse_note(item, self->whole_ms, &frequency, &total_ms, &on_ms);
			self->deadline = bp_time_ms() + total_ms;
			/* Native duration zero is indefinite, unlike a rounded-down note. */
			if (on_ms) {
				python_device_check(bp_sound_tone(frequency, on_ms, self->volume));
			}
			return PBIO_ERROR_AGAIN;
		}
	}
	speaker_close(MP_OBJ_FROM_PTR(self));
	return PBIO_SUCCESS;
}

static void close_preserving_error(speaker_obj_t *self, uint32_t operation, void *error)
{
	nlr_buf_t cleanup;
	if (nlr_push(&cleanup) == 0) {
		if (operation == self->operation) speaker_close(MP_OBJ_FROM_PTR(self));
		nlr_pop();
	}
	nlr_jump(error);
}

static pbio_error_t speaker_next(pbio_os_state_t *state, mp_obj_t self_in)
{
	speaker_obj_t *self = MP_OBJ_TO_PTR(self_in);
	uint32_t operation = self->operation;
	nlr_buf_t nlr;
	if (nlr_push(&nlr) == 0) {
		pbio_error_t result = advance(self);
		nlr_pop();
		return result;
	}
	close_preserving_error(self, operation, nlr.ret_val);
	return PBIO_SUCCESS; /* nlr_jump does not return. */
}

static mp_obj_t wait_or_await(speaker_obj_t *self)
{
	pb_type_async_t config = {
		.parent_obj = MP_OBJ_FROM_PTR(self), .iter_once = speaker_next, .close = speaker_close,
	};
	if (pb_module_tools_run_loop_is_active()) {
		return pb_type_async_wait_or_await(&config, &self->iter, true);
	}
	/* The common DMA service, rather than a PBIO timer, signals completion.
	 * Poll only its bounded snapshot; waveform generation stays in the service. */
	while (advance(self) == PBIO_ERROR_AGAIN) {
		mp_hal_delay_ms(1);
	}
	return mp_const_none;
}

static mp_obj_t speaker_volume(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, pos_args, kw_args, speaker_obj_t, self, PB_ARG_DEFAULT_NONE(volume));
	if (volume_in == mp_const_none) {
		return MP_OBJ_NEW_SMALL_INT(self->volume);
	}
	self->volume = pb_obj_get_pct(volume_in);
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(speaker_volume_obj, 1, speaker_volume);

static mp_obj_t speaker_beep(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, pos_args, kw_args, speaker_obj_t, self,
		PB_ARG_DEFAULT_INT(frequency, 500), PB_ARG_DEFAULT_INT(duration, 100));
	mp_int_t frequency = pb_obj_get_int(frequency_in), duration = pb_obj_get_int(duration_in);
	/* Preserve the sampled Pybricks driver's useful clamp, without converting
	 * negative frequencies to unsigned ultrasound. Zero is explicitly silent. */
	if (frequency) frequency = frequency < 64 ? 64 : frequency > 24000 ? 24000 : frequency;
	uint32_t operation = self->operation + 1;
	nlr_buf_t nlr;
	if (nlr_push(&nlr) == 0) {
		begin(self);
		if (duration) {
			python_device_check(bp_sound_tone(frequency, duration < 0 ? 0 : duration, self->volume));
		}
		mp_obj_t result = duration < 0 ? mp_const_none : wait_or_await(self);
		nlr_pop();
		return result;
	}
	close_preserving_error(self, operation, nlr.ret_val);
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(speaker_beep_obj, 1, speaker_beep);

static mp_obj_t speaker_play_notes(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
	PB_PARSE_ARGS_METHOD(n_args, pos_args, kw_args, speaker_obj_t, self,
		PB_ARG_REQUIRED(notes), PB_ARG_DEFAULT_INT(tempo, 120));
	mp_int_t tempo = pb_obj_get_int(tempo_in);
	if (tempo <= 0) {
		mp_raise_ValueError(MP_ERROR_TEXT("tempo must be positive"));
	}
	mp_obj_t notes = mp_getiter(notes_in, NULL);
	uint32_t operation = self->operation + 1;
	nlr_buf_t nlr;
	if (nlr_push(&nlr) == 0) {
		begin(self);
		self->notes = notes;
		self->whole_ms = 240000 / tempo;
		mp_obj_t result = wait_or_await(self);
		nlr_pop();
		return result;
	}
	close_preserving_error(self, operation, nlr.ret_val);
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(speaker_play_notes_obj, 1, speaker_play_notes);

static const mp_rom_map_elem_t speaker_methods[] = {
	{ MP_ROM_QSTR(MP_QSTR_volume), MP_ROM_PTR(&speaker_volume_obj) },
	{ MP_ROM_QSTR(MP_QSTR_beep), MP_ROM_PTR(&speaker_beep_obj) },
	{ MP_ROM_QSTR(MP_QSTR_play_notes), MP_ROM_PTR(&speaker_play_notes_obj) },
};
static MP_DEFINE_CONST_DICT(speaker_dict, speaker_methods);
static MP_DEFINE_CONST_OBJ_TYPE(speaker_type, MP_QSTR_Speaker, MP_TYPE_FLAG_NONE, locals_dict, &speaker_dict);

mp_obj_t python_speaker_new(void)
{
	if (MP_STATE_PORT(python_hub_speaker) == MP_OBJ_NULL) {
		speaker_obj_t *self = mp_obj_malloc(speaker_obj_t, &speaker_type);
		*self = (speaker_obj_t){.base = {.type = &speaker_type}, .volume = 100};
		MP_STATE_PORT(python_hub_speaker) = MP_OBJ_FROM_PTR(self);
	}
	return MP_STATE_PORT(python_hub_speaker);
}

MP_REGISTER_ROOT_POINTER(mp_obj_t python_hub_speaker);
