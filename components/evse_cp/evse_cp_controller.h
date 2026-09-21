#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "esphome/components/number/number.h"
#include "esphome/components/output/float_output.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/core/component.h"
#include "esphome/core/log.h"

namespace esphome {
namespace evse_cp {

class EvseCpController : public PollingComponent {
 public:
  explicit EvseCpController(uint32_t update_interval_ms = 100) : PollingComponent(update_interval_ms) {}

  void setup() override {
    // Default to State A at boot for a safe idle pilot.
    this->publish_state_(State::STATE_A_IDLE);
    this->apply_outputs_for_state_(State::STATE_A_IDLE);
  }

  void dump_config() override {
    ESP_LOGCONFIG(TAG, "EVSE CP Controller:");
    ESP_LOGCONFIG(TAG, "  Update interval: %u ms", this->get_update_interval());
    ESP_LOGCONFIG(TAG, "  Hysteresis: %.2f V", HYSTERESIS_VOLTS);
    ESP_LOGCONFIG(TAG, "  Debounce: %u samples", DEBOUNCE_SAMPLES);
    ESP_LOGCONFIG(TAG, "  Relay close delay: %u ms", RELAY_CLOSE_DELAY_SAMPLES * this->get_update_interval());
    ESP_LOGCONFIG(TAG, "  Relay open delay: %u ms", RELAY_OPEN_DELAY_SAMPLES * this->get_update_interval());
    ESP_LOGCONFIG(TAG, "  CP high voltage sample present: %s", YESNO(this->has_cp_high_voltage_));
  }

  void update() override {
    if (!this->is_fully_wired_()) {
      return;
    }

    if (this->debug_error_switch_->state) {
      this->force_state_(State::STATE_F_ERROR);
      this->publish_state_(this->active_state_);
      this->apply_outputs_for_state_(this->active_state_);
      return;
    }

    const State requested_state = this->compute_requested_state_();
    const State stable_state = this->debounce_state_(requested_state);

    this->publish_state_(stable_state);
    this->apply_outputs_for_state_(stable_state);
  }

  void set_cp_high_voltage(float voltage) {
    this->cp_high_voltage_ = voltage;
    this->has_cp_high_voltage_ = true;
  }

  void set_cp_low_voltage(float voltage) { this->cp_low_voltage_ = voltage; }

  void set_cp_pwm_output(output::FloatOutput *cp_pwm_output) { this->cp_pwm_output_ = cp_pwm_output; }
  void set_cp_pwm_sensor(sensor::Sensor *cp_pwm_sensor) { this->cp_pwm_sensor_ = cp_pwm_sensor; }
  void set_cp_state_number(number::Number *cp_state_number) { this->cp_state_number_ = cp_state_number; }
  void set_current_setpoint_number(number::Number *current_setpoint_number) {
    this->current_setpoint_number_ = current_setpoint_number;
  }
  void set_enable_switch(switch_::Switch *enable_switch) { this->enable_switch_ = enable_switch; }
  void set_debug_error_switch(switch_::Switch *debug_error_switch) { this->debug_error_switch_ = debug_error_switch; }
  void set_relay_switch(switch_::Switch *relay_switch) { this->relay_switch_ = relay_switch; }

 protected:
  enum class State : uint8_t {
    STATE_A_IDLE = 0,
    STATE_B_CONNECTED = 1,
    STATE_C_CHARGING = 2,
    STATE_D_VENT = 3,
    STATE_F_ERROR = 4,
  };

  static constexpr const char *const TAG = "evse_cp";
  static constexpr float THRESHOLD_A_TO_B = 10.5f;
  static constexpr float THRESHOLD_B_TO_C = 7.0f;
  // Relax this split so a slightly under-calibrated +6V pilot does not
  // get interpreted as ventilation-required (State D).
  static constexpr float THRESHOLD_C_TO_D = 3.5f;
  static constexpr float THRESHOLD_D_TO_F = 2.0f;
  static constexpr float HYSTERESIS_VOLTS = 0.25f;
  static constexpr uint8_t DEBOUNCE_SAMPLES = 2;
  static constexpr uint8_t RELAY_CLOSE_DELAY_SAMPLES = 3;  // 300ms @ 100ms update
  static constexpr uint8_t RELAY_OPEN_DELAY_SAMPLES = 5;   // 500ms @ 100ms update

  bool is_fully_wired_() const {
    return this->cp_pwm_output_ != nullptr && this->cp_pwm_sensor_ != nullptr && this->cp_state_number_ != nullptr &&
           this->current_setpoint_number_ != nullptr && this->enable_switch_ != nullptr &&
           this->debug_error_switch_ != nullptr && this->relay_switch_ != nullptr;
  }

  State compute_requested_state_() const {
    if (!this->has_cp_high_voltage_) {
      return State::STATE_A_IDLE;
    }

    return this->decode_state_from_cp_voltage_(this->cp_high_voltage_, this->active_state_);
  }

  State decode_state_from_cp_voltage_(float voltage, State previous_state) const {
    const float ab_threshold = (previous_state == State::STATE_A_IDLE) ? (THRESHOLD_A_TO_B - HYSTERESIS_VOLTS)
                                                                        : (THRESHOLD_A_TO_B + HYSTERESIS_VOLTS);
    const float bc_threshold = (previous_state == State::STATE_B_CONNECTED) ? (THRESHOLD_B_TO_C - HYSTERESIS_VOLTS)
                                                                             : (THRESHOLD_B_TO_C + HYSTERESIS_VOLTS);
    const float cd_threshold = (previous_state == State::STATE_C_CHARGING) ? (THRESHOLD_C_TO_D - HYSTERESIS_VOLTS)
                                                                            : (THRESHOLD_C_TO_D + HYSTERESIS_VOLTS);
    const float df_threshold = (previous_state == State::STATE_D_VENT) ? (THRESHOLD_D_TO_F - HYSTERESIS_VOLTS)
                                                                        : (THRESHOLD_D_TO_F + HYSTERESIS_VOLTS);

    if (voltage > ab_threshold) {
      return State::STATE_A_IDLE;
    }
    if (voltage > bc_threshold) {
      return State::STATE_B_CONNECTED;
    }
    if (voltage > cd_threshold) {
      return State::STATE_C_CHARGING;
    }
    if (voltage > df_threshold) {
      return State::STATE_D_VENT;
    }
    return State::STATE_F_ERROR;
  }

  State debounce_state_(State requested_state) {
    if (requested_state == this->active_state_) {
      this->pending_state_ = this->active_state_;
      this->pending_samples_ = 0;
      return this->active_state_;
    }

    if (requested_state == State::STATE_F_ERROR) {
      this->force_state_(State::STATE_F_ERROR);
      return this->active_state_;
    }

    if (requested_state != this->pending_state_) {
      this->pending_state_ = requested_state;
      this->pending_samples_ = 1;
      return this->active_state_;
    }

    if (this->pending_samples_ < 255) {
      this->pending_samples_++;
    }

    if (this->pending_samples_ >= DEBOUNCE_SAMPLES) {
      ESP_LOGD(TAG, "CP transition %.0f -> %.0f (Vhigh=%.3f V)", static_cast<float>(static_cast<uint8_t>(this->active_state_)),
               static_cast<float>(static_cast<uint8_t>(requested_state)), this->cp_high_voltage_);
      this->active_state_ = requested_state;
      this->pending_state_ = requested_state;
      this->pending_samples_ = 0;
    }

    return this->active_state_;
  }

  void force_state_(State state) {
    this->active_state_ = state;
    this->pending_state_ = state;
    this->pending_samples_ = 0;
  }

  void publish_state_(State state) {
    const float next_state = static_cast<float>(static_cast<uint8_t>(state));
    if (std::isnan(this->last_published_cp_state_) || std::fabs(this->last_published_cp_state_ - next_state) > 0.01f) {
      this->cp_state_number_->publish_state(next_state);
      this->last_published_cp_state_ = next_state;
      ESP_LOGD(TAG, "CP state -> %.0f (Vhigh=%.3f V, Vlow=%.3f V)", next_state, this->cp_high_voltage_,
               this->cp_low_voltage_);
    }
  }

  void apply_outputs_for_state_(State state) {
    float target_pwm = 1.0f;
    bool target_relay_on = false;
    const bool enabled = this->enable_switch_->state;

    switch (state) {
      case State::STATE_A_IDLE:
        target_pwm = 1.0f;
        target_relay_on = false;
        break;

      case State::STATE_B_CONNECTED:
        if (enabled) {
          target_pwm = this->compute_pwm_from_setpoint_();
          target_relay_on = false;
        } else {
          target_pwm = 1.0f;
          target_relay_on = false;
        }
        break;

      case State::STATE_C_CHARGING:
        if (enabled) {
          target_pwm = this->compute_pwm_from_setpoint_();
          target_relay_on = true;
        } else {
          target_pwm = 1.0f;
          target_relay_on = false;
        }
        break;

      case State::STATE_D_VENT:
        // Ventilation-required charging is unsupported in this setup.
        target_pwm = 0.0f;
        target_relay_on = false;
        break;

      case State::STATE_F_ERROR:
        target_pwm = 0.0f;
        target_relay_on = false;
        break;
    }

    // Sequence pilot first, relay second.
    // This gives the EV a brief chance to react to CP change before contactor movement.
    this->command_pwm_(target_pwm);

    if (state == State::STATE_F_ERROR) {
      this->clear_relay_pending_();
      this->command_relay_(false);
      return;
    }

    this->command_relay_delayed_(target_relay_on);
  }

  float compute_pwm_from_setpoint_() const {
    float setpoint_amps = this->current_setpoint_number_->state;
    if (std::isnan(setpoint_amps)) {
      setpoint_amps = 6.0f;
    }
    return std::clamp(setpoint_amps / 60.0f, 0.0f, 1.0f);
  }

  void command_relay_(bool relay_on) {
    if (this->relay_switch_->state == relay_on) {
      return;
    }
    if (relay_on) {
      this->relay_switch_->turn_on();
    } else {
      this->relay_switch_->turn_off();
    }
  }

  void clear_relay_pending_() {
    this->relay_pending_ = false;
    this->relay_pending_target_ = false;
    this->relay_pending_samples_ = 0;
  }

  uint8_t relay_delay_samples_(bool target_on) const {
    return target_on ? RELAY_CLOSE_DELAY_SAMPLES : RELAY_OPEN_DELAY_SAMPLES;
  }

  void command_relay_delayed_(bool target_on) {
    if (this->relay_switch_->state == target_on) {
      this->clear_relay_pending_();
      return;
    }

    if (!this->relay_pending_ || this->relay_pending_target_ != target_on) {
      this->relay_pending_ = true;
      this->relay_pending_target_ = target_on;
      this->relay_pending_samples_ = 0;
      return;
    }

    if (this->relay_pending_samples_ < 255) {
      this->relay_pending_samples_++;
    }

    if (this->relay_pending_samples_ >= this->relay_delay_samples_(target_on)) {
      this->command_relay_(target_on);
      this->clear_relay_pending_();
    }
  }

  void command_pwm_(float pwm_level) {
    pwm_level = std::clamp(pwm_level, 0.0f, 1.0f);

    if (!std::isnan(this->last_published_pwm_) && std::fabs(this->last_published_pwm_ - pwm_level) < 0.0001f) {
      return;
    }

    this->cp_pwm_output_->set_level(pwm_level);
    this->cp_pwm_sensor_->publish_state(pwm_level);
    this->last_published_pwm_ = pwm_level;
  }

  output::FloatOutput *cp_pwm_output_{nullptr};
  sensor::Sensor *cp_pwm_sensor_{nullptr};
  number::Number *cp_state_number_{nullptr};
  number::Number *current_setpoint_number_{nullptr};
  switch_::Switch *enable_switch_{nullptr};
  switch_::Switch *debug_error_switch_{nullptr};
  switch_::Switch *relay_switch_{nullptr};

  float cp_high_voltage_{12.0f};
  float cp_low_voltage_{0.0f};
  bool has_cp_high_voltage_{false};
  State active_state_{State::STATE_A_IDLE};
  State pending_state_{State::STATE_A_IDLE};
  uint8_t pending_samples_{0};
  bool relay_pending_{false};
  bool relay_pending_target_{false};
  uint8_t relay_pending_samples_{0};

  float last_published_pwm_{NAN};
  float last_published_cp_state_{NAN};
};

inline EvseCpController *cp_controller_singleton{nullptr};

inline void set_cp_controller(EvseCpController *controller) { cp_controller_singleton = controller; }

inline EvseCpController *get_cp_controller() { return cp_controller_singleton; }

}  // namespace evse_cp
}  // namespace esphome
