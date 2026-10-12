// SPDX-License-Identifier: MIT
// Copyright (c) 2018-2023 The Pybricks Authors
// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2020-2023 LEGO System A/S
/* Computation port of lib/pbio/src/drivebase.c at a7b1b04ae. PBIO still
 * supplies both controllers and trajectories. Explicit frame inputs replace
 * its servo, IMU, clock and scheduler dependencies; ownership stays outside. */
#include "drivebase.h"
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <pbio/geometry.h>
#include <pbio/int_math.h>

static void drivebase_adopt_settings(pbio_control_settings_t *s_distance, pbio_control_settings_t *s_heading, const pbio_control_settings_t *s_left, const pbio_control_settings_t *s_right) {

    // Use minimum PID of both motors, to avoid overly aggressive control if
    // one of the two motors has much higher PID values. Then scale it such
    // that we use the reduced kp value not just for low errors, but always.
    int32_t pid_kp = (int64_t)pbio_int_math_min(s_left->pid_kp, s_right->pid_kp) *
        pbio_int_math_min(s_left->pid_kp_low_pct, s_right->pid_kp_low_pct) / 100;

    // Cap maximum speed at the most constrained of the two motors.
    int32_t actuation_max = pbio_int_math_min(s_left->actuation_max, s_right->actuation_max);
    int32_t speed_max = pbio_int_math_min(s_left->speed_max, s_right->speed_max);

    // For all settings, take the value of the least powerful motor to ensure
    // that the drivebase can meet the given specs.
    *s_distance = (pbio_control_settings_t) {
        // Must be set immediately after calling the current function.
        .ctl_steps_per_app_step = 0,
        .stall_speed_limit = pbio_int_math_min(s_left->stall_speed_limit, s_right->stall_speed_limit),
        .stall_time = pbio_int_math_min(s_left->stall_time, s_right->stall_time),
        .speed_max = speed_max,
        // The default speed is 40% of the maximum speed.
        .speed_default = speed_max * 10 / 25,
        .speed_tolerance = pbio_int_math_min(s_left->speed_tolerance, s_right->speed_tolerance),
        // To account for reduced kp, adjust the position tolerance so we
        // always apply enough proportional torque to keep moving near the end.
        .position_tolerance = pbio_control_settings_div_by_gain(actuation_max, pid_kp),
        // Make acceleration, deceleration a bit slower for smoother driving.
        .acceleration = pbio_int_math_min(s_left->acceleration, s_right->acceleration) * 3 / 4,
        .deceleration = pbio_int_math_min(s_left->deceleration, s_right->deceleration) * 3 / 4,
        .actuation_max = actuation_max,
        .pid_kp = pid_kp,
        // Dynamic kp reduction is disabled for drivebases. Instead, it uses
        // reduced kp across the board.
        .pid_kp_low_pct = 0,
        .pid_kp_low_error_threshold = 0,
        .pid_kp_low_speed_threshold = 0,
        // Integral control is not necessary since there is no constant external
        // force to overcome that wouldn't be done by proportional control.
        .pid_ki = 0,
        .pid_kd = pbio_int_math_min(s_left->pid_kd, s_right->pid_kd),
        .integral_deadzone = pbio_int_math_max(s_left->integral_deadzone, s_right->integral_deadzone),
        .integral_change_max = pbio_int_math_min(s_left->integral_change_max, s_right->integral_change_max),
        .smart_passive_hold_time = pbio_int_math_max(s_left->smart_passive_hold_time, s_right->smart_passive_hold_time),
    };

    // By default, heading control is the nearly same as distance control.
    *s_heading = *s_distance;

    // We make the default turn speed a bit slower. Given the typical wheel
    // diameter, the wheels are often quite close together, so this
    // compensates by setting it at 33% instead of 40%.
    s_heading->speed_default = s_heading->speed_max / 3;

    // Most users intuitively expect heading control to take priority. When
    // heading controller is completely saturated, this ensures that it "wins"
    // against the distance controller.
    s_heading->actuation_max = s_distance->actuation_max * 2;
}

int drivebase_control_scales(int32_t gear_scale, int32_t wheel_um, int32_t track_um,
    bool spike, int32_t scales[DRIVEBASE_AXES])
{
    if (gear_scale < 1 || wheel_um < 1000 || track_um < 1000) return -EINVAL;
    int64_t distance = spike ? 1000 : (int64_t)gear_scale * 114592 / wheel_um;
    int64_t heading = spike ? 1000 : (int64_t)gear_scale * track_um / wheel_um;
    if (distance < 1 || heading < 1 || distance > 360000 || heading > 360000) return -EINVAL;
    scales[0] = distance;
    scales[1] = heading;
    return 0;
}

int drivebase_control_init(struct drivebase_control *db,
    const pbio_control_settings_t *left, const pbio_control_settings_t *right,
    int32_t gear_scale, int32_t wheel_um, int32_t track_um, bool spike)
{
    int32_t scales[DRIVEBASE_AXES];
    int err = drivebase_control_scales(gear_scale, wheel_um, track_um, spike, scales);
    if (err) return err;
    *db = (struct drivebase_control){0};
    drivebase_adopt_settings(&db->control[0].settings, &db->control[1].settings, left, right);
    db->control[0].settings.ctl_steps_per_app_step = scales[0];
    db->control[1].settings.ctl_steps_per_app_step = scales[1];
    for (size_t i = 0; i < DRIVEBASE_AXES; i++) pbio_control_reset(&db->control[i]);
    return 0;
}

void drivebase_control_state(const struct drivebase_control *db,
    const pbio_control_state_t *left, const pbio_control_state_t *right,
    struct drivebase_state *state)
{
    pbio_control_state_t *distance = &state->axis[0], *heading = &state->axis[1];
    pbio_angle_avg(&left->position, &right->position, &distance->position);
    pbio_angle_avg(&left->position_estimate, &right->position_estimate, &distance->position_estimate);
    distance->speed = ((int64_t)left->speed + right->speed) / 2;
    distance->speed_estimate = ((int64_t)left->speed_estimate + right->speed_estimate) / 2;
    pbio_angle_diff(&distance->position, &right->position, &heading->position);
    pbio_angle_diff(&distance->position_estimate, &right->position_estimate, &heading->position_estimate);
    heading->speed = ((int64_t)left->speed - right->speed) / 2;
    heading->speed_estimate = ((int64_t)left->speed_estimate - right->speed_estimate) / 2;
    for (size_t i = 0; i < DRIVEBASE_AXES; i++) {
        pbio_angle_diff(&state->axis[i].position, &db->offset[i], &state->axis[i].position);
        pbio_angle_diff(&state->axis[i].position_estimate, &db->offset[i], &state->axis[i].position_estimate);
    }
}

void drivebase_control_stop(struct drivebase_control *db)
{
    for (size_t i = 0; i < DRIVEBASE_AXES; i++) pbio_control_stop(&db->control[i]);
    db->paused = false;
    db->secondary_pending = false;
}

static int reset_offsets(const struct drivebase_control *db,
    const struct drivebase_state *state, int32_t distance, int32_t angle,
    pbio_angle_t offset[DRIVEBASE_AXES])
{
    int32_t values[] = {distance, angle};
    for (size_t i = 0; i < DRIVEBASE_AXES; i++) {
        int64_t raw = (int64_t)state->axis[i].position.rotations * 360000 + state->axis[i].position.millidegrees +
            (int64_t)db->offset[i].rotations * 360000 + db->offset[i].millidegrees;
        int64_t target_value = (int64_t)values[i] * db->control[i].settings.ctl_steps_per_app_step;
        const int64_t limit = (int64_t)(INT32_MAX / 4) * 360000;
        if (llabs(raw) > limit || llabs(target_value) > limit || llabs(raw - target_value) > limit) return -ERANGE;
        pbio_angle_t target, measured;
        pbio_angle_sum(&state->axis[i].position, &db->offset[i], &measured);
        pbio_angle_from_low_res(&target, values[i], db->control[i].settings.ctl_steps_per_app_step);
        pbio_angle_diff(&measured, &target, &offset[i]);
    }
    return 0;
}

int drivebase_control_reset_check(const struct drivebase_control *db,
    const struct drivebase_state *state, int32_t distance, int32_t angle)
{
    pbio_angle_t offset[DRIVEBASE_AXES];
    return reset_offsets(db, state, distance, angle, offset);
}

int drivebase_control_reset(struct drivebase_control *db,
    const struct drivebase_state *state, int32_t distance, int32_t angle)
{
    pbio_angle_t offset[DRIVEBASE_AXES];
    int err = reset_offsets(db, state, distance, angle, offset);
    if (err) return err;
    drivebase_control_stop(db);
    for (size_t i = 0; i < DRIVEBASE_AXES; i++) {
        db->offset[i] = offset[i];
        /* A smart relative move must not reuse an endpoint expressed in the
         * coordinates which reset has just replaced. */
        pbio_control_reset(&db->control[i]);
    }
    return 0;
}

bool drivebase_control_active(const struct drivebase_control *db)
{
    return pbio_control_is_active(&db->control[0]) && pbio_control_is_active(&db->control[1]);
}

bool drivebase_control_done(const struct drivebase_control *db)
{
    return !db->secondary_pending && pbio_control_is_done(&db->control[0]) && pbio_control_is_done(&db->control[1]);
}

static int start(struct drivebase_control *db, uint32_t ticks,
    const struct drivebase_state *state, const struct drivebase_command *command)
{
    int32_t positions[] = {command->distance, command->angle};
    int32_t speeds[] = {command->speed, command->turn_rate};
    for (size_t i = 0; i < DRIVEBASE_AXES; i++) {
        pbio_control_t *ctl = &db->control[i];
        pbio_error_t err;
        if (command->motion == DRIVEBASE_TIMED) {
            err = pbio_control_start_timed_control(ctl, ticks, &state->axis[i],
                command->duration_ms, speeds[i], command->finish);
        } else if (command->motion == DRIVEBASE_ABSOLUTE_TURN && i == DRIVEBASE_HEADING) {
            /* Match the nearest whole rotation, preserving the reference's
             * integer-target rounding. Bounded modulo replaces a potentially
             * millions-of-iterations loop for large accumulated headings. */
            int32_t current = pbio_control_settings_ctl_to_app_long(&ctl->settings, &state->axis[i].position);
            int64_t delta = ((int64_t)positions[i] - current) % 360;
            if (delta > 180) delta -= 360;
            if (delta < -180) delta += 360;
            int64_t target = (int64_t)current + delta;
            if (target < INT32_MIN || target > INT32_MAX) return -ERANGE;
            err = pbio_control_start_position_control(ctl, ticks, &state->axis[i], target, 0, command->finish);
        } else {
            err = pbio_control_start_position_control_relative(ctl, ticks, &state->axis[i],
                positions[i], speeds[i], command->finish, false);
        }
        if (err) return err == PBIO_ERROR_INVALID_ARG ? -ERANGE : -EDOM;
    }
    if (command->motion != DRIVEBASE_TIMED) {
        /* Reuse PBIO's common-duration trajectory; never run two independent
         * motor commands and hope their completion times agree. */
        pbio_trajectory_t *a = &db->control[0].trajectory, *b = &db->control[1].trajectory;
        if (pbio_trajectory_get_duration(a) > pbio_trajectory_get_duration(b)) pbio_trajectory_stretch(b, a);
        else pbio_trajectory_stretch(a, b);
    }
    return 0;
}

int drivebase_control_command(struct drivebase_control *db, uint32_t ticks,
    const struct drivebase_state *state, const struct drivebase_command *command)
{
    /* move_by's geometry and two-phase order come from pb_type_drivebase.c.
     * Keep the sequence here so native and Python callers share cancellation,
     * and its straight phase does not depend on Python being scheduled. */
    bool vector = command->motion == DRIVEBASE_VECTOR;
    struct drivebase_command turn;
    int32_t length = 0;
    pbio_control_on_completion_t finish = command->finish;
    if (vector) {
        int32_t dx = command->distance, dy = command->angle;
        if (dx < -INT16_MAX || dx > INT16_MAX || dy < -INT16_MAX || dy > INT16_MAX ||
            command->duration_ms || command->speed || command->turn_rate ||
            finish < PBIO_CONTROL_ON_COMPLETION_COAST || finish > PBIO_CONTROL_ON_COMPLETION_BRAKE_SMART) return -EINVAL;
        length = (int32_t)sqrtf(dx * dx + dy * dy);
        turn = (struct drivebase_command){.motion = DRIVEBASE_ABSOLUTE_TURN,
            .angle = -(int32_t)pbio_geometry_radians_to_degrees(atan2f(dy, dx)),
            .finish = PBIO_CONTROL_ON_COMPLETION_HOLD};
        command = &turn;
    }
    if (command->motion < DRIVEBASE_RELATIVE || command->motion > DRIVEBASE_TIMED ||
        command->finish < PBIO_CONTROL_ON_COMPLETION_COAST || command->finish > PBIO_CONTROL_ON_COMPLETION_BRAKE_SMART ||
        (command->motion == DRIVEBASE_ABSOLUTE_TURN && (command->distance || command->speed || command->turn_rate)) ||
        (command->motion == DRIVEBASE_TIMED && (command->distance || command->angle ||
            command->duration_ms > PBIO_TRAJECTORY_DURATION_FOREVER_MS ||
            (command->duration_ms == PBIO_TRAJECTORY_DURATION_FOREVER_MS && command->finish != PBIO_CONTROL_ON_COMPLETION_CONTINUE)))) return -EINVAL;
    int32_t speeds[] = {command->speed, command->turn_rate};
    int32_t positions[] = {command->distance, command->angle};
    for (size_t i = 0; i < DRIVEBASE_AXES; i++) {
        int32_t scale = db->control[i].settings.ctl_steps_per_app_step;
        if (scale < 1 || positions[i] == INT32_MIN ||
            llabs((int64_t)speeds[i] * scale) > INT32_MAX) return -ERANGE;
        int64_t position = (int64_t)state->axis[i].position.rotations * 360000 + state->axis[i].position.millidegrees;
        /* PBIO absolute-turn and timed-HOLD paths narrow to application units.
         * Reserve a rounding unit at each endpoint. */
        if ((command->motion == DRIVEBASE_ABSOLUTE_TURN ||
             (command->motion == DRIVEBASE_TIMED && command->finish == PBIO_CONTROL_ON_COMPLETION_HOLD)) &&
            (position / scale <= INT32_MIN || position / scale >= INT32_MAX)) return -ERANGE;
    }
    /* Each PBIO start is transactional, but the pair must be too. Restoring
     * this snapshot retains its original self-referential integrator pointers.
     * No external buffers, hardware or state are modified by these starts. */
    struct drivebase_control previous = *db;
    int err = start(db, ticks, state, command);
    if (err) *db = previous;
    else {
        db->secondary_pending = vector;
        db->secondary_distance = length;
        db->secondary_finish = finish;
    }
    return err;
}

int drivebase_control_step(struct drivebase_control *db, uint32_t ticks,
    const struct drivebase_state *state, struct drivebase_reference *value, struct motor_pid_sample *logs[DRIVEBASE_AXES])
{
    if (db->secondary_pending && pbio_control_is_done(&db->control[0]) && pbio_control_is_done(&db->control[1])) {
        struct drivebase_command straight = {.motion = DRIVEBASE_RELATIVE,
            .distance = db->secondary_distance, .finish = db->secondary_finish};
        int err = drivebase_control_command(db, ticks, state, &straight);
        if (err) return err;
    }
    if (!drivebase_control_active(db)) return -EAGAIN;
    struct drivebase_reference out = {0};
    /* Reject both inputs before advancing either PID. The owner coasts the
     * entire pair on error, including a failure applying the second output. */
    for (size_t i = 0; i < DRIVEBASE_AXES; i++) {
        const pbio_control_t *ctl = &db->control[i];
        if (pbio_control_type_is_time(ctl) && ctl->on_completion == PBIO_CONTROL_ON_COMPLETION_HOLD) {
            int64_t position = ((int64_t)state->axis[i].position.rotations * 360000 +
                state->axis[i].position.millidegrees) / ctl->settings.ctl_steps_per_app_step;
            if (position <= INT32_MIN || position >= INT32_MAX) return -ERANGE;
        }
        int err = motor_control_reference(&db->control[i], ticks, &state->axis[i], &out.axis[i]);
        if (err) return err;
    }
    bool paused = false;
    pbio_dcmotor_actuation_t actuation[DRIVEBASE_AXES];
    for (size_t i = 0; i < DRIVEBASE_AXES; i++) {
        bool external_pause = db->paused;
        controller_log_begin(&db->control[i], &state->axis[i], &out.axis[i], logs ? logs[i] : NULL);
        pbio_control_update(&db->control[i], ticks, &state->axis[i], &out.axis[i],
            &actuation[i], &out.torque[i], &external_pause);
        controller_log_end(&db->control[i]);
        paused |= external_pause;
        out.stalled |= (db->control[i].status & PBIO_CONTROL_STATUS_STALLED) != 0;
    }
    db->paused = paused;
    if (actuation[0] == PBIO_DCMOTOR_ACTUATION_COAST || actuation[1] == PBIO_DCMOTOR_ACTUATION_COAST) {
        out.actuation = PBIO_DCMOTOR_ACTUATION_COAST;
        drivebase_control_stop(db);
    } else if (actuation[0] == PBIO_DCMOTOR_ACTUATION_BRAKE || actuation[1] == PBIO_DCMOTOR_ACTUATION_BRAKE) {
        out.actuation = PBIO_DCMOTOR_ACTUATION_BRAKE;
        drivebase_control_stop(db);
    } else if (actuation[0] == PBIO_DCMOTOR_ACTUATION_TORQUE && actuation[1] == PBIO_DCMOTOR_ACTUATION_TORQUE) {
        out.actuation = PBIO_DCMOTOR_ACTUATION_TORQUE;
    } else {
        return -EDOM;
    }
    out.active = drivebase_control_active(db);
    out.done = drivebase_control_done(db);
    *value = out;
    return 0;
}

void drivebase_control_output(const struct drivebase_reference *value,
    const struct motor_control *motor, bool right, struct motor_reference *out)
{
    int direction = right ? -1 : 1;
    int32_t speed = value->axis[0].speed + direction * value->axis[1].speed;
    int32_t acceleration = value->axis[0].acceleration + direction * value->axis[1].acceleration;
    int32_t feedback = value->torque[0] + direction * value->torque[1];
    int32_t feedforward = pbio_observer_get_feedforward_torque(motor->observer.model, speed, acceleration);
    /* pbio_servo_actuate limits drivebase torque at the physical motor maximum,
     * not at a previous standalone command's temporary duty/stall limit. */
    int32_t torque = pbio_int_math_clamp(feedback + feedforward, pbio_observer_get_max_torque());
    int32_t voltage = value->actuation == PBIO_DCMOTOR_ACTUATION_TORQUE ?
        pbio_observer_torque_to_voltage(motor->observer.model, torque) : 0;
    *out = (struct motor_reference){.speed_mdeg_s = speed, .acceleration_mdeg_s2 = acceleration,
        .feedback_torque = feedback, .feedforward_torque = feedforward,
        .voltage_mv = pbio_int_math_clamp(voltage, motor->max_voltage),
        .coast = value->actuation == PBIO_DCMOTOR_ACTUATION_COAST,
        .active = value->active, .done = value->done, .stalled = value->stalled};
}

void drivebase_settings_get(const struct drivebase_control *db, struct bp_drivebase_settings *value)
{
    const pbio_control_settings_t *d = &db->control[0].settings, *h = &db->control[1].settings;
    *value = (struct bp_drivebase_settings){
        .speed = pbio_control_settings_ctl_to_app(d, d->speed_default),
        .acceleration = pbio_control_settings_ctl_to_app(d, d->acceleration),
        .deceleration = pbio_control_settings_ctl_to_app(d, d->deceleration),
        .turn_rate = pbio_control_settings_ctl_to_app(h, h->speed_default),
        .turn_acceleration = pbio_control_settings_ctl_to_app(h, h->acceleration),
        .turn_deceleration = pbio_control_settings_ctl_to_app(h, h->deceleration)};
}

int drivebase_settings_set(struct drivebase_control *db, const struct bp_drivebase_settings *value)
{
    int32_t speeds[] = {value->speed, value->turn_rate};
    int32_t acceleration[] = {value->acceleration, value->turn_acceleration};
    int32_t deceleration[] = {value->deceleration, value->turn_deceleration};
    for (size_t i = 0; i < DRIVEBASE_AXES; i++) {
        int32_t scale = db->control[i].settings.ctl_steps_per_app_step;
        if (speeds[i] < 1 || acceleration[i] < 1 || deceleration[i] < 1 ||
            pbio_trajectory_validate_speed_limit(scale, speeds[i]) ||
            pbio_trajectory_validate_acceleration_limit(scale, acceleration[i]) ||
            pbio_trajectory_validate_acceleration_limit(scale, deceleration[i])) return -EINVAL;
    }
    for (size_t i = 0; i < DRIVEBASE_AXES; i++) {
        pbio_control_settings_t *s = &db->control[i].settings;
        s->speed_default = pbio_int_math_clamp(pbio_control_settings_app_to_ctl(s, speeds[i]), s->speed_max);
        s->acceleration = pbio_control_settings_app_to_ctl(s, acceleration[i]);
        s->deceleration = pbio_control_settings_app_to_ctl(s, deceleration[i]);
    }
    return 0;
}

int drivebase_arc_command(int32_t radius, int32_t amount, bool distance,
    bool legacy_curve, struct drivebase_command *command)
{
    int64_t r = radius, a = amount, length, angle;
    if (!legacy_curve && llabs(r) < 10) return -EINVAL;
    /* The output must fit int32; reject before multiplying the product by 10. */
    if ((!distance || legacy_curve) && llabs(a * r) > ((int64_t)INT32_MAX + 1) * 573 / 10) return -ERANGE;
    if (legacy_curve) {
        angle = r < 0 ? -a : a;
        length = 10 * llabs(a) * r / 573;
    } else if (distance) {
        length = a;
        angle = llabs(a) * 573 / llabs(r) / 10;
        if ((r < 0) != (a < 0)) angle = -angle;
    } else {
        length = 10 * a * llabs(r) / 573;
        angle = (r > 0) == (a > 0) ? llabs(a) : -llabs(a);
    }
    if (length < -INT32_MAX || length > INT32_MAX || angle < -INT32_MAX || angle > INT32_MAX) return -ERANGE;
    command->motion = DRIVEBASE_RELATIVE;
    command->distance = length;
    command->angle = angle;
    command->speed = command->turn_rate = 0;
    return 0;
}

int drivebase_tank_command(int32_t left, int32_t right, int32_t angle,
    bool timed, struct drivebase_command *command)
{
    if (left == INT32_MIN || right == INT32_MIN || (!timed && angle == INT32_MIN)) return -ERANGE;
    int64_t l = -(int64_t)left, r = right;
    command->motion = timed ? DRIVEBASE_TIMED : DRIVEBASE_RELATIVE;
    command->distance = command->angle = 0;
    if (timed) {
        command->speed = (l + r) / 2;
        command->turn_rate = (l - r) / 2;
    } else {
        int64_t max_speed = llabs(l) > llabs(r) ? llabs(l) : llabs(r);
        int64_t al = max_speed ? (int64_t)angle * l / max_speed : 0;
        int64_t ar = max_speed ? (int64_t)angle * r / max_speed : 0;
        command->distance = (al + ar) / 2;
        command->angle = (al - ar) / 2;
        command->speed = command->turn_rate = (llabs(l) + llabs(r)) / 2;
    }
    return 0;
}

int drivebase_steering_to_tank(int32_t speed, int32_t steering, int32_t *left, int32_t *right)
{
    if (steering < -100 || steering > 100 || speed == INT32_MIN) return -EINVAL;
    *left = *right = speed;
    *(steering > 0 ? right : left) = (int64_t)speed * (100 - 2 * abs(steering)) / 100;
    return 0;
}
