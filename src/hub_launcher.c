// SPDX-License-Identifier: Apache-2.0
#define __ZEPHYR_SUPERVISOR__
#include "hub_launcher.h"
#include "app_store.h"
#include "application.h"
#include "host_transport.h"
#include "hub_matrix.h"
#include "storage_work.h"

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

LOG_MODULE_REGISTER(hub_launcher);
static atomic_t stopping;

enum job_kind { JOB_BROWSE, JOB_START };
enum { FIRST_HOLD_MS = 700, LETTER_HOLD_MS = 450, FADE_FRAME_MS = 20,
	FADE_STEPS = 6, GAP_MS = 40, SWEEP_FRAME_MS = 20,
	LAUNCH_HOLD_MS = 40, LAUNCH_FADE_MS = 25 };
static struct {
	struct k_spinlock lock;
	bool busy, done;
	enum job_kind kind;
	uint32_t generation;
	int direction, result;
	/* The worker replaces the request with its browsed name on completion. */
	char request[APP_NAME_MAX + 1];
} job;

static struct {
	bool visible, pybricks, launching, dirty, need_initial_browse, start_pending;
	bool waiting_selection, launch_ready;
	uint8_t slot, launch_slot;
	uint32_t generation;
	/* Pybricks displays slot; this keeps the Brickphyr selection in RAM. */
	char name[APP_NAME_MAX + 1];
	int browse_error;
	int8_t queued_direction, sweep_direction;
	int64_t frame_at, error_until;
	uint8_t offset, fade_step, effect_step;
	enum { PREVIEW_HOLD, PREVIEW_FADE, PREVIEW_GAP, PREVIEW_DONE } preview;
	enum { EFFECT_NONE, EFFECT_SWEEP, EFFECT_LAUNCH } effect;
} ui;

static bool can_start(void)
{
	if (atomic_get(&stopping)) return false;
	struct application_status status;
	if (application_observe(&status) || status.state == APPLICATION_RUNNING || status.repl) {
		return false;
	}
#if defined(CONFIG_USB_DEVICE_STACK_NEXT) || defined(CONFIG_BRICKPHYR_CC2640_BLE)
	if (host_session_reserved()) return false;
#endif
	return true;
}

static void storage_job(struct k_work *work)
{
	char name[APP_NAME_MAX + 1];
	k_spinlock_key_t key = k_spin_lock(&job.lock);
	enum job_kind kind = job.kind;
	int direction = job.direction;
	strcpy(name, job.request);
	k_spin_unlock(&job.lock, key);
	struct app_file entry = {0};
	int err;
	if (kind == JOB_BROWSE) {
		err = app_store_browse(name, direction, &entry);
	} else {
		err = hub_matrix_release(BP_MATRIX_SYSTEM);
		if (err) LOG_ERR("Program display handoff failed: %d", err);
		if (!err) err = can_start() ? app_store_run(name, 0) : -EBUSY;
	}
	key = k_spin_lock(&job.lock);
	job.result = err;
	strcpy(job.request, entry.name);
	job.busy = false;
	job.done = true;
	k_spin_unlock(&job.lock, key);
}
static K_WORK_DEFINE(storage_task, storage_job);

void hub_launcher_shutdown(void)
{
	atomic_set(&stopping, 1);
	struct k_work_sync sync;
	k_work_flush(&storage_task, &sync);
}

static int submit(enum job_kind kind, const char *name, int direction)
{
	k_spinlock_key_t key = k_spin_lock(&job.lock);
	if (job.busy || job.done) {
		k_spin_unlock(&job.lock, key);
		return -EBUSY;
	}
	job.busy = true;
	job.kind = kind;
	job.generation = ui.generation;
	job.direction = direction;
	strcpy(job.request, name);
	k_spin_unlock(&job.lock, key);
	int err = storage_work_submit(&storage_task);
	if (err < 0) {
		key = k_spin_lock(&job.lock);
		job.busy = false;
		k_spin_unlock(&job.lock, key);
	}
	return err < 0 ? err : 0;
}

static bool job_available(void)
{
	k_spinlock_key_t key = k_spin_lock(&job.lock);
	bool available = !job.busy && !job.done;
	k_spin_unlock(&job.lock, key);
	return available;
}

static void select_name(const char *name, int error, int64_t now)
{
	if (name != ui.name) strcpy(ui.name, name);
	ui.browse_error = error;
	ui.error_until = 0;
	ui.frame_at = now + FIRST_HOLD_MS;
	ui.offset = 0;
	ui.fade_step = 0;
	ui.preview = PREVIEW_HOLD;
	ui.dirty = true;
}

static bool finish_job(int64_t now)
{
	char response[APP_NAME_MAX + 1];
	k_spinlock_key_t key = k_spin_lock(&job.lock);
	if (!job.done) {
		k_spin_unlock(&job.lock, key);
		return false;
	}
	enum job_kind kind = job.kind;
	uint32_t generation = job.generation;
	int result = job.result;
	strcpy(response, job.request);
	job.done = false;
	k_spin_unlock(&job.lock, key);
	if (kind == JOB_START) ui.launching = false;
	if (!ui.visible || generation != ui.generation) return false;
	if (kind == JOB_BROWSE) {
		if (result && result != -ENOENT) LOG_ERR("Program browse failed: %d", result);
		if (ui.waiting_selection) {
			strcpy(ui.name, result ? "" : response);
			ui.browse_error = result;
		} else {
			select_name(result ? "" : response, result, now);
		}
	} else if (result) {
		if (result != -ENOENT) LOG_ERR("Program start failed: %d", result);
		ui.error_until = now + 700;
		ui.dirty = true;
	} else {
		ui.visible = false;
		ui.generation++;
		return true;
	}
	return false;
}

extern const uint8_t pb_font_5x5[95][5];

static void glyph(char character, uint8_t brightness, uint8_t frame[BP_MATRIX_PIXELS])
{
	if (character < 32 || character > 126) character = '?';
	for (size_t row = 0; row < 5; row++) {
		for (size_t col = 0; col < 5; col++) {
			frame[row * 5 + col] =
				pb_font_5x5[character - 32][row] & BIT(4 - col) ? brightness : 0;
		}
	}
}

static void preview_frame(uint8_t frame[BP_MATRIX_PIXELS], int64_t now)
{
	if (ui.effect == EFFECT_SWEEP) {
		memset(frame, 0, BP_MATRIX_PIXELS);
		if (ui.effect_step < 5) {
			int col = ui.sweep_direction > 0 ? ui.effect_step : 4 - ui.effect_step;
			for (size_t row = 0; row < 5; row++) {
				frame[row * 5 + col] = 100;
				int trail = col - ui.sweep_direction;
				if (trail >= 0 && trail < 5) frame[row * 5 + trail] = 55;
			}
		}
		return;
	}
	if (ui.effect == EFFECT_LAUNCH) {
		static const uint8_t brightness[] = {100, 70, 35, 0};
		memset(frame, brightness[ui.effect_step], BP_MATRIX_PIXELS);
		return;
	}
	if (now < ui.error_until) {
		glyph('X', 100, frame);
		return;
	}
	if (ui.pybricks) {
#if CONFIG_BRICKPHYR_PYBRICKS_SLOTS > 0
		glyph('0' + ui.slot, 100, frame);
#else
		glyph('!', 100, frame);
#endif
		return;
	}
	if (!ui.name[0]) {
		glyph(ui.browse_error == -ENOENT ? '-' : '!', 100, frame);
		return;
	}
	if (ui.preview == PREVIEW_GAP) {
		memset(frame, 0, BP_MATRIX_PIXELS);
		return;
	}
	uint8_t brightness = 100;
	if (ui.preview == PREVIEW_FADE) {
		brightness = 100 * (FADE_STEPS + 1 - ui.fade_step) / (FADE_STEPS + 1);
	}
	glyph(ui.name[ui.offset], brightness, frame);
}

static void display(int64_t now)
{
	if (ui.error_until && now >= ui.error_until) {
		ui.error_until = 0;
		select_name(ui.name, ui.browse_error, now);
	}
	if (ui.effect != EFFECT_NONE && !ui.dirty && now >= ui.frame_at) {
		if (ui.effect == EFFECT_SWEEP && ui.effect_step < 5) {
			ui.effect_step++;
			ui.dirty = true;
		} else if (ui.effect == EFFECT_LAUNCH && ui.effect_step < 3) {
			ui.effect_step++;
			ui.dirty = true;
		} else {
			ui.launch_ready = ui.effect == EFFECT_LAUNCH;
			ui.effect = EFFECT_NONE;
		}
	}
	if (ui.effect == EFFECT_NONE && !ui.waiting_selection && !ui.pybricks &&
	    !ui.dirty && ui.name[1] && ui.preview != PREVIEW_DONE) {
		if (now >= ui.frame_at) {
			switch (ui.preview) {
			case PREVIEW_HOLD:
				ui.preview = PREVIEW_FADE;
				ui.fade_step = 1;
				break;
			case PREVIEW_FADE:
				if (ui.fade_step < FADE_STEPS) ui.fade_step++;
				else ui.preview = PREVIEW_GAP;
				break;
			case PREVIEW_GAP:
				ui.offset++;
				if (!ui.name[ui.offset]) {
					ui.offset = 0;
					ui.preview = PREVIEW_DONE;
				} else {
					ui.preview = PREVIEW_HOLD;
				}
				break;
			default:
				break;
			}
			ui.dirty = true;
		}
	}
	if (!ui.dirty || (ui.launching && ui.effect != EFFECT_LAUNCH)) return;
	uint8_t frame[BP_MATRIX_PIXELS];
	preview_frame(frame, now);
	int err = hub_matrix_write(BP_MATRIX_SYSTEM, 0, frame, sizeof(frame));
	if (!err) {
		ui.dirty = false;
		if (ui.effect == EFFECT_SWEEP) {
			ui.frame_at = now + SWEEP_FRAME_MS;
		} else if (ui.effect == EFFECT_LAUNCH) {
			ui.frame_at = now + (ui.effect_step ? LAUNCH_FADE_MS : LAUNCH_HOLD_MS);
		} else if (ui.preview == PREVIEW_HOLD) {
			ui.frame_at = now + (ui.offset ? LETTER_HOLD_MS : FIRST_HOLD_MS);
		} else if (ui.preview == PREVIEW_FADE) {
			ui.frame_at = now + FADE_FRAME_MS;
		} else if (ui.preview == PREVIEW_GAP) {
			ui.frame_at = now + GAP_MS;
		}
	} else if (err != -EBUSY && err != -ESHUTDOWN) {
		LOG_ERR("Launcher display failed: %d", err);
		ui.dirty = false;
		if (ui.effect == EFFECT_LAUNCH) {
			ui.launching = false;
			ui.error_until = now + 700;
		}
		ui.effect = EFFECT_NONE;
	}
}

void hub_launcher_poll(bool available, bool pybricks, unsigned int slot, int64_t now)
{
	if (finish_job(now)) return;
	if (!available) {
		if (ui.visible) {
			ui.visible = false;
			ui.generation++;
			ui.queued_direction = 0;
			ui.start_pending = false;
			ui.waiting_selection = false;
			ui.launching = false;
			ui.launch_ready = false;
			ui.effect = EFFECT_NONE;
			int err = hub_matrix_system_release();
			if (err) LOG_ERR("Launcher display release failed: %d", err);
		}
		return;
	}
	if (!ui.visible || ui.pybricks != pybricks) {
		ui.visible = true;
		ui.pybricks = pybricks;
		ui.generation++;
		ui.queued_direction = 0;
		ui.start_pending = false;
		ui.waiting_selection = false;
		ui.launching = false;
		ui.launch_ready = false;
		ui.effect = EFFECT_NONE;
		ui.error_until = 0;
		ui.slot = slot;
		select_name(ui.name, ui.name[0] ? 0 : -ENOENT, now);
		ui.need_initial_browse = !pybricks;
	}
	if (pybricks) {
#if CONFIG_BRICKPHYR_PYBRICKS_SLOTS > 0
		if (slot != ui.slot) {
			ui.slot = slot;
			ui.error_until = 0;
			ui.dirty = true;
		}
#endif
	} else if (!ui.launching) {
		if (ui.queued_direction && ui.effect == EFFECT_NONE && job_available()) {
			int direction = ui.queued_direction < 0 ? -1 : 1;
			if (!submit(JOB_BROWSE, ui.name, direction)) {
				ui.queued_direction -= direction;
				ui.need_initial_browse = false;
			}
		} else if (ui.need_initial_browse && ui.effect == EFFECT_NONE &&
			   job_available()) {
			if (!submit(JOB_BROWSE, ui.name, 0)) ui.need_initial_browse = false;
		}
	}
	if (ui.waiting_selection && ui.effect == EFFECT_NONE &&
	    !ui.queued_direction && job_available()) {
		ui.waiting_selection = false;
		select_name(ui.name, ui.browse_error, now);
	}
	if (ui.start_pending && !ui.waiting_selection && !ui.launching &&
	    job_available()) {
		ui.start_pending = false;
		hub_launcher_start(now);
	}
	if (ui.launch_ready && job_available()) {
		char name[APP_NAME_MAX + 1];
		if (ui.pybricks) snprintk(name, sizeof(name), "@%u", ui.launch_slot);
		else strcpy(name, ui.name);
		int err = submit(JOB_START, name, 0);
		if (!err) {
			ui.launch_ready = false;
		} else if (err != -EBUSY) {
			LOG_ERR("Program start queue failed: %d", err);
			ui.launch_ready = false;
			ui.launching = false;
			ui.error_until = now + 700;
			ui.dirty = true;
		}
	}
	display(now);
}

void hub_launcher_navigate(int direction)
{
	if (!ui.visible || ui.pybricks || ui.launching) return;
	ui.queued_direction = CLAMP(ui.queued_direction + direction, -8, 8);
	ui.start_pending = false;
	ui.waiting_selection = true;
	ui.sweep_direction = direction;
	ui.effect = EFFECT_SWEEP;
	ui.effect_step = 0;
	ui.preview = PREVIEW_DONE;
	ui.offset = 0;
	ui.dirty = true;
}

void hub_launcher_start(int64_t now)
{
	if (!ui.visible || ui.launching) return;
	if (ui.need_initial_browse || ui.waiting_selection || !job_available()) {
		ui.start_pending = true;
		return;
	}
	if (!ui.pybricks && !ui.name[0]) {
		ui.error_until = now + 700;
		ui.dirty = true;
		return;
	}
	if (ui.pybricks) {
#if CONFIG_BRICKPHYR_PYBRICKS_SLOTS > 0
		ui.launch_slot = ui.slot;
#else
		ui.error_until = now + 700;
		ui.dirty = true;
		return;
#endif
	}
	ui.effect = EFFECT_LAUNCH;
	ui.effect_step = 0;
	ui.dirty = true;
	ui.launching = true;
}
