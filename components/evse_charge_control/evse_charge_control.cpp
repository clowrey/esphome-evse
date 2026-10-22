#include "evse_charge_control.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace evse_charge_control {

static const char *const TAG = "charge_ctl";

void EvseChargeControl::add_power_input(sensor::Sensor *s, bool enabled) {
  if (this->n_primary_ >= evse_cc::MAX_POWER_INPUTS) {
    return;
  }
  Tracked &t = this->primaries_[this->n_primary_++];
  t.sensor = s;
  t.enabled = enabled;
}

void EvseChargeControl::set_fallback_power_input(sensor::Sensor *s, bool enabled) {
  this->fallback_.sensor = s;
  this->fallback_.enabled = enabled;
}

void EvseChargeControl::add_soc_sensor(sensor::Sensor *s, bool enabled) {
  if (this->n_soc_ >= evse_cc::MAX_SOC_INPUTS) {
    return;
  }
  Tracked &t = this->socs_[this->n_soc_++];
  t.sensor = s;
  t.enabled = enabled;
}

void EvseChargeControl::track_(Tracked &t) {
  if (t.sensor == nullptr || !t.enabled) {
    return;
  }
  // Every publish from Home Assistant stamps the input, including NaN for "unavailable",
  // so the age only grows when the API connection or the entity stops updating.
  t.sensor->add_on_state_callback([&t](float) {
    t.last_ms = millis();
    t.seen = true;
  });
}

void EvseChargeControl::setup() {
  for (int i = 0; i < this->n_primary_; i++) {
    this->track_(this->primaries_[i]);
  }
  this->track_(this->fallback_);
  this->lockout_pref_ = global_preferences->make_preference<bool>(fnv1_hash("evse_cc_lv_lockout"));
  bool saved = false;
  if (this->lockout_pref_.load(&saved)) {
    this->saved_lockout_ = saved;
  }
  this->core_.restore(this->saved_lockout_);
  this->build_params_();
}

void EvseChargeControl::dump_config() {
  ESP_LOGCONFIG(TAG, "EVSE Charge Control:");
  ESP_LOGCONFIG(TAG, "  Update interval: %u ms", this->get_update_interval());
  ESP_LOGCONFIG(TAG, "  Power inputs: %d (divisor %.1f), fallback: %s", this->n_primary_, this->divisor_,
                YESNO(this->fallback_.enabled && this->fallback_.sensor != nullptr));
  ESP_LOGCONFIG(TAG, "  SOC inputs: %d", this->n_soc_);
  ESP_LOGCONFIG(TAG, "  Nominal line voltage: %.0f V", this->nominal_v_);
  ESP_LOGCONFIG(TAG, "  Saved low-voltage lockout: %s", YESNO(this->saved_lockout_));
}

float EvseChargeControl::number_state_(number::Number *n, float fallback) {
  if (n == nullptr || !n->has_state()) {
    return fallback;
  }
  const float v = n->state;
  return std::isfinite(v) ? v : fallback;
}

float EvseChargeControl::sensor_state_(sensor::Sensor *s) {
  if (s == nullptr || !s->has_state()) {
    return NAN;
  }
  return s->state;
}

float EvseChargeControl::read_param_(ParamId id, float fallback) const {
  return number_state_(this->params_[static_cast<int>(id)], fallback);
}

void EvseChargeControl::build_params_() {
  const evse_cc::Params d;  // core defaults
  evse_cc::Params p = d;
  p.target_normal_kw = this->read_param_(ParamId::NORMAL_TARGET, d.target_normal_kw);
  p.target_fast_kw = this->read_param_(ParamId::FAST_TARGET, d.target_fast_kw);
  p.min_soc_pct = this->read_param_(ParamId::MIN_SOC, d.min_soc_pct);
  p.lv_cutoff_v = this->read_param_(ParamId::LV_CUTOFF, d.lv_cutoff_v);
  p.deadband_kw = this->read_param_(ParamId::DEADBAND, d.deadband_kw);
  p.gain = this->read_param_(ParamId::GAIN, d.gain);
  p.max_step_up_a = static_cast<int>(std::lround(this->read_param_(ParamId::MAX_STEP_UP, d.max_step_up_a)));
  p.max_step_down_a = static_cast<int>(std::lround(this->read_param_(ParamId::MAX_STEP_DOWN, d.max_step_down_a)));
  p.settle_ms = static_cast<uint32_t>(this->read_param_(ParamId::SETTLE, d.settle_ms / 1000.0f) * 1000.0f);
  p.settle_fallback_ms =
      static_cast<uint32_t>(this->read_param_(ParamId::SETTLE_FALLBACK, d.settle_fallback_ms / 1000.0f) * 1000.0f);
  p.evidence_samples = static_cast<int>(std::lround(this->read_param_(ParamId::EVIDENCE, d.evidence_samples)));
  p.filter_alpha = this->read_param_(ParamId::FILTER_ALPHA, d.filter_alpha);
  p.pause_after_ms = static_cast<uint32_t>(this->read_param_(ParamId::PAUSE_AFTER, d.pause_after_ms / 1000.0f) * 1000.0f);
  p.resume_after_ms =
      static_cast<uint32_t>(this->read_param_(ParamId::RESUME_AFTER, d.resume_after_ms / 1000.0f) * 1000.0f);
  p.resume_margin_kw = this->read_param_(ParamId::RESUME_MARGIN, d.resume_margin_kw);
  p.stale_ms = static_cast<uint32_t>(this->read_param_(ParamId::STALE, d.stale_ms / 1000.0f) * 1000.0f);
  p.fallback_stale_ms = std::max(d.fallback_stale_ms, p.stale_ms * 3);
  p.blind_timeout_ms =
      static_cast<uint32_t>(this->read_param_(ParamId::BLIND_TIMEOUT, d.blind_timeout_ms / 1000.0f) * 1000.0f);
  p.nominal_line_v = this->nominal_v_;
  if (p.max_step_up_a < 1) {
    p.max_step_up_a = 1;
  }
  if (p.max_step_down_a < 1) {
    p.max_step_down_a = 1;
  }
  if (p.evidence_samples < 1) {
    p.evidence_samples = 1;
  }
  this->core_.set_params(p);
}

evse_cc::PowerInput EvseChargeControl::read_power_(const Tracked &t, uint32_t now) const {
  evse_cc::PowerInput pi;
  pi.enabled = t.enabled && t.sensor != nullptr;
  if (!pi.enabled) {
    return pi;
  }
  const float raw = sensor_state_(t.sensor);
  pi.kw = std::isfinite(raw) ? raw / this->divisor_ : NAN;
  pi.age_ms = t.seen ? (now - t.last_ms) : 0xFFFFFFFFu;
  return pi;
}

void EvseChargeControl::update() {
  const uint32_t now = millis();
  this->ticks_++;
  this->build_params_();

  evse_cc::Inputs in;
  in.now_ms = now;
  const int mode = static_cast<int>(std::lround(number_state_(this->mode_, 0.0f)));
  in.mode = static_cast<evse_cc::Mode>(mode < 0 ? 0 : (mode > 3 ? 3 : mode));
  const int cp = static_cast<int>(std::lround(number_state_(this->cp_state_, 0.0f)));
  in.cp_state = static_cast<evse_cc::CpState>(cp < 0 ? 0 : (cp > 4 ? 4 : cp));
  in.setpoint_a = static_cast<int>(std::lround(number_state_(this->setpoint_, 6.0f)));
  in.max_current_a = static_cast<int>(std::lround(number_state_(this->max_current_, 40.0f)));
  for (int i = 0; i < this->n_primary_; i++) {
    in.primary[i] = this->read_power_(this->primaries_[i], now);
  }
  in.fallback = this->read_power_(this->fallback_, now);
  for (int i = 0; i < this->n_soc_; i++) {
    in.soc_pct[i] = this->socs_[i].enabled ? sensor_state_(this->socs_[i].sensor) : NAN;
  }
  in.battery_v = sensor_state_(this->battery_v_);
  in.line_v = sensor_state_(this->line_v_);
  in.lockout_clear_request = this->lockout_clear_request_;
  this->lockout_clear_request_ = false;

  const evse_cc::Outputs &o = this->core_.update(in);

  if (o.setpoint_changed && this->setpoint_ != nullptr) {
    auto call = this->setpoint_->make_call();
    call.set_value(static_cast<float>(o.setpoint_a));
    call.perform();
  }
  if (this->enable_ != nullptr && this->enable_->state != o.charging_allowed) {
    if (o.charging_allowed) {
      this->enable_->turn_on();
    } else {
      this->enable_->turn_off();
    }
  }
  if (o.lv_lockout != this->saved_lockout_) {
    this->saved_lockout_ = o.lv_lockout;
    this->lockout_pref_.save(&this->saved_lockout_);
  }
  if (o.has_event) {
    ESP_LOGI(TAG, "%s", o.event);
    if (this->event_text_ != nullptr) {
      this->event_text_->publish_state(o.event);
    }
  }
  if (this->raw_power_ != nullptr) {
    this->raw_power_->publish_state(o.battery_kw_raw);
  }
  if (this->filtered_power_ != nullptr) {
    this->filtered_power_->publish_state(o.battery_kw_filtered);
  }

  if (this->state_text_ != nullptr) {
    char buf[96];
    this->core_.state_text(buf, sizeof(buf));
    if (strcmp(buf, this->last_state_) != 0) {
      snprintf(this->last_state_, sizeof(this->last_state_), "%s", buf);
      this->state_text_->publish_state(buf);
    }
  }
  if (this->source_text_ != nullptr) {
    char buf[64];
    if (o.source == evse_cc::Source::FALLBACK && o.stale_input >= 0) {
      snprintf(buf, sizeof(buf), "fallback (input %d stale %lus)", o.stale_input + 1,
               static_cast<unsigned long>(o.stale_age_ms == 0xFFFFFFFFu ? 0 : o.stale_age_ms / 1000u));
    } else if (o.source == evse_cc::Source::NONE && o.stale_input >= 0) {
      snprintf(buf, sizeof(buf), "none (input %d stale %lus)", o.stale_input + 1,
               static_cast<unsigned long>(o.stale_age_ms == 0xFFFFFFFFu ? 0 : o.stale_age_ms / 1000u));
    } else {
      snprintf(buf, sizeof(buf), "%s", evse_cc::ChargeController::source_name(o.source));
    }
    if (strcmp(buf, this->last_source_) != 0) {
      snprintf(this->last_source_, sizeof(this->last_source_), "%s", buf);
      this->source_text_->publish_state(buf);
    }
  }
  if (this->debug_text_ != nullptr && (this->ticks_ % 10u) == 0u) {
    char buf[256];
    this->core_.debug_text(buf, sizeof(buf));
    if (strcmp(buf, this->last_debug_) != 0) {
      snprintf(this->last_debug_, sizeof(this->last_debug_), "%s", buf);
      this->debug_text_->publish_state(buf);
    }
  }
}

}  // namespace evse_charge_control
}  // namespace esphome
