/*
	Copyright 2016 Benjamin Vedder	benjamin@vedder.se

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "mc_interface.h"
#include "mcpwm.h"
#include "mcpwm2.h"
#include "mcpwm_foc.h"
#include "mcpwm_foc2.h"
#include "ledpwm.h"
#include "stm32f4xx_conf.h"
#include "hw.h"
#include "terminal.h"
#include "utils.h"
#include "ch.h"
#include "hal.h"
#include "commands.h"
#include "encoder.h"
#include "drv8301.h"
#include "buffer.h"
#include <math.h>
#include "smart_switch.h"

// Macros
#define DIR_MULT        (m_conf.m_invert_direction ? -1.0 : 1.0)
#define DIR_MULT2        (m_conf2.m_invert_direction ? -1.0 : 1.0)

// Global variables
volatile uint16_t ADC_Value[HW_ADC_CHANNELS];
volatile int ADC_curr_norm_value[3];

// Private variables
static volatile mc_configuration m_conf, m_conf2;
static mc_fault_code m_fault_now;
static int m_ignore_iterations;
static volatile unsigned int m_cycles_running;
static volatile bool m_lock_enabled;
static volatile bool m_lock_override_once;
static volatile float m_motor_current_sum;
static volatile float m_motor_current_sum2;
static volatile float m_input_current_sum;
static volatile float m_motor_current_iterations;
static volatile float m_motor_current_iterations2;
static volatile float m_input_current_iterations;
static volatile float m_motor_id_sum;
static volatile float m_motor_id_sum2;
static volatile float m_motor_iq_sum;
static volatile float m_motor_iq_sum2;
static volatile float m_motor_id_iterations;
static volatile float m_motor_id_iterations2;
static volatile float m_motor_iq_iterations;
static volatile float m_motor_iq_iterations2;
static volatile float m_amp_seconds;
static volatile float m_amp_seconds_charged;
static volatile float m_watt_seconds;
static volatile float m_watt_seconds_charged;
static volatile float m_position_set1;
static volatile float m_position_set2;
static volatile float m_temp_fet;
static volatile float m_temp_fet2;
static volatile float m_temp_motor;
static volatile float m_temp_motor2;
static volatile float v_in_last = 12.0;
static volatile float capacity_estimate = -1.0;
static volatile long unsigned int msec_inactive = 0;

// Sampling variables
#define ADC_SAMPLE_MAX_LEN      2000
__attribute__((section(".ram4")))      static volatile int16_t m_curr0_samples[ADC_SAMPLE_MAX_LEN];
__attribute__((section(".ram4")))      static volatile int16_t m_curr1_samples[ADC_SAMPLE_MAX_LEN];
__attribute__((section(".ram4")))      static volatile int16_t m_ph1_samples[ADC_SAMPLE_MAX_LEN];
__attribute__((section(".ram4")))      static volatile int16_t m_ph2_samples[ADC_SAMPLE_MAX_LEN];
__attribute__((section(".ram4")))      static volatile int16_t m_ph3_samples[ADC_SAMPLE_MAX_LEN];
__attribute__((section(".ram4")))      static volatile int16_t m_va1_samples[ADC_SAMPLE_MAX_LEN];
__attribute__((section(".ram4")))      static volatile int16_t m_vb1_samples[ADC_SAMPLE_MAX_LEN];
__attribute__((section(".ram4")))      static volatile int16_t m_va2_samples[ADC_SAMPLE_MAX_LEN];
__attribute__((section(".ram4")))      static volatile int16_t m_vb2_samples[ADC_SAMPLE_MAX_LEN];
__attribute__((section(".ram4")))      static volatile int16_t m_vzero_samples[ADC_SAMPLE_MAX_LEN];
__attribute__((section(".ram4")))      static volatile uint8_t m_status_samples[ADC_SAMPLE_MAX_LEN];
__attribute__((section(".ram4")))      static volatile int16_t m_curr_fir_samples[ADC_SAMPLE_MAX_LEN];
__attribute__((section(".ram4")))      static volatile int16_t m_f_sw_samples[ADC_SAMPLE_MAX_LEN];
__attribute__((section(".ram4")))      static volatile int8_t m_phase_samples[ADC_SAMPLE_MAX_LEN];
static volatile int m_sample_len;
static volatile bool measureFluxLinkage;
static volatile bool measurementDone1;
static volatile bool measurementDone2;
static volatile bool dir1_measured;
static volatile bool dir2_measured;
static volatile int m_sample_int;
static volatile debug_sampling_mode m_sample_mode;
static volatile debug_sampling_mode m_sample_mode_last;
static volatile int m_sample_now;
static volatile int m_sample_trigger;
static volatile float m_last_adc_duration_sample;

// Private functions
static void update_override_limits(volatile mc_configuration *conf, volatile mc_configuration *conf2);

// Function pointers
static void (*pwn_done_func)(void) = 0;

// Threads
static THD_WORKING_AREA(timer_thread_wa, 1024);
static THD_FUNCTION(timer_thread, arg);
static THD_WORKING_AREA(sample_send_thread_wa, 1024);
static THD_FUNCTION(sample_send_thread, arg);
static thread_t *sample_send_tp;

void mc_interface_init(mc_configuration *configuration, mc_configuration *configuration2) {
  m_conf = *configuration;
  m_conf2 = *configuration2;
  m_fault_now = FAULT_CODE_NONE;
  m_ignore_iterations = 0;
  m_cycles_running = 0;
  m_lock_enabled = false;
  m_lock_override_once = false;
  m_motor_current_sum = 0.0;
  m_motor_current_sum2 = 0.0;
  m_input_current_sum = 0.0;
  m_motor_current_iterations = 0.0;
  m_motor_current_iterations2 = 0.0;
  m_input_current_iterations = 0.0;
  m_motor_id_sum = 0.0;
  m_motor_iq_sum = 0.0;
  m_motor_id_sum2 = 0.0;
  m_motor_iq_sum2 = 0.0;
  m_motor_id_iterations = 0.0;
  m_motor_iq_iterations = 0.0;
  m_motor_id_iterations2 = 0.0;
  m_motor_iq_iterations2 = 0.0;
  m_amp_seconds = 0.0;
  m_amp_seconds_charged = 0.0;
  m_watt_seconds = 0.0;
  m_watt_seconds_charged = 0.0;
  m_position_set1 = 0.0;
  m_position_set2 = 0.0;
  m_last_adc_duration_sample = 0.0;
  m_temp_fet = 0.0;
  m_temp_motor = 0.0;

  m_sample_len = 1000;
  m_sample_int = 1;
  m_sample_now = 0;
  m_sample_trigger = 0;
  m_sample_mode = DEBUG_SAMPLING_OFF;
  m_sample_mode_last = DEBUG_SAMPLING_OFF;

  // Start threads
  chThdCreateStatic(timer_thread_wa, sizeof(timer_thread_wa), NORMALPRIO, timer_thread, NULL);
  chThdCreateStatic(sample_send_thread_wa, sizeof(sample_send_thread_wa), NORMALPRIO - 1, sample_send_thread, NULL);

#ifdef HW_HAS_DRV8301
  drv8301_set_oc_mode(configuration->m_drv8301_oc_mode);
  drv8301_set_oc_adj(configuration->m_drv8301_oc_adj);
#endif

  // Initialize encoder
#if !WS2811_ENABLE
  switch (m_conf.m_sensor_port_mode) {
  case SENSOR_PORT_MODE_ABI:
    encoder_init_abi(m_conf.m_encoder_counts);
    break;

  case SENSOR_PORT_MODE_AS5047_SPI:
    encoder_init_as5047p_spi();
    break;

  default:
    break;
  }
#endif

  // Initialize selected implementation
  switch (m_conf.motor_type) {
  case MOTOR_TYPE_BLDC:
  case MOTOR_TYPE_DC:
    mcpwm_init(&m_conf);
    mcpwm_init2(&m_conf2);
    break;

  case MOTOR_TYPE_FOC:
    mcpwm_foc_init(&m_conf);
    mcpwm_foc_init2(&m_conf2);
    break;

  default:
    break;
  }
}

const volatile mc_configuration* mc_interface_get_configuration(void) {
  return &m_conf;
}

const volatile mc_configuration* mc_interface_get_configuration2(void) {
  return &m_conf2;
}

void mc_interface_set_configuration(mc_configuration *configuration, mc_configuration *configuration2) {
#if !WS2811_ENABLE
  if (m_conf.m_sensor_port_mode != configuration->m_sensor_port_mode) {
    encoder_deinit();
    switch (configuration->m_sensor_port_mode) {
    case SENSOR_PORT_MODE_ABI:
      encoder_init_abi(configuration->m_encoder_counts);
      break;

    case SENSOR_PORT_MODE_AS5047_SPI:
      encoder_init_as5047p_spi();
      break;

    default:
      break;
    }
  }

  if (configuration->m_sensor_port_mode == SENSOR_PORT_MODE_ABI) {
    encoder_set_counts(configuration->m_encoder_counts);
  }
#endif

#ifdef HW_HAS_DRV8301
  drv8301_set_oc_mode(configuration->m_drv8301_oc_mode);
  drv8301_set_oc_adj(configuration->m_drv8301_oc_adj);
#endif

  if (m_conf.motor_type == MOTOR_TYPE_FOC && configuration->motor_type != MOTOR_TYPE_FOC) {
    mcpwm_foc_deinit();
    mcpwm_foc_deinit2();
    m_conf = *configuration;
    m_conf2 = *configuration2;
    mcpwm_init(&m_conf);
    mcpwm_init2(&m_conf2);
  }
  else if (m_conf.motor_type != MOTOR_TYPE_FOC && configuration->motor_type == MOTOR_TYPE_FOC) {
    mcpwm_deinit();
    mcpwm_deinit2();
    m_conf = *configuration;
    m_conf2 = *configuration2;
    mcpwm_foc_init(&m_conf);
    mcpwm_foc_init2(&m_conf2);
  }
  else {
    m_conf = *configuration;
    m_conf2 = *configuration2;
    mcpwm_foc_set_configuration(&m_conf);
    mcpwm_foc_set_configuration2(&m_conf2);
    mcpwm_foc_timer_reinit();
  }
  update_override_limits(&m_conf,&m_conf2);
  switch (m_conf.motor_type) {
  case MOTOR_TYPE_BLDC:
  case MOTOR_TYPE_DC:
    mcpwm_set_configuration(&m_conf);
    mcpwm_set_configuration2(&m_conf2);
    break;

  case MOTOR_TYPE_FOC:
    mcpwm_foc_set_configuration(&m_conf);
    mcpwm_foc_set_configuration2(&m_conf2);
    break;

  default:
    break;
  }

}

bool mc_interface_dccal_done(void) {
  bool ret = false;
  switch (m_conf.motor_type) {
  case MOTOR_TYPE_BLDC:
  case MOTOR_TYPE_DC:
    ret = mcpwm_is_dccal_done();
    break;

  case MOTOR_TYPE_FOC:
    ret = mcpwm_foc_is_dccal_done();
    break;

  default:
    break;
  }

  return ret;
}

/**
 * Set a function that should be called after each PWM cycle.
 *
 * @param p_func
 * The function to be called. 0 will not call any function.
 */
void mc_interface_set_pwm_callback(void (*p_func)(void)) {
  pwn_done_func = p_func;
}

/**
 * Lock the control by disabling all control commands.
 */
void mc_interface_lock(void) {
  m_lock_enabled = true;
}

/**
 * Unlock all control commands.
 */
void mc_interface_unlock(void) {
  m_lock_enabled = false;
}

/**
 * Allow just one motor control command in the locked state.
 */
void mc_interface_lock_override_once(void) {
  m_lock_override_once = true;
}

mc_fault_code mc_interface_get_fault(void) {
  return m_fault_now;
}

const char* mc_interface_fault_to_string(mc_fault_code fault) {
  switch (fault) {
  case FAULT_CODE_NONE:
    return "FAULT_CODE_NONE";
    break;
  case FAULT_CODE_OVER_VOLTAGE:
    return "FAULT_CODE_OVER_VOLTAGE";
    break;
  case FAULT_CODE_UNDER_VOLTAGE:
    return "FAULT_CODE_UNDER_VOLTAGE";
    break;
  case FAULT_CODE_DRV:
    return "FAULT_CODE_DRV";
    break;
  case FAULT_CODE_ABS_OVER_CURRENT:
    return "FAULT_CODE_ABS_OVER_CURRENT";
    break;
  case FAULT_CODE_OVER_TEMP_FET:
    return "FAULT_CODE_OVER_TEMP_FET";
    break;
  case FAULT_CODE_OVER_TEMP_MOTOR:
    return "FAULT_CODE_OVER_TEMP_MOTOR";
    break;
  case FAULT_CODE_DRV2:
    return "FAULT_CODE_DRV2";
    break;
  case FAULT_CODE_ABS_OVER_CURRENT2:
    return "FAULT_CODE_ABS_OVER_CURRENT2";
    break;
  case FAULT_CODE_OVER_TEMP_FET2:
    return "FAULT_CODE_OVER_TEMP_FET2";
    break;
  case FAULT_CODE_OVER_TEMP_MOTOR2:
    return "FAULT_CODE_OVER_TEMP_MOTOR2";
    break;
  default:
    return "FAULT_UNKNOWN";
    break;
  }
}

mc_state mc_interface_get_state(void) {
  mc_state ret = MC_STATE_OFF;
  switch (m_conf.motor_type) {
  case MOTOR_TYPE_BLDC:
  case MOTOR_TYPE_DC:
    ret = mcpwm_get_state();
    break;

  case MOTOR_TYPE_FOC:
    ret = mcpwm_foc_get_state();
    break;

  default:
    break;
  }

  return ret;
}

void mc_interface_set_duty(float dutyCycle1, float dutyCycle2) {
  if (mc_interface_try_input()) {
    return;
  }

  switch (m_conf.motor_type) {
  case MOTOR_TYPE_BLDC:
  case MOTOR_TYPE_DC:
    mcpwm_set_duty(DIR_MULT * dutyCycle1);
    mcpwm_set_duty2(DIR_MULT2 * dutyCycle2);
    break;

  case MOTOR_TYPE_FOC:
    mcpwm_foc_set_duty(DIR_MULT * dutyCycle1);
    mcpwm_foc_set_duty2(DIR_MULT2 * dutyCycle2);
    break;

  default:
    break;
  }
}

void mc_interface_set_duty_noramp(float dutyCycle1, float dutyCycle2) {
  if (mc_interface_try_input()) {
    return;
  }

  switch (m_conf.motor_type) {
  case MOTOR_TYPE_BLDC:
  case MOTOR_TYPE_DC:
    mcpwm_set_duty_noramp(DIR_MULT * dutyCycle1);
    mcpwm_set_duty_noramp2(DIR_MULT2 * dutyCycle2);
    break;

  case MOTOR_TYPE_FOC:
    mcpwm_foc_set_duty_noramp(DIR_MULT * dutyCycle1);
    mcpwm_foc_set_duty_noramp2(DIR_MULT2 * dutyCycle2);
    break;

  default:
    break;
  }
}

void mc_interface_set_pid_speed(float rpm1, float rpm2) {
  if (mc_interface_try_input()) {
    return;
  }

  switch (m_conf.motor_type) {
  case MOTOR_TYPE_BLDC:
  case MOTOR_TYPE_DC:
    mcpwm_set_pid_speed(DIR_MULT * rpm1);
    mcpwm_set_pid_speed2(DIR_MULT2 * rpm2);
    break;

  case MOTOR_TYPE_FOC:
    mcpwm_foc_set_pid_speed(DIR_MULT * rpm1);
    mcpwm_foc_set_pid_speed2(DIR_MULT2 * rpm2);
    break;

  default:
    break;
  }
}

void mc_interface_set_pid_pos(float pos1, float pos2) {
  if (mc_interface_try_input()) {
    return;
  }

  m_position_set1 = pos1;
  m_position_set2 = pos2;

  switch (m_conf.motor_type) {
  case MOTOR_TYPE_BLDC:
  case MOTOR_TYPE_DC:
    mcpwm_set_pid_pos(DIR_MULT * m_position_set1);
    mcpwm_set_pid_pos2(DIR_MULT2 * m_position_set2);
    break;

  case MOTOR_TYPE_FOC:
    mcpwm_foc_set_pid_pos(DIR_MULT * m_position_set1);
    mcpwm_foc_set_pid_pos2(DIR_MULT2 * m_position_set2);
    break;

  default:
    break;
  }
}

void mc_interface_set_current(float current1, float current2) {

  if (mc_interface_try_input()) {
    return;
  }

  switch (m_conf.motor_type) {
  case MOTOR_TYPE_BLDC:
  case MOTOR_TYPE_DC:
    mcpwm_set_current(DIR_MULT * current1);
    mcpwm_set_current2(DIR_MULT2 * current2);
    break;

  case MOTOR_TYPE_FOC:
    mcpwm_foc_set_current(DIR_MULT * current1);
    mcpwm_foc_set_current2(DIR_MULT2 * current2);
    break;

  default:
    break;
  }
}

void mc_interface_set_brake_current(float current1, float current2) {
  if (mc_interface_try_input()) {
    return;
  }

  switch (m_conf.motor_type) {
  case MOTOR_TYPE_BLDC:
  case MOTOR_TYPE_DC:
    mcpwm_set_brake_current(DIR_MULT * current1);
    mcpwm_set_brake_current2(DIR_MULT2 * current2);
    break;

  case MOTOR_TYPE_FOC:
    mcpwm_foc_set_brake_current(DIR_MULT * current1);
    mcpwm_foc_set_brake_current2(DIR_MULT2 * current2);
    break;

  default:
    break;
  }
}

/**
 * Set current relative to the minimum and maximum current limits.
 *
 * @param current
 * The relative current value, range [-1.0 1.0]
 */
void mc_interface_set_current_rel(float val) {

  if (val > 0.0) {
    mc_interface_set_current(val * m_conf.lo_current_motor_max_now, val * m_conf2.lo_current_motor_max_now);
  }
  else {
    mc_interface_set_current(val * fabsf(m_conf.lo_current_motor_min_now), val * fabsf(m_conf2.lo_current_motor_min_now));
  }
}

/**
 * Set brake current relative to the minimum current limit.
 *
 * @param current
 * The relative current value, range [0.0 1.0]
 */
void mc_interface_set_brake_current_rel(float val) {
  mc_interface_set_brake_current(val * m_conf.lo_current_motor_max_now, val * m_conf2.lo_current_motor_max_now);
}

void mc_interface_set_handbrake(float current1, float current2) {
  if (mc_interface_try_input()) {
    return;
  }

  switch (m_conf.motor_type) {
  case MOTOR_TYPE_BLDC:
  case MOTOR_TYPE_DC:
    // TODO: Not implemented yet, use brake mode for now.
    mcpwm_set_brake_current(current1);
    mcpwm_set_brake_current2(current2);
    break;

  case MOTOR_TYPE_FOC:
    mcpwm_foc_set_handbrake(current1);
    mcpwm_foc_set_handbrake(current2);
    break;

  default:
    break;
  }
}

void mc_interface_brake_now(void) {
  mc_interface_set_duty(0.0, 0.0);
}

/**
 * Disconnect the motor and let it turn freely.
 */
void mc_interface_release_motor(void) {
  mc_interface_set_current(0.0, 0.0);
}

/**
 * Stop the motor and use braking.
 */
float mc_interface_get_duty_cycle_set(void) {
  float ret = 0.0;

  switch (m_conf.motor_type) {
  case MOTOR_TYPE_BLDC:
  case MOTOR_TYPE_DC:
    ret = mcpwm_get_duty_cycle_set();
    break;

  case MOTOR_TYPE_FOC:
    ret = mcpwm_foc_get_duty_cycle_set();
    break;

  default:
    break;
  }

  return DIR_MULT * ret;
}

float mc_interface_get_duty_cycle_now(void) {
  float ret = 0.0;
  switch (m_conf.motor_type) {
  case MOTOR_TYPE_BLDC:
  case MOTOR_TYPE_DC:
    ret = mcpwm_get_duty_cycle_now();
    break;

  case MOTOR_TYPE_FOC:
    ret = mcpwm_foc_get_duty_cycle_now();
    break;

  default:
    break;
  }

  return DIR_MULT * ret;
}

float mc_interface_get_duty_cycle_now2(void) {
  float ret = 0.0;
  switch (m_conf.motor_type) {
  case MOTOR_TYPE_BLDC:
  case MOTOR_TYPE_DC:
    ret = mcpwm_get_duty_cycle_now2();
    break;

  case MOTOR_TYPE_FOC:
    ret = mcpwm_foc_get_duty_cycle_now2();
    break;

  default:
    break;
  }

  return DIR_MULT2 * ret;
}

float mc_interface_get_sampling_frequency_now(void) {
  float ret = 0.0;

  switch (m_conf.motor_type) {
  case MOTOR_TYPE_BLDC:
  case MOTOR_TYPE_DC:
    ret = mcpwm_get_switching_frequency_now();
    break;

  case MOTOR_TYPE_FOC:
    ret = mcpwm_foc_get_sampling_frequency_now();
    break;

  default:
    break;
  }

  return ret;
}

float mc_interface_get_rpm(void) {
  float ret = 0.0;

  switch (m_conf.motor_type) {
  case MOTOR_TYPE_BLDC:
  case MOTOR_TYPE_DC:
    ret = mcpwm_get_rpm();
    break;

  case MOTOR_TYPE_FOC:
    ret = mcpwm_foc_get_rpm();
    break;

  default:
    break;
  }

  return DIR_MULT * ret;
}

float mc_interface_get_rpm2(void) {
  float ret = 0.0;

  switch (m_conf.motor_type) {
  case MOTOR_TYPE_BLDC:
  case MOTOR_TYPE_DC:
    ret = mcpwm_get_rpm2();
    break;

  case MOTOR_TYPE_FOC:
    ret = mcpwm_foc_get_rpm2();
    break;

  default:
    break;
  }

  return DIR_MULT2 * ret;
}
/**
 * Get the amount of amp hours drawn from the input source.
 *
 * @param reset
 * If true, the counter will be reset after this call.
 *
 * @return
 * The amount of amp hours drawn.
 */
float mc_interface_get_amp_hours(bool reset) {
  float val = m_amp_seconds / 3600;

  if (reset) {
    m_amp_seconds = 0.0;
  }

  return val;
}

/**
 * Get the amount of amp hours fed back into the input source.
 *
 * @param reset
 * If true, the counter will be reset after this call.
 *
 * @return
 * The amount of amp hours fed back.
 */
float mc_interface_get_amp_hours_charged(bool reset) {
  float val = m_amp_seconds_charged / 3600;

  if (reset) {
    m_amp_seconds_charged = 0.0;
  }

  return val;
}

/**
 * Get the amount of watt hours drawn from the input source.
 *
 * @param reset
 * If true, the counter will be reset after this call.
 *
 * @return
 * The amount of watt hours drawn.
 */
float mc_interface_get_watt_hours(bool reset) {
  float val = m_watt_seconds / 3600;

  if (reset) {
    m_watt_seconds = 0.0;
  }

  return val;
}

float mc_interface_get_capacity(void) {
  return capacity_estimate;
}



/**
 * Get the amount of watt hours fed back into the input source.
 *
 * @param reset
 * If true, the counter will be reset after this call.
 *
 * @return
 * The amount of watt hours fed back.
 */
float mc_interface_get_watt_hours_charged(bool reset) {
  float val = m_watt_seconds_charged / 3600;

  if (reset) {
    m_watt_seconds_charged = 0.0;
  }

  return val;
}

float mc_interface_get_tot_current(void) {
  float ret = 0.0;

  switch (m_conf.motor_type) {
  case MOTOR_TYPE_BLDC:
  case MOTOR_TYPE_DC:
    ret = mcpwm_get_tot_current();
    break;

  case MOTOR_TYPE_FOC:
    ret = mcpwm_foc_get_tot_current();
    break;

  default:
    break;
  }

  return ret;
}

float mc_interface_get_tot_current2(void) {
  float ret = 0.0;

  switch (m_conf.motor_type) {
  case MOTOR_TYPE_BLDC:
  case MOTOR_TYPE_DC:
    ret = mcpwm_get_tot_current2();
    break;

  case MOTOR_TYPE_FOC:
    ret = mcpwm_foc_get_tot_current2();
    break;

  default:
    break;
  }

  return ret;
}

float mc_interface_get_tot_current_filtered(void) {
  float ret = 0.0;

  switch (m_conf.motor_type) {
  case MOTOR_TYPE_BLDC:
  case MOTOR_TYPE_DC:
    ret = mcpwm_get_tot_current_filtered();
    break;

  case MOTOR_TYPE_FOC:
    ret = mcpwm_foc_get_tot_current_filtered();
    break;

  default:
    break;
  }

  return ret;
}

float mc_interface_get_tot_current_filtered2(void) {
  float ret = 0.0;

  switch (m_conf.motor_type) {
  case MOTOR_TYPE_BLDC:
  case MOTOR_TYPE_DC:
    ret = mcpwm_get_tot_current_filtered2();
    break;

  case MOTOR_TYPE_FOC:
    ret = mcpwm_foc_get_tot_current_filtered2();
    break;

  default:
    break;
  }

  return ret;
}

float mc_interface_get_tot_current_directional(void) {

  float ret = 0.0;

  switch (m_conf.motor_type) {
  case MOTOR_TYPE_BLDC:
  case MOTOR_TYPE_DC:
    ret = mcpwm_get_tot_current_directional();
    break;

  case MOTOR_TYPE_FOC:
    ret = mcpwm_foc_get_tot_current_directional();
    break;

  default:
    break;
  }

  return DIR_MULT * ret;
}

float mc_interface_get_tot_current_directional_filtered(void) {
  float ret = 0.0;

  switch (m_conf.motor_type) {
  case MOTOR_TYPE_BLDC:
  case MOTOR_TYPE_DC:
    ret = mcpwm_get_tot_current_directional_filtered();
    break;

  case MOTOR_TYPE_FOC:
    ret = mcpwm_foc_get_tot_current_directional_filtered();
    break;

  default:
    break;
  }

  return DIR_MULT * ret;
}

float mc_interface_get_tot_current_in(void) {
  float ret = 0.0;

  switch (m_conf.motor_type) {
  case MOTOR_TYPE_BLDC:
  case MOTOR_TYPE_DC:
    ret = mcpwm_get_tot_current_in() + mcpwm_get_tot_current_in2();
    break;

  case MOTOR_TYPE_FOC:
    ret = mcpwm_foc_get_tot_current_in() + mcpwm_foc_get_tot_current_in2();
    break;

  default:
    break;
  }

  return ret;
}

float mc_interface_get_tot_current_in_filtered(void) {
  float ret = 0.0;
  switch (m_conf.motor_type) {
  case MOTOR_TYPE_BLDC:
  case MOTOR_TYPE_DC:
    ret = mcpwm_get_tot_current_in_filtered() + mcpwm_get_tot_current_in_filtered2();
    break;

  case MOTOR_TYPE_FOC:
    ret = mcpwm_foc_get_tot_current_in_filtered() + mcpwm_foc_get_tot_current_in_filtered2();
    break;

  default:
    break;
  }

  return ret;
}

int mc_interface_get_tachometer_value(bool reset) {
  int ret = 0;

  switch (m_conf.motor_type) {
  case MOTOR_TYPE_BLDC:
  case MOTOR_TYPE_DC:
    ret = mcpwm_get_tachometer_value(reset);
    break;

  case MOTOR_TYPE_FOC:
    ret = mcpwm_foc_get_tachometer_value(reset);
    break;

  default:
    break;
  }

  return DIR_MULT * ret;
}

int mc_interface_get_tachometer_value2(bool reset) {
  int ret = 0;
  switch (m_conf.motor_type) {
  case MOTOR_TYPE_BLDC:
  case MOTOR_TYPE_DC:
    ret = mcpwm_get_tachometer_value2(reset);
    break;

  case MOTOR_TYPE_FOC:
    ret = mcpwm_foc_get_tachometer_value2(reset);
    break;

  default:
    break;
  }
  return DIR_MULT2 * ret;
}

int mc_interface_get_tachometer_abs_value(bool reset) {
  int ret = 0;

  switch (m_conf.motor_type) {
  case MOTOR_TYPE_BLDC:
  case MOTOR_TYPE_DC:
    ret = mcpwm_get_tachometer_abs_value(reset);
    break;

  case MOTOR_TYPE_FOC:
    ret = mcpwm_foc_get_tachometer_abs_value(reset);
    break;

  default:
    break;
  }

  return ret;
}

int mc_interface_get_tachometer_abs_value2(bool reset) {
  int ret = 0;

  switch (m_conf.motor_type) {
  case MOTOR_TYPE_BLDC:
  case MOTOR_TYPE_DC:
    ret = mcpwm_get_tachometer_abs_value2(reset);
    break;

  case MOTOR_TYPE_FOC:
    ret = mcpwm_foc_get_tachometer_abs_value2(reset);
    break;

  default:
    break;
  }

  return ret;
}

float mc_interface_get_last_inj_adc_isr_duration(void) {
  float ret = 0.0;

  switch (m_conf.motor_type) {
  case MOTOR_TYPE_BLDC:
  case MOTOR_TYPE_DC:
    ret = mcpwm_get_last_inj_adc_isr_duration();
    break;

  case MOTOR_TYPE_FOC:
    ret = mcpwm_foc_get_last_inj_adc_isr_duration();
    break;

  default:
    break;
  }

  return ret;
}

float mc_interface_read_reset_avg_motor_current(void) {
  float res = m_motor_current_sum / m_motor_current_iterations;
  m_motor_current_sum = 0.0;
  m_motor_current_iterations = 0.0;
  return res;
}

float mc_interface_read_reset_avg_motor_current2(void) {
  float res = m_motor_current_sum2 / m_motor_current_iterations2;
  m_motor_current_sum2 = 0.0;
  m_motor_current_iterations2 = 0.0;
  return res;
}

float mc_interface_read_reset_avg_input_current(void) {
  float res = m_input_current_sum / m_input_current_iterations;
  m_input_current_sum = 0.0;
  m_input_current_iterations = 0.0;
  return res;
}

/**
 * Read and reset the average direct axis motor current. (FOC only)
 *
 * @return
 * The average D axis current.
 */
float mc_interface_read_reset_avg_id(void) {
  float res = m_motor_id_sum / m_motor_id_iterations;
  m_motor_id_sum = 0.0;
  m_motor_id_iterations = 0.0;
  return DIR_MULT * res; // TODO: DIR_MULT?
}

float mc_interface_read_reset_avg_id2(void) {
  float res = m_motor_id_sum2 / m_motor_id_iterations2;
  m_motor_id_sum2 = 0.0;
  m_motor_id_iterations2 = 0.0;
  return DIR_MULT2 * res; // TODO: DIR_MULT?
}

/**
 * Read and reset the average quadrature axis motor current. (FOC only)
 *
 * @return
 * The average Q axis current.
 */
float mc_interface_read_reset_avg_iq(void) {
  float res = m_motor_iq_sum / m_motor_iq_iterations;
  m_motor_iq_sum = 0.0;
  m_motor_iq_iterations = 0.0;
  return DIR_MULT * res;
}
float mc_interface_read_reset_avg_iq2(void) {
  float res = m_motor_iq_sum2 / m_motor_iq_iterations2;
  m_motor_iq_sum2 = 0.0;
  m_motor_iq_iterations2 = 0.0;
  return DIR_MULT2 * res;
}

float mc_interface_get_pid_pos_set(void) {
  return m_position_set1;
}

float mc_interface_get_pid_pos_now(void) {

  float ret = 0.0;

  switch (m_conf.motor_type) {
  case MOTOR_TYPE_BLDC:
  case MOTOR_TYPE_DC:
    ret = encoder_read_deg();
    break;

  case MOTOR_TYPE_FOC:
    ret = mcpwm_foc_get_pid_pos_now();
    break;

  default:
    break;
  }

  return DIR_MULT * ret;
}

float mc_interface_get_last_sample_adc_isr_duration(void) {
  return m_last_adc_duration_sample;
}

void mc_interface_sample_print_data(debug_sampling_mode mode, uint16_t len, uint8_t decimation) {
  if (len > ADC_SAMPLE_MAX_LEN) {
    len = ADC_SAMPLE_MAX_LEN;
  }

  if (mode == DEBUG_SAMPLING_SEND_LAST_SAMPLES) {
    chEvtSignal(sample_send_tp, (eventmask_t)1);
  }
  else {
    m_sample_trigger = -1;
    m_sample_now = 0;
    m_sample_len = len;
    m_sample_int = decimation;
    m_sample_mode = mode;
  }
}

float mc_interface_get_voltage(void){
  return v_in_last;
}

/**
 * Get filtered MOSFET temperature. The temperature is pre-calculated, so this
 * functions is fast.
 *
 * @return
 * The filtered MOSFET temperature.
 */
float mc_interface_temp_fet_filtered(void) {
  return m_temp_fet;
}

float mc_interface_temp_fet_filtered2(void) {
  return m_temp_fet2;
}

void mc_interface_measure_flux_linkage_start(void) {
  measureFluxLinkage = true;
  measurementDone1 = false;
  measurementDone2 = false;
}
bool mc_interface_flux_linkage_measurements_done(void) {
  return (measurementDone1 && measurementDone2);
}
void mc_interface_measure_flux_linkage_finish(float *linkage1, float *linkage2, bool *completed1, bool *completed2, bool *dir1, bool *dir2) {
  measureFluxLinkage = false;
  *dir1 = dir1_measured;
  *dir2 = dir2_measured;
  static int len = ADC_SAMPLE_MAX_LEN;
  static long int avg1;
  static long int avg2;
  static float linkage_tmp = 0;
  avg1 = 0;
  avg2 = 0;
  for (int i = 0; i < len; i++) {
    avg1 += m_va1_samples[i];
    avg2 += m_vb1_samples[i];
  }
  avg1 /= len;
  avg2 /= len;
  for (int i = 0; i < len; i++) {
    m_va1_samples[i] -= avg1;
    m_vb1_samples[i] -= avg2;
  }
  for (int i = 0; i < len; i++) {
    linkage_tmp += sqrtf(SQ(m_va1_samples[i]) + SQ(m_vb1_samples[i]));
  }
  linkage_tmp = (linkage_tmp / ((float)len *  mc_interface_get_sampling_frequency_now() * 4096.0) * V_REG) * ((VIN_R1 + VIN_R2) / VIN_R2);
  *linkage1 = linkage_tmp;
  linkage_tmp = 0;
  avg1 = 0;
  avg2 = 0;
  for (int i = 0; i < len; i++) {
    avg1 += m_va2_samples[i];
    avg2 += m_vb2_samples[i];
  }
  avg1 /= len;
  avg2 /= len;
  for (int i = 0; i < len; i++) {
    m_va2_samples[i] -= avg1;
    m_vb2_samples[i] -= avg2;
  }
  for (int i = 0; i < len; i++) {
    linkage_tmp += sqrtf(SQ(m_va2_samples[i]) + SQ(m_vb2_samples[i]));
  }
  linkage_tmp = (linkage_tmp / ((float)len * mc_interface_get_sampling_frequency_now() * 4096.0) * V_REG) * ((VIN_R1 + VIN_R2) / VIN_R2);
  *linkage2 = linkage_tmp;
  *completed1 = measurementDone1;
  *completed2 = measurementDone2;
  measurementDone1 = false;
  measurementDone2 = false;
}
/**
 * Get filtered motor temperature. The temperature is pre-calculated, so this
 * functions is fast.
 *
 * @return
 * The filtered motor temperature.
 */
float mc_interface_temp_motor_filtered(void) {
  return m_temp_motor;
}
float mc_interface_temp_motor_filtered2(void) {
  return m_temp_motor2;
}

unsigned int mc_interface_get_seconds_inactive(void) {
  return msec_inactive/1000;
}

void mc_interface_reset_seconds_inactive(void) {
  msec_inactive = 0;
}

// MC implementation functions

/**
 * A helper function that should be called before sending commands to control
 * the motor. If the state is detecting, the detection will be stopped.
 *
 * @return
 * The amount if milliseconds left until user commands are allowed again.
 *
 */
int mc_interface_try_input(void) {
  // TODO: Remove this later
  if (mc_interface_get_state() == MC_STATE_DETECTING) {
    mcpwm_stop_pwm();
    m_ignore_iterations = MCPWM_DETECT_STOP_TIME;
  }

  int retval = m_ignore_iterations;

  if (!m_ignore_iterations && m_lock_enabled) {
    if (!m_lock_override_once) {
      retval = 1;
    }
    else {
      m_lock_override_once = false;
    }
  }

  switch (m_conf.motor_type) {
  case MOTOR_TYPE_BLDC:
  case MOTOR_TYPE_DC:
    if (!mcpwm_init_done()) {
      retval = 1;
    }
    break;

  case MOTOR_TYPE_FOC:
    if (!mcpwm_foc_init_done()) {
      retval = 1;
    }
    break;

  default:
    break;
  }

  return retval;
}

void mc_interface_fault_stop(mc_fault_code fault) {
  if (m_fault_now == fault) {
    m_ignore_iterations = m_conf.m_fault_stop_time_ms;
    return;
  }

  if (mc_interface_dccal_done() && m_fault_now == FAULT_CODE_NONE) {
    // Sent to terminal fault logger so that all faults and their conditions
    // can be printed for debugging.
    utils_sys_lock_cnt();
    volatile int val_samp = TIM3->CCR1;
    volatile int current_samp = TIM1->CCR4;
    volatile int tim_top = TIM1->ARR;
    utils_sys_unlock_cnt();
    temperature_values motor_temps;
    motor_temps = mcpwm_foc_get_temperature();

    fault_data fdata;
    fdata.fault = fault;
    fdata.current = mc_interface_get_tot_current();
    fdata.current_filtered = mc_interface_get_tot_current_filtered();
    fdata.voltage = mcpwm_foc_get_v_bus();
    fdata.duty = mc_interface_get_duty_cycle_now();
    fdata.rpm = mc_interface_get_rpm();
    fdata.tacho = mc_interface_get_tachometer_value(false);
    fdata.cycles_running = m_cycles_running;
    fdata.tim_val_samp = val_samp;
    fdata.tim_current_samp = current_samp;
    fdata.tim_top = tim_top;
    fdata.temperature = motor_temps.mosfet_temp_1;
    fdata.current2 = mc_interface_get_tot_current2();
    fdata.current_filtered2 = mc_interface_get_tot_current_filtered2();
    fdata.duty2 = mc_interface_get_duty_cycle_now2();
    fdata.rpm2 = mc_interface_get_rpm2();
    fdata.tacho2 = mc_interface_get_tachometer_value2(false);
    fdata.temperature2 = motor_temps.mosfet_temp_2;
    if (m_conf.motor_type == MOTOR_TYPE_FOC) {
      // TODO: Make this more general
      fdata.abs_current_filt = mcpwm_foc_get_abs_motor_current_filtered();
      fdata.abs_current_filt2 = mcpwm_foc_get_abs_motor_current_filtered2();
      fdata.iq = mcpwm_foc_get_iq();
      fdata.iq2 = mcpwm_foc_get_iq2();
      fdata.id = mcpwm_foc_get_id();
      fdata.id2 = mcpwm_foc_get_id2();
    }


#ifdef HW_HAS_DRV8301
    if (fault == FAULT_CODE_DRV || fault == FAULT_CODE_DRV2) {
      fdata.drv8301_faults = drv8301_read_faults();
      fdata.drv8301_faults2 = drv8301_read_faults2();
    }
#endif
    terminal_add_fault_data(&fdata);
  }

  m_ignore_iterations = m_conf.m_fault_stop_time_ms;

  switch (m_conf.motor_type) {
  case MOTOR_TYPE_BLDC:
  case MOTOR_TYPE_DC:
    mcpwm_stop_pwm();
    mcpwm_stop_pwm2();
    break;

  case MOTOR_TYPE_FOC:
    mcpwm_foc_stop_pwm();
    mcpwm_foc_stop_pwm2();
    break;

  default:
    break;
  }

  m_fault_now = fault;
}

void mc_interface_mc_timer_isr(void) {
  ledpwm_update_pwm(); // LED PWM Driver update



  const float input_voltage = GET_INPUT_VOLTAGE();

  // Check for faults that should stop the motor
  static int wrong_voltage_iterations = 0;
  if (input_voltage < m_conf.l_min_vin || input_voltage > m_conf.l_max_vin) {
    wrong_voltage_iterations++;

    if ((wrong_voltage_iterations >= 8)) {
      mc_interface_fault_stop(input_voltage < m_conf.l_min_vin ? FAULT_CODE_UNDER_VOLTAGE : FAULT_CODE_OVER_VOLTAGE);
    }
  }
  else {
    wrong_voltage_iterations = 0;
  }

  if (mc_interface_get_state() == MC_STATE_RUNNING) {
    m_cycles_running++;
  }
  else {
    m_cycles_running = 0;
  }

  if (pwn_done_func) {
    pwn_done_func();
  }

  const float current = mc_interface_get_tot_current_filtered();
  const float current2 = mc_interface_get_tot_current_filtered2();
  const float current_in = mc_interface_get_tot_current_in_filtered();
  m_motor_current_sum += current;
  m_motor_current_sum2 += current2;
  m_input_current_sum += current_in;
  m_motor_current_iterations++;
  m_motor_current_iterations2++;
  m_input_current_iterations++;

  m_motor_id_sum += mcpwm_foc_get_id();
  m_motor_iq_sum += mcpwm_foc_get_iq();
  m_motor_id_sum2 += mcpwm_foc_get_id2();
  m_motor_iq_sum2 += mcpwm_foc_get_iq2();
  m_motor_id_iterations++;
  m_motor_iq_iterations++;
  m_motor_id_iterations2++;
  m_motor_iq_iterations2++;
  float abs_current = mc_interface_get_tot_current();
  float abs_current2 = mc_interface_get_tot_current2();
  float abs_current_filtered = current;
  float abs_current_filtered2 = current2;
  if (m_conf.motor_type == MOTOR_TYPE_FOC) {
    // TODO: Make this more general
    abs_current = mcpwm_foc_get_abs_motor_current();
    abs_current_filtered = mcpwm_foc_get_abs_motor_current_filtered();
    abs_current2 = mcpwm_foc_get_abs_motor_current2();
    abs_current_filtered2 = mcpwm_foc_get_abs_motor_current_filtered2();
  }

  // Current fault code
  static int abs_over_current_ctr_1 = 0;
  static int abs_over_current_ctr_2 = 0;
  if (m_conf.l_slow_abs_current) {
    if (fabsf(abs_current_filtered) > m_conf.l_abs_current_max) {
      abs_over_current_ctr_1 ++;
    }else{
      abs_over_current_ctr_1 = 0;
    }
    if(abs_over_current_ctr_1 > 250){
      mc_interface_fault_stop(FAULT_CODE_ABS_OVER_CURRENT);
    }

    if (fabsf(abs_current_filtered2) > m_conf2.l_abs_current_max) {
      abs_over_current_ctr_2 ++;
    }else{
      abs_over_current_ctr_2 = 0;
    }
    if(abs_over_current_ctr_2 > 250){
      mc_interface_fault_stop(FAULT_CODE_ABS_OVER_CURRENT2);
    }
  }
  else {
    if (fabsf(abs_current) > m_conf.l_abs_current_max) {
      mc_interface_fault_stop(FAULT_CODE_ABS_OVER_CURRENT);
    }
    if (fabsf(abs_current2) > m_conf2.l_abs_current_max) {
      mc_interface_fault_stop(FAULT_CODE_ABS_OVER_CURRENT2);
    }
  }

  // DRV fault code
  if (IS_DRV_FAULT2()) {
    mc_interface_fault_stop(FAULT_CODE_DRV2);
  }

  if (IS_DRV_FAULT()) {
    mc_interface_fault_stop(FAULT_CODE_DRV);
  }



  // Watt and ah counters
  const float f_samp = mc_interface_get_sampling_frequency_now();
  if (fabsf(current) > 1.0 || fabsf(current2) > 1.0) {
    // Some extra filtering
    static float curr_diff_sum = 0.0;
    static float curr_diff_samples = 0;

    curr_diff_sum += current_in  / f_samp;
    curr_diff_samples += 1.0 / f_samp;

    if (curr_diff_samples >= 0.01) {
      if (curr_diff_sum > 0.0) {
        m_amp_seconds += curr_diff_sum;
        m_watt_seconds += curr_diff_sum * input_voltage;
      }
      else {
        m_amp_seconds_charged -= curr_diff_sum;
        m_watt_seconds_charged -= curr_diff_sum * input_voltage;
      }

      curr_diff_samples = 0.0;
      curr_diff_sum = 0.0;
    }
  }

  bool sample = false;
  if (measureFluxLinkage) {
    static int a = 0;
    static int b = 0;
    static bool measureNow1 = false;
    static bool measureNow2 = false;
    if ((measureNow1 == false) && (fabsf(((ADC_V_L1 )/ 4095.0 * V_REG) * ((VIN_R1 + VIN_R2) / VIN_R2)) > 2.5) && (measurementDone1 == false)){
      dir1_measured = (DIR_MULT* mc_interface_get_duty_cycle_now()) > 0;
      measureNow1 = true;
      a = 0;
    }
    if ((measureNow2 == false) && (fabsf(((ADC_V_L4 )/ 4095.0 * V_REG) * ((VIN_R1 + VIN_R2) / VIN_R2)) > 2.5) && (measurementDone2 == false)){
      dir2_measured = (DIR_MULT2* mc_interface_get_duty_cycle_now2()) > 0;
      measureNow2 = true;
      b = 0;
    }
    if (measureNow1) {

      if (a > 0) {
        m_va1_samples[a] = 2 * ADC_V_L1/3- ADC_V_L3/3 - ADC_V_L2/3 + m_va1_samples[a-1];
        m_vb1_samples[a] = ONE_BY_SQRT3 * ADC_V_L3- ONE_BY_SQRT3 * ADC_V_L2 + m_vb1_samples[a-1];
      }
      else {
        m_va1_samples[a] = 2*ADC_V_L1/3- ADC_V_L3/3 - ADC_V_L2/3;
        m_vb1_samples[a] = ONE_BY_SQRT3 * ADC_V_L3- ONE_BY_SQRT3 * ADC_V_L2;
      }
      a++;
      if(a>=ADC_SAMPLE_MAX_LEN) {
        measureNow1 = false;
        measurementDone1 = true;
      }
    }
    if (measureNow2) {
      if (b > 0) {
        m_va2_samples[b] = 2 * ADC_V_L4/3- ADC_V_L6/3 - ADC_V_L5/3 + m_va2_samples[b-1];
        m_vb2_samples[b] = ONE_BY_SQRT3 * ADC_V_L6- ONE_BY_SQRT3 * ADC_V_L5 + m_vb2_samples[b-1];
      }
      else {
        m_va2_samples[b] = 2*ADC_V_L4/3- ADC_V_L6/3 - ADC_V_L5/3;
        m_vb2_samples[b] = ONE_BY_SQRT3 * ADC_V_L6- ONE_BY_SQRT3 * ADC_V_L5;
      }
      b++;
      if(b>=ADC_SAMPLE_MAX_LEN) {
        measureNow2 = false;
        measurementDone2 = true;
      }
    }
  }
  switch (m_sample_mode) {
  case DEBUG_SAMPLING_NOW:
    if (m_sample_now == m_sample_len) {
      m_sample_mode = DEBUG_SAMPLING_OFF;
      m_sample_mode_last = DEBUG_SAMPLING_NOW;
      chSysLockFromISR();
      chEvtSignalI(sample_send_tp, (eventmask_t)1);
      chSysUnlockFromISR();
    }
    else {
      sample = true;
    }
    break;

  case DEBUG_SAMPLING_START:
    if (mc_interface_get_state() == MC_STATE_RUNNING || m_sample_now > 0) {
      sample = true;
    }

    if (m_sample_now == m_sample_len) {
      m_sample_mode_last = m_sample_mode;
      m_sample_mode = DEBUG_SAMPLING_OFF;
      chSysLockFromISR();
      chEvtSignalI(sample_send_tp, (eventmask_t)1);
      chSysUnlockFromISR();
    }
    break;

  case DEBUG_SAMPLING_TRIGGER_START:
  case DEBUG_SAMPLING_TRIGGER_START_NOSEND: {
    sample = true;

    int sample_last = -1;
    if (m_sample_trigger >= 0) {
      sample_last = m_sample_trigger - m_sample_len;
      if (sample_last < 0) {
        sample_last += ADC_SAMPLE_MAX_LEN;
      }
    }

    if (m_sample_now == sample_last) {
      m_sample_mode_last = m_sample_mode;
      sample = false;

      if (m_sample_mode == DEBUG_SAMPLING_TRIGGER_START) {
        chSysLockFromISR();
        chEvtSignalI(sample_send_tp, (eventmask_t)1);
        chSysUnlockFromISR();
      }

      m_sample_mode = DEBUG_SAMPLING_OFF;
    }

    if (mc_interface_get_state() == MC_STATE_RUNNING && m_sample_trigger < 0) {
      m_sample_trigger = m_sample_now;
    }
  }
  break;

  case DEBUG_SAMPLING_TRIGGER_FAULT:
  case DEBUG_SAMPLING_TRIGGER_FAULT_NOSEND: {
    sample = true;

    int sample_last = -1;
    if (m_sample_trigger >= 0) {
      sample_last = m_sample_trigger - m_sample_len;
      if (sample_last < 0) {
        sample_last += ADC_SAMPLE_MAX_LEN;
      }
    }

    if (m_sample_now == sample_last) {
      m_sample_mode_last = m_sample_mode;
      sample = false;

      if (m_sample_mode == DEBUG_SAMPLING_TRIGGER_FAULT) {
        chSysLockFromISR();
        chEvtSignalI(sample_send_tp, (eventmask_t)1);
        chSysUnlockFromISR();
      }

      m_sample_mode = DEBUG_SAMPLING_OFF;
    }

    if (m_fault_now != FAULT_CODE_NONE && m_sample_trigger < 0) {
      m_sample_trigger = m_sample_now;
    }
  }
  break;

  default:
    break;
  }
  if (sample) {
    static int a = 0;
    a++;

    if (a >= m_sample_int) {
      a = 0;

      if (m_sample_now >= ADC_SAMPLE_MAX_LEN) {
        m_sample_now = 0;
      }

      int16_t zero;
      if (m_conf.motor_type == MOTOR_TYPE_FOC) {
        zero = (ADC_V_L1+ ADC_V_L2 + ADC_V_L3) / 3;
        m_phase_samples[m_sample_now] = (uint8_t)(mcpwm_foc_get_phase() / 360.0 * 250.0);
        //              m_phase_samples[m_sample_now] = (uint8_t)(mcpwm_foc_get_phase_observer() / 360.0 * 250.0);
        //              float ang = utils_angle_difference(mcpwm_foc_get_phase_observer(), mcpwm_foc_get_phase_encoder()) + 180.0;
        //              m_phase_samples[m_sample_now] = (uint8_t)(ang / 360.0 * 250.0);
      }
      else {
        zero = mcpwm_vzero;
        m_phase_samples[m_sample_now] = 0;
      }

      if (mc_interface_get_state() == MC_STATE_DETECTING) {
        m_curr0_samples[m_sample_now] = (int16_t)mcpwm_detect_currents[mcpwm_get_comm_step() - 1];
        m_curr1_samples[m_sample_now] = (int16_t)mcpwm_detect_currents_diff[mcpwm_get_comm_step() - 1];

        m_ph1_samples[m_sample_now] = (int16_t)mcpwm_detect_voltages[0];
        m_ph2_samples[m_sample_now] = (int16_t)mcpwm_detect_voltages[1];
        m_ph3_samples[m_sample_now] = (int16_t)mcpwm_detect_voltages[2];
      }
      else {
        m_curr0_samples[m_sample_now] = ADC_curr_norm_value[0];
        m_curr1_samples[m_sample_now] = ADC_curr_norm_value[1];
        //float Va = ADC_VOLTS(ADC_IND_SENS1) * ((VIN_R1 + VIN_R2) / VIN_R2);
        //float Vb = ADC_VOLTS(ADC_IND_SENS3) * ((VIN_R1 + VIN_R2) / VIN_R2);
        //float Vc = ADC_VOLTS(ADC_IND_SENS2) * ((VIN_R1 + VIN_R2) / VIN_R2);
        //m_motor_state.v_alpha = (2.0 / 3.0) * Va - (1.0 / 3.0) * Vb	- (1.0 / 3.0) * Vc;
        //m_motor_state.v_beta = ONE_BY_SQRT3 * Vb - ONE_BY_SQRT3 * Vc;
        if (m_sample_now > 0) {
          m_ph1_samples[m_sample_now] = 2 * ADC_V_L4/3- ADC_V_L6/3 - ADC_V_L5/3 + m_ph1_samples[m_sample_now-1];
          m_ph2_samples[m_sample_now] = ONE_BY_SQRT3 * ADC_V_L6- ONE_BY_SQRT3 * ADC_V_L5 + m_ph2_samples[m_sample_now-1];
        }
        else {
          m_ph1_samples[m_sample_now] = 2*ADC_V_L4/3- ADC_V_L6/3 - ADC_V_L5/3;
          m_ph2_samples[m_sample_now] = ONE_BY_SQRT3 * ADC_V_L6- ONE_BY_SQRT3 * ADC_V_L5;
        }

        m_ph3_samples[m_sample_now] = 0;				//ADC_V_L3- zero;
      }

      m_vzero_samples[m_sample_now] = zero;
      m_curr_fir_samples[m_sample_now] = (int16_t)(mc_interface_get_tot_current() * (8.0 / FAC_CURRENT));
      m_f_sw_samples[m_sample_now] = (int16_t)(f_samp / 10.0);
      m_status_samples[m_sample_now] = mcpwm_get_comm_step() | (mcpwm_read_hall_phase() << 3);

      m_sample_now++;

      m_last_adc_duration_sample = mc_interface_get_last_sample_adc_isr_duration();
    }
  }
}

void mc_interface_adc_inj_int_handler(void) {
  switch (m_conf.motor_type) {
  case MOTOR_TYPE_BLDC:
  case MOTOR_TYPE_DC:
    mcpwm_adc_inj_int_handler();
    break;

  case MOTOR_TYPE_FOC:
    break;

  default:
    break;
  }
}

/**
 * Update the override limits for a configuration based on MOSFET temperature etc.
 *
 * @param conf
 * The configaration to update.
 */
static void update_override_limits(volatile mc_configuration *conf, volatile mc_configuration *conf2) {
  const float v_in = 0.1*GET_INPUT_VOLTAGE() + 0.9*v_in_last;
  v_in_last = v_in;
  const float rpm_now = mc_interface_get_rpm();
  const float rpm_now2 = mc_interface_get_rpm2();
  //update inactivity timer
  if((fabsf(rpm_now)<250.0) && (fabsf(rpm_now2)<250.0)){
    msec_inactive++;
  }else{
    msec_inactive = 0;
  }

  temperature_values motor_temps;
  motor_temps = mcpwm_foc_get_temperature();
  UTILS_LP_FAST(m_temp_fet,motor_temps.mosfet_temp_1 , 0.1);
  UTILS_LP_FAST(m_temp_fet2,motor_temps.mosfet_temp_2 , 0.1);
  UTILS_LP_FAST(m_temp_motor, motor_temps.motor_temp_1, 0.1);
  UTILS_LP_FAST(m_temp_motor2, motor_temps.motor_temp_2, 0.1);
  // Temperature MOSFET
  float lo_max_mos = conf->l_current_max;
  float lo_min_mos = conf->l_current_min;
  float lo_max_mos2 = conf2->l_current_max;
  float lo_min_mos2 = conf2->l_current_min;
  if (m_temp_fet < conf->l_temp_fet_start) {
    // keep values
  }
  else if (m_temp_fet > conf->l_temp_fet_end) {
    lo_min_mos = 0.0;
    lo_max_mos = 0.0;
    mc_interface_fault_stop(FAULT_CODE_OVER_TEMP_FET);
  }
  else {
    float maxc = fabsf(conf->l_current_max);
    if (fabsf(conf->l_current_min) > maxc) {
      maxc = fabsf(conf->l_current_min);
    }

    maxc = utils_map(m_temp_fet, conf->l_temp_fet_start, conf->l_temp_fet_end, maxc, 0.0);

    if (fabsf(conf->l_current_max) > maxc) {
      lo_max_mos = SIGN(conf->l_current_max) * maxc;
    }

    if (fabsf(conf->l_current_min) > maxc) {
      lo_min_mos = SIGN(conf->l_current_min) * maxc;
    }
  }

  if (m_temp_fet2 < conf2->l_temp_fet_start) {
    //keep values
  }
  else if (m_temp_fet2 > conf2->l_temp_fet_end) {
    lo_min_mos2 = 0.0;
    lo_max_mos2 = 0.0;
    mc_interface_fault_stop(FAULT_CODE_OVER_TEMP_FET2);
  }
  else {
    float maxc2 = fabsf(conf2->l_current_max);
    if (fabsf(conf2->l_current_min) > maxc2) {
      maxc2 = fabsf(conf2->l_current_min);
    }

    maxc2 = utils_map(m_temp_fet2, conf2->l_temp_fet_start, conf2->l_temp_fet_end, maxc2, 0.0);

    if (fabsf(conf2->l_current_max) > maxc2) {
      lo_max_mos2 = SIGN(conf2->l_current_max) * maxc2;
    }

    if (fabsf(conf2->l_current_min) > maxc2) {
      lo_min_mos2 = SIGN(conf2->l_current_min) * maxc2;
    }
  }

  // Temperature MOTOR
  float lo_max_mot = conf->l_current_max;
  float lo_min_mot = conf->l_current_min;
  float lo_max_mot2 = conf2->l_current_max;
  float lo_min_mot2 = conf2->l_current_min;
  if ((m_temp_motor < conf->l_temp_motor_start) || (!conf->m_motor_temp_throttle_enable)) {
    // keep values
  }
  else if (m_temp_motor > conf->l_temp_motor_end) {
    lo_min_mot = 0.0;
    lo_max_mot = 0.0;
    mc_interface_fault_stop(FAULT_CODE_OVER_TEMP_MOTOR);
  }
  else {
    float maxc = fabsf(conf->l_current_max);
    if (fabsf(conf->l_current_min) > maxc) {
      maxc = fabsf(conf->l_current_min);
    }

    maxc = utils_map(m_temp_motor, conf->l_temp_motor_start, conf->l_temp_motor_end, maxc, 0.0);

    if (fabsf(conf->l_current_max) > maxc) {
      lo_max_mot = SIGN(conf->l_current_max) * maxc;
    }

    if (fabsf(conf->l_current_min) > maxc) {
      lo_min_mot = SIGN(conf->l_current_min) * maxc;
    }
  }

  if ((m_temp_motor2 < conf2->l_temp_motor_start) ||  (!conf2->m_motor_temp_throttle_enable)) {
    // keep values
  }
  else if (m_temp_motor2 > conf2->l_temp_motor_end) {
    lo_min_mot2 = 0.0;
    lo_max_mot2 = 0.0;
    mc_interface_fault_stop(FAULT_CODE_OVER_TEMP_MOTOR2);
  }
  else {
    float maxc = fabsf(conf2->l_current_max);
    if (fabsf(conf2->l_current_min) > maxc) {
      maxc = fabsf(conf2->l_current_min);
    }

    maxc = utils_map(m_temp_motor2, conf2->l_temp_motor_start, conf2->l_temp_motor_end, maxc, 0.0);

    if (fabsf(conf->l_current_max) > maxc) {
      lo_max_mot2 = SIGN(conf2->l_current_max) * maxc;
    }

    if (fabsf(conf->l_current_min) > maxc) {
      lo_min_mot2 = SIGN(conf2->l_current_min) * maxc;
    }
  }

  // Decreased temperatures during acceleration
  // in order to still have braking torque available
  const float temp_fet_accel_start = utils_map(conf->l_temp_accel_dec, 0.0, 1.0, conf->l_temp_fet_start, 25.0);
  const float temp_fet_accel_end = utils_map(conf->l_temp_accel_dec, 0.0, 1.0, conf->l_temp_fet_end, 25.0);
  const float temp_motor_accel_start = utils_map(conf->l_temp_accel_dec, 0.0, 1.0, conf->l_temp_motor_start, 25.0);
  const float temp_motor_accel_end = utils_map(conf->l_temp_accel_dec, 0.0, 1.0, conf->l_temp_motor_end, 25.0);

  const float temp_fet_accel_start2 = utils_map(conf2->l_temp_accel_dec, 0.0, 1.0, conf2->l_temp_fet_start, 25.0);
  const float temp_fet_accel_end2 = utils_map(conf2->l_temp_accel_dec, 0.0, 1.0, conf2->l_temp_fet_end, 25.0);
  const float temp_motor_accel_start2 = utils_map(conf2->l_temp_accel_dec, 0.0, 1.0, conf2->l_temp_motor_start, 25.0);
  const float temp_motor_accel_end2 = utils_map(conf2->l_temp_accel_dec, 0.0, 1.0, conf2->l_temp_motor_end, 25.0);

  float lo_fet_temp_accel = 0.0;
  if (m_temp_fet < temp_fet_accel_start) {
    lo_fet_temp_accel = conf->l_current_max;
  }
  else if (m_temp_fet > temp_fet_accel_end) {
    lo_fet_temp_accel = 0.0;
  }
  else {
    lo_fet_temp_accel = utils_map(m_temp_fet, temp_fet_accel_start, temp_fet_accel_end, conf->l_current_max, 0.0);
  }

  float lo_motor_temp_accel = 0.0;
  if (m_temp_motor < temp_motor_accel_start || (!conf->m_motor_temp_throttle_enable)) {
    lo_motor_temp_accel = conf->l_current_max;
  }
  else if (m_temp_motor > temp_motor_accel_end) {
    lo_motor_temp_accel = 0.0;
  }
  else {
    lo_motor_temp_accel = utils_map(m_temp_motor, temp_motor_accel_start, temp_motor_accel_end, conf->l_current_max, 0.0);
  }

  float lo_fet_temp_accel2 = 0.0;
  if (m_temp_fet2 < temp_fet_accel_start2) {
    lo_fet_temp_accel2 = conf2->l_current_max;
  }
  else if (m_temp_fet2 > temp_fet_accel_end2) {
    lo_fet_temp_accel2 = 0.0;
  }
  else {
    lo_fet_temp_accel2 = utils_map(m_temp_fet2, temp_fet_accel_start2, temp_fet_accel_end2, conf2->l_current_max, 0.0);
  }

  float lo_motor_temp_accel2 = 0.0;
  if (m_temp_motor2 < temp_motor_accel_start2 || (!conf2->m_motor_temp_throttle_enable)) {
    lo_motor_temp_accel2 = conf2->l_current_max;
  }
  else if (m_temp_motor2 > temp_motor_accel_end2) {
    lo_motor_temp_accel2 = 0.0;
  }
  else {
    lo_motor_temp_accel2 = utils_map(m_temp_motor2, temp_motor_accel_start2, temp_motor_accel_end2, conf2->l_current_max, 0.0);
  }


  // RPM max
  float lo_max_rpm = 0.0;
  const float rpm_pos_cut_start = conf->l_max_erpm * conf->l_erpm_start;
  const float rpm_pos_cut_end = conf->l_max_erpm;
  if (rpm_now < rpm_pos_cut_start) {
    lo_max_rpm = conf->l_current_max;
  }
  else if (rpm_now > rpm_pos_cut_end) {
    lo_max_rpm = 0.0;
  }
  else {
    lo_max_rpm = utils_map(rpm_now, rpm_pos_cut_start, rpm_pos_cut_end, conf->l_current_max, 0.0);
  }

  // RPM min
  float lo_min_rpm = 0.0;
  const float rpm_neg_cut_start = conf->l_min_erpm * conf->l_erpm_start;
  const float rpm_neg_cut_end = conf->l_min_erpm;
  if (rpm_now > rpm_neg_cut_start) {
    lo_min_rpm = conf->l_current_max;
  }
  else if (rpm_now < rpm_neg_cut_end) {
    lo_min_rpm = 0.0;
  }
  else {
    lo_min_rpm = utils_map(rpm_now, rpm_neg_cut_start, rpm_neg_cut_end, conf->l_current_max, 0.0);
  }

  // RPM max
  float lo_max_rpm2 = 0.0;
  const float rpm_pos_cut_start2 = conf2->l_max_erpm * conf2->l_erpm_start;
  const float rpm_pos_cut_end2 = conf2->l_max_erpm;
  if (rpm_now2 < rpm_pos_cut_start2) {
    lo_max_rpm2 = conf2->l_current_max;
  }
  else if (rpm_now2 > rpm_pos_cut_end2) {
    lo_max_rpm2 = 0.0;
  }
  else {
    lo_max_rpm2 = utils_map(rpm_now2, rpm_pos_cut_start2, rpm_pos_cut_end2, conf2->l_current_max, 0.0);
  }

  // RPM min
  float lo_min_rpm2 = 0.0;
  const float rpm_neg_cut_start2 = conf2->l_min_erpm * conf2->l_erpm_start;
  const float rpm_neg_cut_end2 = conf2->l_min_erpm;
  if (rpm_now2 > rpm_neg_cut_start2) {
    lo_min_rpm2 = conf2->l_current_max;
  }
  else if (rpm_now2 < rpm_neg_cut_end2) {
    lo_min_rpm2 = 0.0;
  }
  else {
    lo_min_rpm2 = utils_map(rpm_now2, rpm_neg_cut_start2, rpm_neg_cut_end2, conf2->l_current_max, 0.0);
  }

  float lo_max = utils_min_abs(lo_max_mos, lo_max_mot);
  float lo_min = utils_min_abs(lo_min_mos, lo_min_mot);

  lo_max = utils_min_abs(lo_max, lo_max_rpm);
  lo_max = utils_min_abs(lo_max, lo_min_rpm);
  lo_max = utils_min_abs(lo_max, lo_fet_temp_accel);
  lo_max = utils_min_abs(lo_max, lo_motor_temp_accel);

  if (lo_max < conf->cc_min_current) {
    lo_max = conf->cc_min_current;
  }

  if (lo_min > -conf->cc_min_current) {
    lo_min = -conf->cc_min_current;
  }

  conf->lo_current_max = lo_max;
  conf->lo_current_min = lo_min;

  float lo_max2 = utils_min_abs(lo_max_mos2, lo_max_mot2);
  float lo_min2 = utils_min_abs(lo_min_mos2, lo_min_mot2);

  lo_max2 = utils_min_abs(lo_max2, lo_max_rpm2);
  lo_max2 = utils_min_abs(lo_max2, lo_min_rpm2);
  lo_max2 = utils_min_abs(lo_max2, lo_fet_temp_accel2);
  lo_max2 = utils_min_abs(lo_max2, lo_motor_temp_accel2);

  if (lo_max2 < conf2->cc_min_current) {
    lo_max2 = conf2->cc_min_current;
  }

  if (lo_min2 > -conf2->cc_min_current) {
    lo_min2 = -conf2->cc_min_current;
  }

  conf2->lo_current_max = lo_max2;
  conf2->lo_current_min = lo_min2;

  // Battery cutoff
  float lo_in_max_batt = 0.0;
  float lo_in_max_batt2 = 0.0;
  if (v_in > conf->l_battery_cut_start) {
    lo_in_max_batt = conf->l_in_current_max;
    lo_in_max_batt2 = conf2->l_in_current_max;
  }
  else if (v_in < conf->l_battery_cut_end) {
    lo_in_max_batt = 0.0;
    lo_in_max_batt2 = 0.0;
  }
  else {
    lo_in_max_batt = utils_map(v_in, conf->l_battery_cut_start, conf->l_battery_cut_end, conf->l_in_current_max, 0.0);
    lo_in_max_batt2 = utils_map(v_in, conf2->l_battery_cut_start, conf2->l_battery_cut_end, conf2->l_in_current_max, 0.0);
  }

  // Wattage limits
  const float lo_in_max_watt = conf->l_watt_max / v_in;
  const float lo_in_min_watt = conf->l_watt_min / v_in;
  const float lo_in_max_watt2 = conf2->l_watt_max / v_in;
  const float lo_in_min_watt2 = conf2->l_watt_min / v_in;

  const float lo_in_max = utils_min_abs(lo_in_max_watt, lo_in_max_batt);
  const float lo_in_min = lo_in_min_watt;

  const float lo_in_max2 = utils_min_abs(lo_in_max_watt2, lo_in_max_batt2);
  const float lo_in_min2 = lo_in_min_watt2;

  conf->lo_in_current_max = utils_min_abs(conf->l_in_current_max, lo_in_max);
  conf->lo_in_current_min = utils_min_abs(conf->l_in_current_min, lo_in_min);

  conf2->lo_in_current_max = utils_min_abs(conf2->l_in_current_max, lo_in_max2);
  conf2->lo_in_current_min = utils_min_abs(conf2->l_in_current_min, lo_in_min2);

  // Maximum current right now
  //  float duty_abs = fabsf(mc_interface_get_duty_cycle_now());
  //
  //  // TODO: This is not an elegant solution.
  //  if (m_conf.motor_type == MOTOR_TYPE_FOC) {
  //      duty_abs *= SQRT3_BY_2;
  //  }
  //
  //  if (duty_abs > 0.001) {
  //      conf->lo_current_motor_max_now = utils_min_abs(conf->lo_current_max, conf->lo_in_current_max / duty_abs);
  //      conf->lo_current_motor_min_now = utils_min_abs(conf->lo_current_min, conf->lo_in_current_min / duty_abs);
  //  } else {
  //      conf->lo_current_motor_max_now = conf->lo_current_max;
  //      conf->lo_current_motor_min_now = conf->lo_current_min;
  //  }

  // Note: The above code should work, but many people have reported issues with it. Leaving it
  // disabled for now until I have done more investigation.
  conf->lo_current_motor_max_now = conf->lo_current_max;
  conf->lo_current_motor_min_now = conf->lo_current_min;

  conf2->lo_current_motor_max_now = conf2->lo_current_max;
  conf2->lo_current_motor_min_now = conf2->lo_current_min;
}

static THD_FUNCTION(timer_thread, arg) {
  (void)arg;

  chRegSetThreadName("mcif timer");
  for (;;) {
    // Decrease fault iterations
    if(chVTGetSystemTime() > MS2ST(5000)){
      float batt_resistance = 0.13;
      float normalized_voltage = mc_interface_get_voltage();
      normalized_voltage += batt_resistance*mc_interface_get_tot_current_in();
      normalized_voltage = ((normalized_voltage/(m_conf.l_battery_cut_end/3.1)) - 3.1)/1.1;
      if(capacity_estimate < 0){
        capacity_estimate = utils_batt_norm_v_to_capacity(normalized_voltage);
      }else
      {
        UTILS_LP_FAST(capacity_estimate,utils_batt_norm_v_to_capacity(normalized_voltage),0.001);
      }
    }

    if (m_ignore_iterations > 0) {
      m_ignore_iterations--;
    }
    else {
      if (!IS_DRV_FAULT() && !IS_DRV_FAULT2()) {
        m_fault_now = FAULT_CODE_NONE;
      }
    }

    update_override_limits(&m_conf,&m_conf2);


    chThdSleepMilliseconds(1);
  }
}

static THD_FUNCTION(sample_send_thread, arg) {
  (void)arg;

  chRegSetThreadName("SampleSender");

  sample_send_tp = chThdGetSelfX();

  for (;;) {
    chEvtWaitAny((eventmask_t)1);

    int len = 0;
    int offset = 0;

    switch (m_sample_mode_last) {
    case DEBUG_SAMPLING_NOW:
    case DEBUG_SAMPLING_START:
      len = m_sample_len;
      break;

    case DEBUG_SAMPLING_TRIGGER_START:
    case DEBUG_SAMPLING_TRIGGER_FAULT:
    case DEBUG_SAMPLING_TRIGGER_START_NOSEND:
    case DEBUG_SAMPLING_TRIGGER_FAULT_NOSEND:
      len = ADC_SAMPLE_MAX_LEN;
      offset = m_sample_trigger - m_sample_len;
      break;

    default:
      break;
    }
    static long int avg1;
    static long int avg2;
    for (int i = 0; i < len; i++) {
      avg1 += m_ph1_samples[i];
      avg2 += m_ph2_samples[i];
    }
    avg1 /= len;
    avg2 /= len;
    for (int i = 0; i < len; i++) {
      m_ph1_samples[i] -= avg1;
      m_ph2_samples[i] -= avg2;
      m_ph3_samples[i] = sqrtf((m_ph1_samples[i] * m_ph1_samples[i]) + (m_ph2_samples[i] * m_ph2_samples[i]));
    }
    avg1 = 0;
    for (int i = 0; i < len; i++) {
      avg1 += m_ph3_samples[i];
    }
    avg1 /= len;
    for (int i = 0; i < len; i++) {
      m_ph3_samples[i] = avg1;
    }

    for (int i = 0; i < len; i++) {
      uint8_t buffer[40];
      int32_t index = 0;
      int ind_samp = i + offset;

      while (ind_samp >= ADC_SAMPLE_MAX_LEN) {
        ind_samp -= ADC_SAMPLE_MAX_LEN;
      }

      while (ind_samp < 0) {
        ind_samp += ADC_SAMPLE_MAX_LEN;
      }

      buffer[index++] = COMM_SAMPLE_PRINT;
      buffer_append_float32_auto(buffer, (float)m_curr0_samples[ind_samp] * FAC_CURRENT, &index);
      buffer_append_float32_auto(buffer, (float)m_curr1_samples[ind_samp] * FAC_CURRENT, &index);
      buffer_append_float32_auto(buffer, ((float)m_va1_samples[ind_samp] / 4096.0 * V_REG) * ((VIN_R1 + VIN_R2) / VIN_R2), &index);
      buffer_append_float32_auto(buffer, ((float)m_vb1_samples[ind_samp] / 4096.0 * V_REG) * ((VIN_R1 + VIN_R2) / VIN_R2), &index);
      buffer_append_float32_auto(buffer, ((float)m_ph3_samples[ind_samp] / 4096.0 * V_REG) * ((VIN_R1 + VIN_R2) / VIN_R2), &index);
      buffer_append_float32_auto(buffer, ((float)m_vzero_samples[ind_samp] / 4096.0 * V_REG) * ((VIN_R1 + VIN_R2) / VIN_R2), &index);
      buffer_append_float32_auto(buffer, (float)m_curr_fir_samples[ind_samp] / (8.0 / FAC_CURRENT), &index);
      buffer_append_float32_auto(buffer, (float)m_f_sw_samples[ind_samp] * 10.0, &index);
      buffer[index++] = m_status_samples[ind_samp];
      buffer[index++] = m_phase_samples[ind_samp];

      commands_send_packet(buffer, index);
    }
  }
}
