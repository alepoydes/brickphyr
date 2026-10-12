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
    if (command->finish > BP_MOTOR_FINISH_CONTINUE ||
        command->speed_mdeg_s < -d->rated_max_speed * 1000 ||
        command->speed_mdeg_s > d->rated_max_speed * 1000) return -EINVAL;
    pbio_angle_t checked;
    int err = angle_from_mdeg(angle, &checked);
    if (err) return err;
    if (command->mode == BP_MOTOR_SPEED) {
        return command->target_mdeg || command->duration_ms >= PBIO_TRAJECTORY_DURATION_FOREVER_MS ? -EINVAL : 0;
    }
    if (command->mode != BP_MOTOR_POSITION || command->duration_ms || command->speed_mdeg_s < 0) return -EINVAL;
    err = angle_from_mdeg(command->target_mdeg, &checked);
    if (err) return err;
    int64_t distance = command->target_mdeg - angle;
    if (distance <= -INT32_MAX / 2 || distance >= INT32_MAX / 2) return -ERANGE;
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
    angle_from_mdeg(angle, &trajectory_command.position_start);
    pbio_trajectory_t trajectory;
    return pbio_trajectory_new_angle_command(&trajectory, &trajectory_command) == PBIO_SUCCESS ? 0 : -ERANGE;
}

#define DEG_TO_MDEG(deg) ((deg) * 1000)
int motor_control_init(struct motor_control *motor, uint16_t id, int64_t angle_mdeg)
{
    const struct defaults *d = find_defaults(id);
    if (!d) return -ENOTSUP;
    pbio_angle_t angle;
    int err = angle_from_mdeg(angle_mdeg, &angle);
    if (err) return err;
    *motor = (struct motor_control){0};
    motor->observer.model = d->model;
    int32_t max_voltage = motor_max_voltage(id);
    motor->max_voltage = max_voltage;
    int32_t precision_profile = d->precision_profile;
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

static int state_get(struct motor_control *motor, int64_t measured, pbio_control_state_t *state)
{
    int err = angle_from_mdeg(measured, &state->position);
    if (err) return err;
    pbio_observer_get_estimated_state(&motor->observer, &state->speed,
        &state->position_estimate, &state->speed_estimate);
    /* A corrupt angle jump must not overflow observer feedback arithmetic. */
    int64_t error = measured - angle_mdeg(&state->position_estimate);
    int32_t bound = INT32_MAX / motor->observer.settings.feedback_gain_high;
    return error < -bound || error > bound ? -ERANGE : 0;
}

static int result(pbio_error_t err)
{
    return err == PBIO_SUCCESS ? 0 : err == PBIO_ERROR_INVALID_ARG ? -ERANGE : -EDOM;
}

int motor_control_run(struct motor_control *motor, uint32_t ticks, int64_t angle,
    int32_t speed, uint32_t duration, pbio_control_on_completion_t finish)
{
    if (finish > PBIO_CONTROL_ON_COMPLETION_CONTINUE || finish < 0 ||
        speed < -motor->control.settings.speed_max || speed > motor->control.settings.speed_max ||
        duration >= PBIO_TRAJECTORY_DURATION_FOREVER_MS) return -EINVAL;
    pbio_control_state_t state;
    int err = state_get(motor, angle, &state);
    if (err) return err;
    /* PBIO's timed HOLD narrows the absolute position through int32 application
     * units. Complete with brake, then start a zero-relative position command
     * below, preserving the long millidegree angle. */
    bool hold = duration && finish == PBIO_CONTROL_ON_COMPLETION_HOLD;
    err = result(pbio_control_start_timed_control(&motor->control, ticks, &state,
        duration ? duration : PBIO_TRAJECTORY_DURATION_FOREVER_MS, speed,
        !duration ? PBIO_CONTROL_ON_COMPLETION_CONTINUE : hold ? PBIO_CONTROL_ON_COMPLETION_BRAKE : finish));
    if (!err) motor->timed_hold = hold;
    return err;
}

int motor_control_target(struct motor_control *motor, uint32_t ticks, int64_t angle,
    int64_t target, int32_t speed, pbio_control_on_completion_t finish)
{
    if (finish > PBIO_CONTROL_ON_COMPLETION_CONTINUE || finish < 0 || speed < 0 ||
        speed > motor->control.settings.speed_max) return -EINVAL;
    pbio_control_state_t state;
    int err = state_get(motor, angle, &state);
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

int motor_control_step(struct motor_control *motor, uint32_t ticks, int64_t angle,
    struct motor_reference *out)
{
    pbio_control_state_t state;
    int err = state_get(motor, angle, &state);
    if (err) return err;
    if (!pbio_control_is_active(&motor->control)) return -EAGAIN;
    pbio_trajectory_reference_t ref;
    pbio_trajectory_get_reference(&motor->control.trajectory,
        pbio_control_get_ref_time(&motor->control, ticks), &ref);
    pbio_trajectory_reference_t end;
    pbio_trajectory_get_endpoint(&motor->control.trajectory, &end);
    int64_t error = angle_mdeg(&ref.position) - angle;
    int64_t remaining = angle_mdeg(&end.position) - angle;
    /* PBIO computes differences and the sum of PID terms in int32. Reserve
     * room for derivative and bounded integral torque before entering it. */
    int64_t derivative = llabs((int64_t)ref.speed - state.speed_estimate) *
        motor->control.settings.pid_kd / 1000;
    int64_t proportional = llabs(error) * motor->control.settings.pid_kp / 1000;
    if (llabs(error) >= INT32_MAX / 4 || llabs(remaining) >= INT32_MAX ||
        proportional + derivative + motor->control.settings.actuation_max >= INT32_MAX) return -ERANGE;
    pbio_dcmotor_actuation_t actuation;
    int32_t feedback;
    bool pause = false;
    pbio_control_update(&motor->control, ticks, &state, &ref, &actuation, &feedback, &pause);
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
    int err = state_get(motor, measured, &state);
    if (err) return err;
    pbio_observer_update(&motor->observer, ticks, &state.position,
        coast ? PBIO_DCMOTOR_ACTUATION_COAST : PBIO_DCMOTOR_ACTUATION_VOLTAGE, voltage);
    return 0;
}
