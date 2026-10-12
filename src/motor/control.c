// SPDX-License-Identifier: MIT
// Copyright (c) 2018-2023 The Pybricks Authors
// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2020-2023 LEGO System A/S
/* Selected PUP models and default settings from PBIO motor/servo_settings.c
 * and servo.c at a7b1b04ae. The computation adapter uses public PBIO control,
 * trajectory and observer APIs; device ownership and scheduling stay outside.
 * See README.md for the boundary and numerical assumptions. */
#include "control.h"
#include "voltage.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <lego/device.h>
#include <pbio/control_settings.h>
#include <pbio/int_math.h>

_Static_assert(PBIO_CONFIG_CONTROL_LOOP_TIME_MS == 5, "PUP models use a 5ms step");
#define ACCELERATION_MDEG_S2 2000000

struct defaults {
    uint16_t id;
    const pbio_observer_model_t *model;
    int32_t rated_max_speed, feedback_gain_low, precision_profile;
    int32_t pid_kp_low_speed_threshold;
};

static const pbio_observer_model_t model_technic_s_angular = {
    .d_angle_d_speed = 179217,
    .d_speed_d_speed = 956,
    .d_current_d_speed = -249247,
    .d_angle_d_current = 1950303,
    .d_speed_d_current = 7666,
    .d_current_d_current = -9356019,
    .d_angle_d_voltage = 5654927,
    .d_speed_d_voltage = 11702,
    .d_current_d_voltage = 349105,
    .d_angle_d_torque = -425928,
    .d_speed_d_torque = -1085,
    .d_current_d_torque = 383927,
    .d_voltage_d_torque = 22334,
    .d_torque_d_voltage = 17203,
    .d_torque_d_speed = 12282,
    .d_torque_d_acceleration = 35129,
    .torque_friction = 9182,
};

static const pbio_observer_model_t model_technic_m_angular = {
    .d_angle_d_speed = 177194,
    .d_speed_d_speed = 934,
    .d_current_d_speed = -165023,
    .d_angle_d_current = 2407354,
    .d_speed_d_current = 8311,
    .d_current_d_current = 1058029,
    .d_angle_d_voltage = 7431528,
    .d_speed_d_voltage = 14444,
    .d_current_d_voltage = 225610,
    .d_angle_d_torque = -919183,
    .d_speed_d_torque = -2332,
    .d_current_d_torque = 629020,
    .d_voltage_d_torque = 47606,
    .d_torque_d_voltage = 8071,
    .d_torque_d_speed = 5903,
    .d_torque_d_acceleration = 16163,
    .torque_friction = 21413,
};

static const pbio_observer_model_t model_technic_l_angular = {
    .d_angle_d_speed = 174943,
    .d_speed_d_speed = 904,
    .d_current_d_speed = -58045,
    .d_angle_d_current = 8368268,
    .d_speed_d_current = 26508,
    .d_current_d_current = 396164,
    .d_angle_d_voltage = 13442903,
    .d_speed_d_voltage = 25105,
    .d_current_d_voltage = 86900,
    .d_angle_d_torque = -3690545,
    .d_speed_d_torque = -9310,
    .d_current_d_torque = 975141,
    .d_voltage_d_torque = 133763,
    .d_torque_d_voltage = 2872,
    .d_torque_d_speed = 1919,
    .d_torque_d_acceleration = 3997,
    .torque_friction = 23239,
};

static const pbio_observer_model_t model_interactive = {
    .d_angle_d_speed = 179110,
    .d_speed_d_speed = 941,
    .d_current_d_speed = -316164,
    .d_angle_d_current = 7311289,
    .d_speed_d_current = 35750,
    .d_current_d_current = -12014584,
    .d_angle_d_voltage = 4603893,
    .d_speed_d_voltage = 10967,
    .d_current_d_voltage = 355664,
    .d_angle_d_torque = -728461,
    .d_speed_d_torque = -1850,
    .d_current_d_torque = 668004,
    .d_voltage_d_torque = 32225,
    .d_torque_d_voltage = 11923,
    .d_torque_d_speed = 10599,
    .d_torque_d_acceleration = 20588,
    .torque_friction = 11227,
};

static const pbio_observer_model_t model_technic_l = {
    .d_angle_d_speed = 175977,
    .d_speed_d_speed = 912,
    .d_current_d_speed = -159828,
    .d_angle_d_current = 5728019,
    .d_speed_d_current = 22787,
    .d_current_d_current = -44152415,
    .d_angle_d_voltage = 6164994,
    .d_speed_d_voltage = 12888,
    .d_current_d_voltage = 142828,
    .d_angle_d_torque = -1377701,
    .d_speed_d_torque = -3482,
    .d_current_d_torque = 794862,
    .d_voltage_d_torque = 62889,
    .d_torque_d_voltage = 6110,
    .d_torque_d_speed = 6837,
    .d_torque_d_acceleration = 10751,
    .torque_friction = 26430,
};

static const pbio_observer_model_t model_technic_xl = {
    .d_angle_d_speed = 176559,
    .d_speed_d_speed = 916,
    .d_current_d_speed = -175173,
    .d_angle_d_current = 8098298,
    .d_speed_d_current = 35736,
    .d_current_d_current = -7606150,
    .d_angle_d_voltage = 5471477,
    .d_speed_d_voltage = 12148,
    .d_current_d_voltage = 156891,
    .d_angle_d_torque = -1282598,
    .d_speed_d_torque = -3244,
    .d_current_d_torque = 729279,
    .d_voltage_d_torque = 55617,
    .d_torque_d_voltage = 6908,
    .d_torque_d_speed = 7713,
    .d_torque_d_acceleration = 11578,
    .torque_friction = 12893,
};

static const struct defaults defaults[] = {
    {
        .id = LEGO_DEVICE_TYPE_ID_INTERACTIVE_MOTOR,
        .model = &model_interactive,
        .rated_max_speed = 1200,
        .feedback_gain_low = 45,
        .precision_profile = 12,
        .pid_kp_low_speed_threshold = 0,
    },
    {
        .id = LEGO_DEVICE_TYPE_ID_TECHNIC_L_MOTOR,
        .model = &model_technic_l,
        .rated_max_speed = 1500,
        .feedback_gain_low = 45,
        .precision_profile = 20,
        .pid_kp_low_speed_threshold = 250,
    },
    {
        .id = LEGO_DEVICE_TYPE_ID_TECHNIC_XL_MOTOR,
        .model = &model_technic_xl,
        .rated_max_speed = 1500,
        .feedback_gain_low = 45,
        .precision_profile = 20,
        .pid_kp_low_speed_threshold = 250,
    },
    {
        .id = LEGO_DEVICE_TYPE_ID_SPIKE_S_MOTOR,
        .model = &model_technic_s_angular,
        .rated_max_speed = 620,
        .feedback_gain_low = 30,
        .precision_profile = 11,
        .pid_kp_low_speed_threshold = 150,
    },
    {
        .id = LEGO_DEVICE_TYPE_ID_TECHNIC_L_ANGULAR_MOTOR,
        .model = &model_technic_l_angular,
        .rated_max_speed = 1000,
        .feedback_gain_low = 45,
        .precision_profile = 11,
        .pid_kp_low_speed_threshold = 150,
    },
    {
        .id = LEGO_DEVICE_TYPE_ID_TECHNIC_M_ANGULAR_MOTOR,
        .model = &model_technic_m_angular,
        .rated_max_speed = 1000,
        .feedback_gain_low = 45,
        .precision_profile = 11,
        .pid_kp_low_speed_threshold = 150,
    },
};

static const struct defaults *find_defaults(uint16_t id)
{
    if (id == LEGO_DEVICE_TYPE_ID_SPIKE_M_MOTOR) id = LEGO_DEVICE_TYPE_ID_TECHNIC_M_ANGULAR_MOTOR;
    if (id == LEGO_DEVICE_TYPE_ID_SPIKE_L_MOTOR) id = LEGO_DEVICE_TYPE_ID_TECHNIC_L_ANGULAR_MOTOR;
    for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
        if (defaults[i].id == id) return &defaults[i];
    }
    return NULL;
}

/* Keep rotation arithmetic away from signed overflow, including PBIO sums.
 * Public acquisition uses int64; conversion must never narrow silently. */
static int angle_from_mdeg(int64_t value, pbio_angle_t *angle)
{
    if (value < -(int64_t)(INT32_MAX / 4) * 360000 ||
        value > (int64_t)(INT32_MAX / 4) * 360000) return -ERANGE;
    *angle = (pbio_angle_t){.rotations = value / 360000, .millidegrees = value % 360000};
    return 0;
}

static int64_t angle_mdeg(const pbio_angle_t *angle)
{
    return (int64_t)angle->rotations * 360000 + angle->millidegrees;
}

int motor_control_check(uint16_t id, int64_t angle, const struct bp_motor_command *command)
{
    const struct defaults *d = find_defaults(id);
    if (!d) return -ENOTSUP;
    if (command->finish > BP_MOTOR_FINISH_BRAKE_SMART ||
        command->speed_mdeg_s < -d->rated_max_speed * 1000 ||
        command->speed_mdeg_s > d->rated_max_speed * 1000) return -EINVAL;
    if (command->action > BP_MOTOR_UNTIL_STALLED ||
        (command->action == BP_MOTOR_UNTIL_STALLED &&
         (command->duty_limit < -1 || command->duty_limit > 1000 ||
          command->finish == BP_MOTOR_FINISH_CONTINUE))) return -EINVAL;
    if (command->action != BP_MOTOR_TRAJECTORY &&
        (command->mode != ((command->action == BP_MOTOR_TIMED || command->action == BP_MOTOR_UNTIL_STALLED) ?
         BP_MOTOR_SPEED : BP_MOTOR_POSITION))) return -EINVAL;
    pbio_angle_t checked;
    int err = angle_from_mdeg(angle, &checked);
    if (err) return err;
    if (command->mode == BP_MOTOR_SPEED) {
        return command->target_mdeg || command->duration_ms >= PBIO_TRAJECTORY_DURATION_FOREVER_MS ? -EINVAL : 0;
    }
    if (command->mode != BP_MOTOR_POSITION || command->duration_ms || command->speed_mdeg_s < 0) return -EINVAL;
    err = angle_from_mdeg(command->target_mdeg, &checked);
    if (err) return err;
    if (command->action == BP_MOTOR_HOLD) return 0;
    int64_t distance = command->action == BP_MOTOR_RELATIVE ? command->target_mdeg : command->target_mdeg - angle;
    /* Even a constant TRACK target needs bounded int32 feedback differences.
     * This limits displacement, not the long absolute shaft origin. */
    if (distance <= -INT32_MAX / 2 || distance >= INT32_MAX / 2) return -ERANGE;
    if (command->action == BP_MOTOR_TRACK) return 0;
    /* Reject infeasible distance/speed combinations before queuing a command.
     * The worker plans again from its live reference when applying it. */
    pbio_trajectory_command_t trajectory_command = {
        .position_end = checked,
        .speed_target = command->speed_mdeg_s ? command->speed_mdeg_s : d->rated_max_speed * 1000,
        .speed_max = d->rated_max_speed * 1000,
        .acceleration = ACCELERATION_MDEG_S2,
        .deceleration = ACCELERATION_MDEG_S2,
        .continue_running = command->finish == BP_MOTOR_FINISH_CONTINUE,
    };
    if (command->action == BP_MOTOR_RELATIVE) {
        err = angle_from_mdeg(angle + distance, &trajectory_command.position_end);
        if (err) return err;
    }
    angle_from_mdeg(angle, &trajectory_command.position_start);
    pbio_trajectory_t trajectory;
    return pbio_trajectory_new_angle_command(&trajectory, &trajectory_command) == PBIO_SUCCESS ? 0 : -ERANGE;
}

#define DEG_TO_MDEG(deg) ((deg) * 1000)
int motor_control_init_profile(struct motor_control *motor, uint16_t id, int64_t angle_mdeg, int32_t profile)
{
    const struct defaults *d = find_defaults(id);
    if (!d) return -ENOTSUP;
    if (profile < 0 || profile > INT32_MAX / 1000) return -EINVAL;
    pbio_angle_t angle;
    int err = angle_from_mdeg(angle_mdeg, &angle);
    if (err) return err;
    *motor = (struct motor_control){0};
    motor->observer.model = d->model;
    int32_t max_voltage = motor_max_voltage(id);
    motor->max_voltage = max_voltage;
    int32_t precision_profile = profile ? profile : d->precision_profile;
    int32_t nominal_torque = pbio_observer_voltage_to_torque(d->model, pbio_int_math_min(max_voltage, 7500));
    motor->control.settings = (pbio_control_settings_t) {
        // The native boundary uses millidegrees, without a gear-ratio transform.
        .ctl_steps_per_app_step = 1,
        .stall_speed_limit = DEG_TO_MDEG(20),
        .stall_time = pbio_control_time_ms_to_ticks(200),
        .speed_max = DEG_TO_MDEG(d->rated_max_speed),
        // Zero speed in a position command selects this model limit.
        .speed_default = DEG_TO_MDEG(d->rated_max_speed),
        .speed_tolerance = DEG_TO_MDEG(50),
        .position_tolerance = DEG_TO_MDEG(precision_profile),
        .acceleration = ACCELERATION_MDEG_S2,
        .deceleration = ACCELERATION_MDEG_S2,
        .actuation_max = pbio_observer_voltage_to_torque(motor->observer.model, max_voltage),
        .actuation_max_temporary = pbio_observer_voltage_to_torque(motor->observer.model, max_voltage),
        // The nominal voltage is an indication for the nominal torque limit. To
        // ensure proportional control can always get the motor to within the
        // configured tolerance, we select pid_kp such that proportional feedback
        // just exceeds the nominal torque at the tolerance boundary.
        .pid_kp = nominal_torque / precision_profile,
        // Initialize ki such that integral control saturates in about two seconds
        // if the motor were stuck at the position tolerance.
        .pid_ki = nominal_torque / precision_profile / 2,
        // By default, the kd value is the same ratio of kp on all motors to
        // get a comparable step response. This value is not reduced along with
        // the user given precision profile like kp, so we use the default
        // value from the reduced settings.
        .pid_kd = nominal_torque / d->precision_profile / 8,
        .pid_kp_low_pct = 50,
        .pid_kp_low_error_threshold = DEG_TO_MDEG(5),
        .pid_kp_low_speed_threshold = DEG_TO_MDEG(d->pid_kp_low_speed_threshold),
        .integral_deadzone = DEG_TO_MDEG(8),
        .integral_change_max = DEG_TO_MDEG(15),
        .smart_passive_hold_time = pbio_control_time_ms_to_ticks(100),
    };

    motor->observer.settings = (pbio_observer_settings_t) {
        .stall_speed_limit = motor->control.settings.stall_speed_limit,
        .stall_time = motor->control.settings.stall_time,
        .feedback_voltage_negligible = pbio_observer_torque_to_voltage(motor->observer.model, motor->observer.model->torque_friction) * 5 / 2,
        .feedback_voltage_stall_ratio = 75,
        .feedback_gain_low = d->feedback_gain_low,
        .feedback_gain_high = d->feedback_gain_low * 7,
        .feedback_gain_threshold = DEG_TO_MDEG(20),
        .coulomb_friction_speed_cutoff = 500,
    };

    pbio_control_reset(&motor->control);
    pbio_observer_reset(&motor->observer, &angle);
    return 0;
}
#undef DEG_TO_MDEG

int motor_control_init(struct motor_control *motor, uint16_t id, int64_t angle)
{
    return motor_control_init_profile(motor, id, angle, 0);
}

int motor_control_reset_angle(struct motor_control *motor, int64_t measured)
{
    pbio_angle_t angle;
    int err = angle_from_mdeg(measured, &angle);
    if (err) return err;
    pbio_control_reset(&motor->control);
    pbio_observer_reset(&motor->observer, &angle);
    motor->timed_hold = false;
    return 0;
}

/* Settings conversions and diagnostics follow pb_type_control.c and
 * control_settings.c at a7b1b04ae. The running controller remains in shaft
 * millidegrees; only a temporary settings copy uses the application's scale. */
void motor_control_settings_get(const pbio_control_settings_t *settings, int32_t scale,
    struct bp_servo_settings *value)
{
    pbio_control_settings_t s = *settings;
    s.ctl_steps_per_app_step = scale;
    value->scale = scale;
    pbio_control_settings_get_trajectory_limits(&s, &value->speed, &value->acceleration, &value->deceleration);
    value->torque = pbio_control_settings_get_actuation_limit(&s);
    pbio_control_settings_get_pid(&s, &value->kp, &value->ki, &value->kd,
        &value->integral_deadzone, &value->integral_rate);
    pbio_control_settings_get_target_tolerances(&s, &value->target_speed, &value->target_position);
    pbio_control_settings_get_stall_tolerances(&s, &value->stall_speed, &value->stall_time);
}

int motor_control_settings_set(pbio_control_settings_t *settings, uint32_t group,
    const struct bp_servo_settings *value)
{
    if (value->scale < 1) return -EINVAL;
    pbio_control_settings_t s = *settings;
    s.ctl_steps_per_app_step = value->scale;
    pbio_error_t err;
    switch (group) {
    case BP_SERVO_LIMITS:
        err = pbio_control_settings_set_trajectory_limits(&s, value->speed, value->acceleration, value->deceleration);
        if (!err) err = pbio_control_settings_set_actuation_limit(&s, value->torque);
        break;
    case BP_SERVO_PID:
        err = pbio_control_settings_set_pid(&s, value->kp, value->ki, value->kd,
            value->integral_deadzone, value->integral_rate);
        break;
    case BP_SERVO_TARGET_TOLERANCES:
        err = pbio_control_settings_set_target_tolerances(&s, value->target_speed, value->target_position);
        break;
    case BP_SERVO_STALL_TOLERANCES:
        err = pbio_control_settings_set_stall_tolerances(&s, value->stall_speed, value->stall_time);
        break;
    default:
        return -EINVAL;
    }
    if (err) return -EINVAL;
    s.ctl_steps_per_app_step = 1;
    *settings = s;
    return 0;
}

void motor_control_settings_apply(struct motor_control *motor, const pbio_control_settings_t *settings)
{
    /* PBIO samples the torque cap when a command starts, including temporary
     * stall caps. A settings update must not erase that command-local limit. */
    int32_t temporary = motor->control.settings.actuation_max_temporary;
    motor->control.settings = *settings;
    motor->control.settings.actuation_max_temporary = temporary;
}

void motor_control_trajectory_get(struct motor_control *motor, struct bp_servo_trajectory *value)
{
    pbio_control_t *ctl = &motor->control;
    pbio_trajectory_t *t = &ctl->trajectory;
    bool active = pbio_control_is_active(ctl);
    *value = (struct bp_servo_trajectory){
        .active = active, .done = pbio_control_is_done(ctl),
        .stalled = active && (ctl->status & PBIO_CONTROL_STATUS_STALLED),
        .load = active ? ctl->pid_average / 1000 : 0,
    };
    if (!active) return;
    const int32_t points[] = {0, t->t1 / 10, t->t2 / 10, t->t3 / 10,
        0, t->th1 / 1000, t->th2 / 1000, t->th3 / 1000,
        t->w0 / 10, t->w1 / 10, t->w3 / 10, t->a0, t->a2};
    memcpy(value->points, points, sizeof(points));
}

void motor_model_settings_get(const pbio_observer_settings_t *s, struct bp_motor_model_settings *value)
{
    *value = (struct bp_motor_model_settings){.stall_speed = s->stall_speed_limit,
        .stall_ticks = s->stall_time, .negligible_voltage = s->feedback_voltage_negligible,
        .stall_ratio = s->feedback_voltage_stall_ratio, .gain_low = s->feedback_gain_low,
        .gain_high = s->feedback_gain_high, .gain_threshold = s->feedback_gain_threshold,
        .friction_cutoff = s->coulomb_friction_speed_cutoff};
}

int motor_model_settings_set(uint16_t id, pbio_observer_settings_t *settings, const struct bp_motor_model_settings *s)
{
    const struct defaults *d = find_defaults(id);
    if (!d) return -ENOTSUP;
    /* The reference binding assigns raw integers without validation. Preserve
     * useful tuning but exclude a zero divisor and overflow in the friction
     * interpolation. Feedback products are checked against each actual frame. */
    if (s->stall_speed < 0 || s->negligible_voltage < 0 || s->stall_ratio < 0 || s->stall_ratio > 100 ||
        s->gain_low < 0 || s->gain_high < 0 || s->gain_threshold < 0 || s->friction_cutoff <= 0 ||
        s->friction_cutoff > INT32_MAX / d->model->torque_friction) return -EINVAL;
    *settings = (pbio_observer_settings_t){.stall_speed_limit = s->stall_speed,
        .stall_time = s->stall_ticks, .feedback_voltage_negligible = s->negligible_voltage,
        .feedback_voltage_stall_ratio = s->stall_ratio, .feedback_gain_low = s->gain_low,
        .feedback_gain_high = s->gain_high, .feedback_gain_threshold = s->gain_threshold,
        .coulomb_friction_speed_cutoff = s->friction_cutoff};
    return 0;
}

int motor_control_state_get(struct motor_control *motor, int64_t measured, pbio_control_state_t *state)
{
    int err = angle_from_mdeg(measured, &state->position);
    if (err) return err;
    pbio_observer_get_estimated_state(&motor->observer, &state->speed,
        &state->position_estimate, &state->speed_estimate);
    /* A corrupt angle jump must not overflow observer feedback arithmetic. */
    int64_t error = llabs(measured - angle_mdeg(&state->position_estimate));
    const pbio_observer_settings_t *s = &motor->observer.settings;
    if (error >= INT32_MAX) return -ERANGE;
    int64_t low = error < s->feedback_gain_threshold ? error : s->feedback_gain_threshold;
    return low * s->feedback_gain_low + (error - low) * s->feedback_gain_high > INT32_MAX ? -ERANGE : 0;
}

static int result(pbio_error_t err)
{
    return err == PBIO_SUCCESS ? 0 : err == PBIO_ERROR_INVALID_ARG ? -ERANGE : -EDOM;
}

static int run_duration(struct motor_control *motor, uint32_t ticks, int64_t angle,
    int32_t speed, uint32_t duration, pbio_control_on_completion_t finish, bool forever)
{
    if (finish > PBIO_CONTROL_ON_COMPLETION_BRAKE_SMART || finish < 0 ||
        speed < -motor->control.settings.speed_max || speed > motor->control.settings.speed_max ||
        duration >= PBIO_TRAJECTORY_DURATION_FOREVER_MS) return -EINVAL;
    pbio_control_state_t state;
    int err = motor_control_state_get(motor, angle, &state);
    if (err) return err;
    /* PBIO's timed HOLD narrows the absolute position through int32 application
     * units. Complete with brake, then start a zero-relative position command
     * below, preserving the long millidegree angle. */
    bool hold = !forever && finish == PBIO_CONTROL_ON_COMPLETION_HOLD;
    err = result(pbio_control_start_timed_control(&motor->control, ticks, &state,
        forever ? PBIO_TRAJECTORY_DURATION_FOREVER_MS : duration, speed,
        forever ? PBIO_CONTROL_ON_COMPLETION_CONTINUE : hold ? PBIO_CONTROL_ON_COMPLETION_BRAKE : finish));
    if (!err) motor->timed_hold = hold;
    return err;
}

int motor_control_run(struct motor_control *motor, uint32_t ticks, int64_t angle,
    int32_t speed, uint32_t duration, pbio_control_on_completion_t finish)
{
    return run_duration(motor, ticks, angle, speed, duration, finish, !duration);
}

int32_t motor_control_speed_limit(uint16_t id)
{
    const struct defaults *d = find_defaults(id);
    return d ? d->rated_max_speed * 1000 : 0;
}

/* The public PBIO hold entry takes int32 application units. Our controller uses
 * physical millidegrees with a long accumulated position. Preserve its state
 * transition and replace only the constant trajectory through the public API. */
static int track(struct motor_control *motor, uint32_t ticks, int64_t target)
{
    pbio_trajectory_command_t command = {0};
    int err = angle_from_mdeg(target, &command.position_start);
    if (err) return err;
    command.position_end = command.position_start;
    err = result(pbio_control_start_position_control_hold(&motor->control, ticks, 0));
    if (err) return err;
    command.time_start = pbio_control_get_ref_time(&motor->control, ticks);
    pbio_trajectory_make_constant(&motor->control.trajectory, &command);
    motor->timed_hold = false;
    return 0;
}

int motor_control_command(struct motor_control *motor, uint32_t ticks, int64_t angle,
    const struct bp_motor_command *command, int32_t battery_mv)
{
    int32_t speed = pbio_int_math_clamp(command->speed_mdeg_s, motor->control.settings.speed_max);
    pbio_control_on_completion_t finish = command->finish;
    if (command->action == BP_MOTOR_TRAJECTORY) {
        return command->mode == BP_MOTOR_SPEED ?
            motor_control_run(motor, ticks, angle, speed, command->duration_ms, finish) :
            motor_control_target(motor, ticks, angle, command->target_mdeg, speed, finish);
    }
    if (command->action == BP_MOTOR_TIMED) {
        return run_duration(motor, ticks, angle, speed, command->duration_ms, finish, false);
    }
    if (command->action == BP_MOTOR_UNTIL_STALLED) {
        /* Smart endpoints belong to finite angle moves. Waiting for the end
         * of a forever trajectory after stall would keep driving the stop. */
        if (finish == PBIO_CONTROL_ON_COMPLETION_COAST_SMART) finish = PBIO_CONTROL_ON_COMPLETION_COAST;
        if (finish == PBIO_CONTROL_ON_COMPLETION_BRAKE_SMART) finish = PBIO_CONTROL_ON_COMPLETION_BRAKE;
        int err = motor_control_run(motor, ticks, angle, speed, 0, PBIO_CONTROL_ON_COMPLETION_CONTINUE);
        if (err) return err;
        motor->timed_hold = finish == PBIO_CONTROL_ON_COMPLETION_HOLD;
        motor->control.on_completion = motor->timed_hold ? PBIO_CONTROL_ON_COMPLETION_BRAKE : finish;
        motor->control.type |= PBIO_CONTROL_TYPE_FLAG_OBJECTIVE_IS_STALL;
        if (command->duty_limit >= 0) motor->control.settings.actuation_max_temporary =
            pbio_observer_voltage_to_torque(motor->observer.model, battery_mv * command->duty_limit / 1000);
        return 0;
    }
    pbio_control_state_t state;
    int err = motor_control_state_get(motor, angle, &state);
    if (err) return err;
    if (command->action == BP_MOTOR_HOLD) {
        pbio_trajectory_reference_t ref = {.position = state.position};
        if (pbio_control_is_active(&motor->control)) pbio_control_get_reference(&motor->control, ticks, &state, &ref);
        return track(motor, ticks, angle_mdeg(&ref.position));
    }
    if (command->action == BP_MOTOR_TRACK) return track(motor, ticks, command->target_mdeg);
    if (command->action != BP_MOTOR_RELATIVE) return -EINVAL;
    err = result(pbio_control_start_position_control_relative(&motor->control, ticks,
        &state, command->target_mdeg, speed, finish, true));
    if (!err) motor->timed_hold = false;
    return err;
}

int motor_control_target(struct motor_control *motor, uint32_t ticks, int64_t angle,
    int64_t target, int32_t speed, pbio_control_on_completion_t finish)
{
    if (finish > PBIO_CONTROL_ON_COMPLETION_BRAKE_SMART || finish < 0 || speed < 0 ||
        speed > motor->control.settings.speed_max) return -EINVAL;
    pbio_control_state_t state;
    int err = motor_control_state_get(motor, angle, &state);
    if (err) return err;
    pbio_angle_t checked_target;
    err = angle_from_mdeg(target, &checked_target);
    if (err) return err;
    pbio_trajectory_reference_t ref = {.position = state.position};
    if (pbio_control_is_active(&motor->control)) {
        pbio_control_get_reference(&motor->control, ticks, &state, &ref);
    }
    int64_t distance = target - angle_mdeg(&ref.position);
    if (distance <= -INT32_MAX / 2 || distance >= INT32_MAX / 2) return -ERANGE;
    err = result(pbio_control_start_position_control_relative(&motor->control, ticks, &state,
        distance, speed, finish, false));
    if (!err) motor->timed_hold = false;
    return err;
}

int motor_control_reference(pbio_control_t *control, uint32_t ticks,
    const pbio_control_state_t *state, pbio_trajectory_reference_t *ref)
{
    pbio_trajectory_get_reference(&control->trajectory,
        pbio_control_get_ref_time(control, ticks), ref);
    pbio_trajectory_reference_t end;
    pbio_trajectory_get_endpoint(&control->trajectory, &end);
    int64_t angle = angle_mdeg(&state->position);
    int64_t error = angle_mdeg(&ref->position) - angle;
    int64_t remaining = angle_mdeg(&end.position) - angle;
    int64_t speed_error = (int64_t)ref->speed - state->speed_estimate;
    if (llabs(error) >= INT32_MAX / 4 || llabs(remaining) >= INT32_MAX ||
        llabs(speed_error) >= INT32_MAX || state->speed == INT32_MIN) return -ERANGE;
    /* PBIO computes differences and the sum of PID terms in int32. Reserve
     * room for derivative and bounded integral torque before entering it. */
    int64_t derivative = llabs(speed_error) *
        control->settings.pid_kd / 1000;
    int64_t proportional = llabs(error) * control->settings.pid_kp / 1000;
    int64_t windup_margin = llabs((int64_t)state->speed) / (1000 / PBIO_CONFIG_CONTROL_LOOP_TIME_MS) * 2;
    if (proportional + derivative + control->settings.actuation_max >= INT32_MAX ||
        windup_margin * control->settings.pid_kp / 1000 + control->settings.actuation_max_temporary >= INT32_MAX) return -ERANGE;
    return 0;
}

int motor_control_step(struct motor_control *motor, uint32_t ticks, int64_t angle,
    struct motor_reference *out, struct motor_pid_sample *log)
{
    pbio_control_state_t state;
    int err = motor_control_state_get(motor, angle, &state);
    if (err) return err;
    if (!pbio_control_is_active(&motor->control)) return -EAGAIN;
    pbio_trajectory_reference_t ref;
    err = motor_control_reference(&motor->control, ticks, &state, &ref);
    if (err) return err;
    int64_t error = angle_mdeg(&ref.position) - angle;
    pbio_dcmotor_actuation_t actuation;
    int32_t feedback;
    bool pause = false;
    if (log) {
        /* PBIO's CSV hook converts its positions to int32 application units.
         * Our controller uses shaft millidegrees. Reject an unrepresentable
         * sample rather than let the diagnostic conversion wrap. Motion keeps
         * its full angle range; only logging stops on this error. */
        int64_t logged_ref = angle_mdeg(&ref.position);
        if (pbio_control_type_is_time(&motor->control)) {
            logged_ref = angle + pbio_speed_integrator_get_error(&motor->control.speed_integrator, error);
        }
        int64_t estimate = angle_mdeg(&state.position_estimate);
        log->error = angle < INT32_MIN || angle > INT32_MAX || estimate < INT32_MIN ||
            estimate > INT32_MAX || logged_ref < INT32_MIN || logged_ref > INT32_MAX ? -ERANGE : 0;
        if (!log->error) {
            /* Include the D term already produced by the reference hook.
             * Its pinned default width accidentally omits that final value. */
            pbio_logger_start(&motor->control.log, log->values, 1, BP_CONTROL_LOG_COLUMNS, 1);
        }
    }
    pbio_control_update(&motor->control, ticks, &state, &ref, &actuation, &feedback, &pause);
    pbio_logger_stop(&motor->control.log);
    motor->control.log.data = NULL;
    int32_t feedforward = pbio_observer_get_feedforward_torque(motor->observer.model,
        ref.speed, ref.acceleration);
    int32_t torque = pbio_int_math_clamp(feedback + feedforward,
        motor->control.settings.actuation_max_temporary);
    int32_t voltage = actuation == PBIO_DCMOTOR_ACTUATION_TORQUE ?
        pbio_observer_torque_to_voltage(motor->observer.model, torque) : 0;
    bool done = pbio_control_is_done(&motor->control);
    if (motor->timed_hold && done) {
        err = result(pbio_control_start_position_control_relative(&motor->control, ticks,
            &state, 0, 0, PBIO_CONTROL_ON_COMPLETION_HOLD, false));
        motor->timed_hold = false;
        if (err) return err;
    }
    *out = (struct motor_reference){.angle_mdeg = angle_mdeg(&ref.position),
        .speed_mdeg_s = ref.speed, .acceleration_mdeg_s2 = ref.acceleration,
        .estimated_speed = state.speed_estimate,
        .feedback_torque = feedback, .feedforward_torque = feedforward,
        .voltage_mv = pbio_int_math_clamp(voltage, motor->max_voltage),
        .coast = actuation == PBIO_DCMOTOR_ACTUATION_COAST,
        .active = pbio_control_is_active(&motor->control), .done = done,
        .stalled = (motor->control.status & PBIO_CONTROL_STATUS_STALLED) != 0};
    return 0;
}

int motor_control_predict(struct motor_control *motor, uint32_t ticks,
    int64_t measured, bool coast, int32_t voltage)
{
    if (voltage < -12000 || voltage > 12000 || (coast && voltage)) return -EINVAL;
    pbio_control_state_t state;
    int err = motor_control_state_get(motor, measured, &state);
    if (err) return err;
    pbio_observer_update(&motor->observer, ticks, &state.position,
        coast ? PBIO_DCMOTOR_ACTUATION_COAST : PBIO_DCMOTOR_ACTUATION_VOLTAGE, voltage);
    return 0;
}

int motor_control_observe(struct motor_control *motor, uint32_t ticks,
    int64_t measured, bool coast, struct motor_observation *out)
{
    if (!out) return -EINVAL;
    pbio_control_state_t state;
    int err = motor_control_state_get(motor, measured, &state);
    if (err) return err;
    struct motor_observation value = {.estimated_angle = angle_mdeg(&state.position_estimate),
        .estimated_speed = state.speed_estimate, .estimated_current = motor->observer.current,
        .model_stalled = motor->observer.stalled};
    if (!coast) {
        uint32_t duration;
        if (pbio_control_is_active(&motor->control)) {
            value.load_mnm = pbio_control_settings_actuation_ctl_to_app(-motor->control.pid_average);
            /* The boolean is already published by control_update. Its getter
             * also computes duration from a global PBIO clock; this adapter
             * receives time explicitly and needs no scheduler clock hook. */
            value.stalled = (motor->control.status & PBIO_CONTROL_STATUS_STALLED) != 0;
        } else {
            int32_t feedback = pbio_observer_get_feedback_voltage(&motor->observer, &state.position);
            int32_t torque = pbio_observer_voltage_to_torque(motor->observer.model, feedback);
            value.load_mnm = pbio_control_settings_actuation_ctl_to_app(torque);
            value.stalled = pbio_observer_is_stalled(&motor->observer, ticks, &duration);
        }
    }
    *out = value;
    return 0;
}

/* The 10 servo log values from PBIO servo.c, before predicting the next state.
 * Buffer ownership, elapsed time, downsampling and output units belong to the
 * port service. No copy of controller/PID equations is needed here. */
int motor_control_log_sample(struct motor_control *motor, uint32_t ticks, int64_t angle,
    bool coast, int32_t voltage, const struct motor_reference *reference, struct motor_log_sample *value)
{
    pbio_control_state_t state;
    int err = motor_control_state_get(motor, angle, &state);
    if (err) return err;
    uint32_t duration;
    bool stalled = pbio_control_is_active(&motor->control) ?
        motor->control.status & PBIO_CONTROL_STATUS_STALLED :
        pbio_observer_is_stalled(&motor->observer, ticks, &duration);
    *value = (struct motor_log_sample){.angle_mdeg = angle,
        .estimated_angle = angle_mdeg(&state.position_estimate), .speed = state.speed,
        .estimated_speed = state.speed_estimate,
        .flags = (coast ? PBIO_DCMOTOR_ACTUATION_COAST : PBIO_DCMOTOR_ACTUATION_VOLTAGE) | ((int32_t)stalled << 2),
        .voltage = voltage, .feedback_torque = reference->feedback_torque,
        .feedforward_torque = reference->feedforward_torque,
        .feedback_voltage = pbio_observer_get_feedback_voltage(&motor->observer, &state.position)};
    return 0;
}
