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
	uint8_t slot;
	uint32_t generation;
	char name[APP_NAME_MAX + 1];
	int browse_error;
	int8_t queued_direction;
	int64_t frame_at, error_until;
	uint8_t offset;
	enum { PREVIEW_FIRST, PREVIEW_SCROLL, PREVIEW_DONE } preview;
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
	ui.frame_at = now + 700;
	ui.offset = 0;
	ui.preview = PREVIEW_FIRST;
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
		select_name(result ? "" : response, result, now);
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

static void glyph(char character, uint8_t frame[BP_MATRIX_PIXELS])
{
	if (character < 32 || character > 126) character = '?';
	for (size_t row = 0; row < 5; row++) {
		for (size_t col = 0; col < 5; col++) {
			frame[row * 5 + col] =
				pb_font_5x5[character - 32][row] & BIT(4 - col) ? 100 : 0;
		}
	}
}

static void preview_frame(uint8_t frame[BP_MATRIX_PIXELS], int64_t now)
{
	if (now < ui.error_until) {
		glyph('X', frame);
		return;
	}
	if (!ui.name[0]) {
		glyph(ui.browse_error == -ENOENT ? '-' : '!', frame);
		return;
	}
	if (!ui.offset) {
		glyph(ui.name[0], frame);
		return;
	}
	size_t length = strlen(ui.name);
	for (size_t row = 0; row < 5; row++) {
		for (size_t col = 0; col < 5; col++) {
			size_t column = ui.offset + col;
			size_t character = column / 6;
			size_t glyph_column = column % 6;
			frame[row * 5 + col] = character < length && glyph_column < 5 &&
				(pb_font_5x5[ui.name[character] - 32][row] &
				 BIT(4 - glyph_column)) ? 100 : 0;
		}
	}
}

static void display(int64_t now)
{
	if (ui.error_until && now >= ui.error_until) {
		ui.error_until = 0;
		select_name(ui.name, ui.browse_error, now);
	}
	if (!ui.dirty && ui.name[1] && now >= ui.frame_at &&
	    ui.preview != PREVIEW_DONE) {
		if (ui.preview == PREVIEW_FIRST) {
			ui.preview = PREVIEW_SCROLL;
			ui.offset = 1;
		} else if (ui.offset < strlen(ui.name) * 6) {
			ui.offset++;
		} else {
			ui.preview = PREVIEW_DONE;
			ui.offset = 0;
		}
		ui.dirty = true;
	}
	if (!ui.dirty || ui.launching) return;
	uint8_t frame[BP_MATRIX_PIXELS];
	preview_frame(frame, now);
	int err = hub_matrix_write(BP_MATRIX_SYSTEM, 0, frame, sizeof(frame));
	if (!err) {
		ui.dirty = false;
		if (ui.preview == PREVIEW_SCROLL) ui.frame_at = now + 90;
	} else if (err != -EBUSY && err != -ESHUTDOWN) {
		LOG_ERR("Launcher display failed: %d", err);
		ui.dirty = false;
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
		ui.error_until = 0;
		ui.slot = slot;
		select_name("", -ENOENT, now);
		ui.need_initial_browse = !pybricks;
		if (pybricks) {
#if CONFIG_BRICKPHYR_PYBRICKS_SLOTS > 0
			char digit[] = {(char)('0' + slot), 0};
			select_name(digit, 0, now);
#else
			select_name("", -ENOTSUP, now);
#endif
		}
	}
	if (pybricks) {
#if CONFIG_BRICKPHYR_PYBRICKS_SLOTS > 0
		if (slot != ui.slot) {
			ui.slot = slot;
			char digit[] = {(char)('0' + slot), 0};
			select_name(digit, 0, now);
		}
#endif
	} else if (!ui.launching) {
		if (ui.queued_direction && job_available()) {
			int direction = ui.queued_direction < 0 ? -1 : 1;
			if (!submit(JOB_BROWSE, ui.name, direction)) {
				ui.queued_direction -= direction;
				ui.need_initial_browse = false;
			}
		} else if (ui.need_initial_browse && job_available()) {
			if (!submit(JOB_BROWSE, "", 0)) ui.need_initial_browse = false;
		}
	}
	if (ui.start_pending && job_available()) {
		ui.start_pending = false;
		hub_launcher_start(now);
	}
	display(now);
}

void hub_launcher_navigate(int direction)
{
	if (!ui.visible || ui.pybricks || ui.launching) return;
	ui.queued_direction = CLAMP(ui.queued_direction + direction, -8, 8);
	ui.preview = PREVIEW_DONE;
	ui.offset = 0;
	ui.dirty = true;
}

void hub_launcher_start(int64_t now)
{
	if (!ui.visible || ui.launching) return;
	if (!ui.name[0]) {
		ui.error_until = now + 700;
		ui.dirty = true;
		return;
	}
	char name[APP_NAME_MAX + 1];
	if (ui.pybricks) {
#if CONFIG_BRICKPHYR_PYBRICKS_SLOTS > 0
		snprintk(name, sizeof(name), "@%u", ui.slot);
#else
		ui.error_until = now + 700;
		ui.dirty = true;
		return;
#endif
	} else {
		strcpy(name, ui.name);
	}
	int err = submit(JOB_START, name, 0);
	if (err == -EBUSY) {
		ui.start_pending = true;
		return;
	}
	if (err) {
		LOG_ERR("Program start queue failed: %d", err);
		ui.error_until = now + 700;
		ui.dirty = true;
		return;
	}
	ui.launching = true;
}
