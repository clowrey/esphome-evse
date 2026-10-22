// ESPHome glue for the charge controller: reads the entities, runs the core once a second
// and writes its outputs back to the Current Setpoint number, the enable switch and the
// status sensors. All control decisions live in charge_controller.h.
#pragma once

#include <cstdint>

#include "esphome/components/number/number.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"

#include "charge_controller.h"

namespace esphome {
namespace evse_charge_control {

enum class ParamId : uint8_t {
  NORMAL_TARGET = 0,
  FAST_TARGET,
  MIN_SOC,
  LV_CUTOFF,
  DEADBAND,
  GAIN,
  MAX_STEP_UP,
  MAX_STEP_DOWN,
  SETTLE,
  SETTLE_FALLBACK,
  EVIDENCE,
  FILTER_ALPHA,
  PAUSE_AFTER,
  RESUME_AFTER,
  RESUME_MARGIN,
  STALE,
  BLIND_TIMEOUT,
  COUNT
};

class EvseChargeControl : public PollingComponent {
 public:
  void setup() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  // Inputs.
  void add_power_input(sensor::Sensor *s, bool enabled);
  void set_fallback_power_input(sensor::Sensor *s, bool enabled);
  void set_power_divisor(float d) { this->divisor_ = d; }
  void set_battery_voltage_sensor(sensor::Sensor *s) { this->battery_v_ = s; }
  void add_soc_sensor(sensor::Sensor *s, bool enabled);
  void set_line_voltage_sensor(sensor::Sensor *s) { this->line_v_ = s; }
  void set_nominal_line_voltage(float v) { this->nominal_v_ = v; }
  void set_cp_state_number(number::Number *n) { this->cp_state_ = n; }
  void set_mode_number(number::Number *n) { this->mode_ = n; }
  void set_setpoint_number(number::Number *n) { this->setpoint_ = n; }
  void set_max_current_number(number::Number *n) { this->max_current_ = n; }
  void set_enable_switch(switch_::Switch *s) { this->enable_ = s; }
  // Tunables: each number is read every tick; a missing or NaN number keeps the core default.
  void set_param_number(ParamId id, number::Number *n) { this->params_[static_cast<int>(id)] = n; }

  // Outputs (all optional).
  void set_raw_power_sensor(sensor::Sensor *s) { this->raw_power_ = s; }
  void set_filtered_power_sensor(sensor::Sensor *s) { this->filtered_power_ = s; }
  void set_state_text_sensor(text_sensor::TextSensor *t) { this->state_text_ = t; }
  void set_event_text_sensor(text_sensor::TextSensor *t) { this->event_text_ = t; }
  void set_source_text_sensor(text_sensor::TextSensor *t) { this->source_text_ = t; }
  void set_debug_text_sensor(text_sensor::TextSensor *t) { this->debug_text_ = t; }

  // Called from the Charger Enabled switch: try to clear a low-voltage lockout on the next tick.
  void request_lockout_clear() { this->lockout_clear_request_ = true; }
  const evse_cc::Outputs &outputs() const { return this->core_.outputs(); }

 protected:
  struct Tracked {
    sensor::Sensor *sensor{nullptr};
    bool enabled{false};
    bool seen{false};
    uint32_t last_ms{0};
  };

  void track_(Tracked &t);
  evse_cc::PowerInput read_power_(const Tracked &t, uint32_t now) const;
  float read_param_(ParamId id, float fallback) const;
  void build_params_();
  static float number_state_(number::Number *n, float fallback);
  static float sensor_state_(sensor::Sensor *s);

  evse_cc::ChargeController core_;
  ESPPreferenceObject lockout_pref_;
  bool saved_lockout_{false};

  Tracked primaries_[evse_cc::MAX_POWER_INPUTS];
  int n_primary_{0};
  Tracked fallback_;
  Tracked socs_[evse_cc::MAX_SOC_INPUTS];
  int n_soc_{0};
  float divisor_{1.0f};
  float nominal_v_{230.0f};
  sensor::Sensor *battery_v_{nullptr};
  sensor::Sensor *line_v_{nullptr};
  number::Number *cp_state_{nullptr};
  number::Number *mode_{nullptr};
  number::Number *setpoint_{nullptr};
  number::Number *max_current_{nullptr};
  switch_::Switch *enable_{nullptr};
  number::Number *params_[static_cast<int>(ParamId::COUNT)] = {};

  sensor::Sensor *raw_power_{nullptr};
  sensor::Sensor *filtered_power_{nullptr};
  text_sensor::TextSensor *state_text_{nullptr};
  text_sensor::TextSensor *event_text_{nullptr};
  text_sensor::TextSensor *source_text_{nullptr};
  text_sensor::TextSensor *debug_text_{nullptr};

  bool lockout_clear_request_{false};
  uint32_t ticks_{0};
  char last_state_[96] = {};
  char last_source_[64] = {};
  char last_debug_[256] = {};
};

}  // namespace evse_charge_control
}  // namespace esphome
