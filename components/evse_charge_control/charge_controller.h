// Pure charge-current controller for the EVSE: holds the house-battery power at a
// per-mode target by stepping the current offered to the car.
//
// No ESPHome dependencies, so the same code runs in the firmware and in the host
// replay test under test/charge_control. All times are milliseconds, all powers
// are signed kilowatts with positive meaning the house battery is charging.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace evse_cc {

enum class Mode : uint8_t { STOP = 0, MANUAL = 1, NORMAL = 2, FAST = 3 };
enum class CpState : uint8_t { A_IDLE = 0, B_CONNECTED = 1, C_CHARGING = 2, D_VENT = 3, F_ERROR = 4 };
enum class Source : uint8_t { NONE = 0, PRIMARY = 1, FALLBACK = 2 };

constexpr int MAX_POWER_INPUTS = 4;
constexpr int MAX_SOC_INPUTS = 2;
constexpr int ABSOLUTE_MAX_A = 40;

// One Home Assistant power entity, already scaled to kW by the caller.
struct PowerInput {
  bool enabled = false;
  float kw = NAN;                  // NaN = unavailable / unknown
  uint32_t age_ms = 0xFFFFFFFFu;   // time since the last update
};

struct Inputs {
  uint32_t now_ms = 0;
  Mode mode = Mode::STOP;
  CpState cp_state = CpState::A_IDLE;
  int setpoint_a = 6;              // what the Current Setpoint number holds right now
  int max_current_a = 40;          // Max Current Limit number
  PowerInput primary[MAX_POWER_INPUTS];
  PowerInput fallback;
  float soc_pct[MAX_SOC_INPUTS] = {NAN, NAN};
  float battery_v = NAN;
  float line_v = NAN;              // EVSE line voltage; NaN or implausible -> nominal_line_v
  bool lockout_clear_request = false;  // one-shot from the Charger Enabled switch
};

struct Params {
  float target_normal_kw = -1.0f;
  float target_fast_kw = -3.0f;
  float deadband_kw = 0.25f;           // effective = max(deadband_kw, deadband_amps * kw_per_amp)
  float deadband_amps = 1.5f;
  float gain = 0.8f;                   // amps per amp of error
  int max_step_up_a = 4;
  int max_step_down_a = 8;
  uint32_t settle_ms = 8000;           // minimum wait after a step before judging again
  uint32_t settle_fallback_ms = 40000; // minimum wait while running on the fallback source
  float settle_intervals = 2.5f;       // also wait this many observed input update intervals (slow sensors)
  int evidence_samples = 3;            // ticks outside the band before a step
  float filter_alpha = 0.5f;           // EMA per tick, primary source
  float filter_alpha_fallback = 0.3f;
  uint32_t pause_after_ms = 60000;     // Fast only: discharge above target at the floor for this long -> pause
  uint32_t resume_after_ms = 60000;
  float resume_margin_kw = 0.5f;
  float min_soc_pct = 20.0f;
  float soc_ramp_pct = 10.0f;
  float lv_cutoff_v = 0.0f;            // 0 disables the low-voltage lockout
  uint32_t lv_latch_ms = 30000;
  uint32_t stale_ms = 30000;           // primary input older than this is stale
  uint32_t fallback_stale_ms = 90000;
  uint32_t blind_timeout_ms = 600000;  // no battery data for this long -> drop to the floor; 0 = hold forever
  float nominal_line_v = 230.0f;
  int floor_a = 6;
  uint32_t boot_hold_ms = 30000;       // no steps this soon after boot
};

struct Outputs {
  bool charging_allowed = false;       // -> enable switch
  int setpoint_a = 6;                  // -> Current Setpoint number
  bool setpoint_changed = false;       // true only on the tick that commands a new setpoint
  float battery_kw_raw = NAN;
  float battery_kw_filtered = NAN;
  Source source = Source::NONE;
  int stale_input = -1;                // index of the first stale primary input, or -1
  uint32_t stale_age_ms = 0;
  uint32_t input_interval_ms = 0;      // observed update interval of the active source (slowest input)
  uint32_t settle_ms = 0;              // settle time in use
  bool paused_low_pv = false;
  bool paused_low_soc = false;
  bool lv_lockout = false;
  bool soc_limiter_active = false;
  bool blind = false;
  int lv_below_s = 0;
  float lv_trigger_v = NAN;
  int soc_limit_a = ABSOLUTE_MAX_A;
  int cap_a = ABSOLUTE_MAX_A;
  float kw_per_amp = 0.23f;
  float target_kw = -1.0f;
  float deadband_kw = 0.3f;
  float error_kw = NAN;
  float house_soc = NAN;
  bool has_event = false;
  char event[64] = {};
};

class ChargeController {
 public:
  void set_params(const Params &p) { p_ = p; }
  const Params &params() const { return p_; }
  // Call once at boot with the saved lockout flag, before the first update().
  void restore(bool lv_lockout) { out_.lv_lockout = lv_lockout; }
  // Call once per tick (1 s).
  const Outputs &update(const Inputs &in);
  const Outputs &outputs() const { return out_; }

  // Status line for the LCD and Home Assistant, same wording as the old firmware plus the vehicle state.
  void state_text(char *buf, size_t len) const;
  // One-line diagnostic dump for the HA Inputs Debug sensor.
  void debug_text(char *buf, size_t len) const;

  static const char *mode_name(Mode m);
  static const char *source_name(Source s);

 private:
  void event_(const char *fmt, ...);
  void reset_evidence_();
  void evaluate_source_(const Inputs &in);
  void track_interval_(int slot, const PowerInput &pi, uint32_t now);
  void evaluate_lockout_(const Inputs &in);
  void evaluate_soc_(const Inputs &in, bool auto_mode);

  Params p_;
  Outputs out_;
  Inputs last_in_;
  bool started_ = false;
  uint32_t boot_ms_ = 0;
  uint32_t last_step_ms_ = 0;
  int last_commanded_a_ = -1;
  int evidence_up_ = 0;
  int evidence_down_ = 0;
  uint32_t floor_discharge_since_ms_ = 0;
  uint32_t resume_ok_since_ms_ = 0;
  uint32_t blind_since_ms_ = 0;
  bool blind_floor_done_ = false;
  uint32_t lv_below_since_ms_ = 0;
  bool lv_below_ = false;
  bool filter_seeded_ = false;
  // Per input (primaries, then the fallback): time of the last new sample and its smoothed interval.
  uint32_t last_sample_ms_[MAX_POWER_INPUTS + 1] = {};
  float interval_ms_[MAX_POWER_INPUTS + 1] = {};
  bool interval_seeded_[MAX_POWER_INPUTS + 1] = {};
  Mode last_mode_ = Mode::STOP;
  CpState last_cp_ = CpState::A_IDLE;
  Source last_source_ = Source::NONE;
};

}  // namespace evse_cc
