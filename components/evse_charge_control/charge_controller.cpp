#include "charge_controller.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace evse_cc {

namespace {

bool finite_(float v) { return std::isfinite(v); }

int clamp_i(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Elapsed time that is robust to millis() wrap-around.
uint32_t since(uint32_t now, uint32_t then) { return now - then; }

}  // namespace

const char *ChargeController::mode_name(Mode m) {
  switch (m) {
    case Mode::STOP:
      return "Stop";
    case Mode::MANUAL:
      return "Manual";
    case Mode::NORMAL:
      return "Normal";
    case Mode::FAST:
      return "Fast";
  }
  return "?";
}

const char *ChargeController::source_name(Source s) {
  switch (s) {
    case Source::NONE:
      return "none";
    case Source::PRIMARY:
      return "primary";
    case Source::FALLBACK:
      return "fallback";
  }
  return "?";
}

void ChargeController::event_(const char *fmt, ...) {
  // The first event of a tick wins; later ones in the same tick are dropped.
  if (out_.has_event) {
    return;
  }
  va_list args;
  va_start(args, fmt);
  vsnprintf(out_.event, sizeof(out_.event), fmt, args);
  va_end(args);
  out_.has_event = true;
}

void ChargeController::reset_evidence_() {
  evidence_up_ = 0;
  evidence_down_ = 0;
}

void ChargeController::track_interval_(int slot, const PowerInput &pi, uint32_t now) {
  if (!pi.enabled || !std::isfinite(pi.kw) || pi.age_ms == 0xFFFFFFFFu || pi.age_ms > now) {
    return;
  }
  const uint32_t sample_ms = now - pi.age_ms;
  if (!interval_seeded_[slot]) {
    last_sample_ms_[slot] = sample_ms;
    interval_seeded_[slot] = true;
    interval_ms_[slot] = 0.0f;
    return;
  }
  if (sample_ms > last_sample_ms_[slot] + 100u) {
    const float interval = static_cast<float>(sample_ms - last_sample_ms_[slot]);
    interval_ms_[slot] = (interval_ms_[slot] <= 0.0f) ? interval : (0.3f * interval + 0.7f * interval_ms_[slot]);
    last_sample_ms_[slot] = sample_ms;
  }
}

void ChargeController::evaluate_source_(const Inputs &in) {
  bool any_primary = false;
  bool primary_ok = true;
  float sum = 0.0f;
  float primary_interval = 0.0f;
  out_.stale_input = -1;
  out_.stale_age_ms = 0;
  for (int i = 0; i < MAX_POWER_INPUTS; i++) {
    const PowerInput &pi = in.primary[i];
    if (!pi.enabled) {
      continue;
    }
    any_primary = true;
    track_interval_(i, pi, in.now_ms);
    primary_interval = std::max(primary_interval, interval_ms_[i]);
    if (!finite_(pi.kw) || pi.age_ms > p_.stale_ms) {
      if (primary_ok) {
        out_.stale_input = i;
        out_.stale_age_ms = pi.age_ms;
      }
      primary_ok = false;
      continue;
    }
    sum += pi.kw;
  }

  track_interval_(MAX_POWER_INPUTS, in.fallback, in.now_ms);

  Source source = Source::NONE;
  float raw = NAN;
  float interval = 0.0f;
  if (any_primary && primary_ok) {
    source = Source::PRIMARY;
    raw = sum;
    interval = primary_interval;
  } else if (in.fallback.enabled && finite_(in.fallback.kw) && in.fallback.age_ms <= p_.fallback_stale_ms) {
    source = Source::FALLBACK;
    raw = in.fallback.kw;
    interval = interval_ms_[MAX_POWER_INPUTS];
  }
  out_.input_interval_ms = static_cast<uint32_t>(interval);
  // Settle time: the configured minimum, more on the fallback, and always a few update
  // intervals of a slow sensor so a step is judged on readings taken after it.
  float settle = static_cast<float>(p_.settle_ms);
  if (source == Source::FALLBACK) {
    settle = std::max(settle, static_cast<float>(p_.settle_fallback_ms));
  }
  settle = std::max(settle, p_.settle_intervals * interval);
  out_.settle_ms = static_cast<uint32_t>(settle);

  if (source != last_source_) {
    event_("source %s", source_name(source));
    filter_seeded_ = false;
    if (source == Source::NONE) {
      blind_since_ms_ = in.now_ms;
      blind_floor_done_ = false;
    }
    last_source_ = source;
  }

  out_.source = source;
  out_.blind = (source == Source::NONE);
  out_.battery_kw_raw = raw;
  if (finite_(raw)) {
    const float alpha = (source == Source::FALLBACK) ? p_.filter_alpha_fallback : p_.filter_alpha;
    if (!filter_seeded_) {
      out_.battery_kw_filtered = raw;
      filter_seeded_ = true;
    } else {
      out_.battery_kw_filtered = alpha * raw + (1.0f - alpha) * out_.battery_kw_filtered;
    }
  }
}

void ChargeController::evaluate_lockout_(const Inputs &in) {
  const float cutoff = p_.lv_cutoff_v;
  if (!(cutoff > 0.0f)) {
    // 0 V (or NaN) disables the lockout entirely.
    out_.lv_lockout = false;
    out_.lv_below_s = 0;
    lv_below_ = false;
    return;
  }

  if (finite_(in.battery_v) && in.battery_v <= cutoff) {
    if (!lv_below_) {
      lv_below_ = true;
      lv_below_since_ms_ = in.now_ms;
    }
    out_.lv_below_s = static_cast<int>(since(in.now_ms, lv_below_since_ms_) / 1000u);
    if (!out_.lv_lockout && since(in.now_ms, lv_below_since_ms_) >= p_.lv_latch_ms) {
      out_.lv_lockout = true;
      out_.lv_trigger_v = in.battery_v;
      event_("lv lockout %.2fV <= %.2fV", in.battery_v, cutoff);
    }
  } else {
    lv_below_ = false;
    out_.lv_below_s = 0;
  }

  if (in.lockout_clear_request && out_.lv_lockout) {
    if (finite_(in.battery_v) && in.battery_v > cutoff) {
      out_.lv_lockout = false;
      lv_below_ = false;
      out_.lv_below_s = 0;
      event_("lv lockout cleared %.2fV", in.battery_v);
    } else if (finite_(in.battery_v)) {
      event_("lockout clear blocked (%.2fV)", in.battery_v);
    } else {
      event_("lockout clear blocked (no batt voltage)");
    }
  }
}

void ChargeController::evaluate_soc_(const Inputs &in, bool auto_mode) {
  float house_soc = NAN;
  for (int i = 0; i < MAX_SOC_INPUTS; i++) {
    const float s = in.soc_pct[i];
    if (!finite_(s)) {
      continue;
    }
    const float c = std::clamp(s, 0.0f, 100.0f);
    if (!finite_(house_soc) || c < house_soc) {
      house_soc = c;
    }
  }
  out_.house_soc = house_soc;

  const int max_a = clamp_i(in.max_current_a, p_.floor_a, ABSOLUTE_MAX_A);
  out_.soc_limit_a = max_a;

  if (!auto_mode || !finite_(house_soc)) {
    out_.paused_low_soc = false;
    out_.soc_limiter_active = false;
    out_.cap_a = max_a;
    return;
  }

  const float min_soc = std::clamp(p_.min_soc_pct, 0.0f, 100.0f);
  if (house_soc <= min_soc) {
    if (!out_.paused_low_soc) {
      out_.paused_low_soc = true;
      event_("paused low soc %.0f%% <= %.0f%%", house_soc, min_soc);
    }
  } else if (out_.paused_low_soc && house_soc >= std::min(100.0f, min_soc + 1.0f)) {
    out_.paused_low_soc = false;
    event_("resumed soc %.0f%%", house_soc);
  }

  const float ramp_start = std::min(100.0f, min_soc + p_.soc_ramp_pct);
  const float release = std::min(100.0f, ramp_start + 1.0f);
  if (!out_.soc_limiter_active) {
    if (house_soc <= ramp_start) {
      out_.soc_limiter_active = true;
    }
  } else if (house_soc >= release) {
    out_.soc_limiter_active = false;
  }

  if (out_.soc_limiter_active) {
    if (house_soc <= min_soc || ramp_start <= min_soc) {
      out_.soc_limit_a = p_.floor_a;
    } else {
      float ratio = (house_soc - min_soc) / (ramp_start - min_soc);
      ratio = std::clamp(ratio, 0.0f, 1.0f);
      out_.soc_limit_a = p_.floor_a + static_cast<int>(std::lround(ratio * static_cast<float>(max_a - p_.floor_a)));
      out_.soc_limit_a = clamp_i(out_.soc_limit_a, p_.floor_a, max_a);
    }
  }
  out_.cap_a = std::min(max_a, out_.soc_limit_a);
}

const Outputs &ChargeController::update(const Inputs &in) {
  const uint32_t now = in.now_ms;
  const int floor_a = p_.floor_a;
  out_.has_event = false;
  out_.event[0] = '\0';
  out_.setpoint_changed = false;

  int setpoint = in.setpoint_a;
  if (setpoint < floor_a) {
    setpoint = floor_a;
  }
  // Note: a lower setpoint than the floor is treated as the floor; the number itself has that minimum.
  bool commanded = false;
  auto command = [&](int a) {
    a = clamp_i(a, floor_a, ABSOLUTE_MAX_A);
    if (a != setpoint) {
      setpoint = a;
      commanded = true;
    }
  };

  if (!started_) {
    started_ = true;
    boot_ms_ = now;
    last_step_ms_ = now;
    last_commanded_a_ = setpoint;
    last_mode_ = in.mode;
    last_cp_ = in.cp_state;
    last_source_ = Source::NONE;
  }

  const bool auto_mode = (in.mode == Mode::NORMAL || in.mode == Mode::FAST);

  // 0. Mode and vehicle transitions.
  if (in.mode != last_mode_) {
    reset_evidence_();
    floor_discharge_since_ms_ = 0;
    resume_ok_since_ms_ = 0;
    out_.paused_low_pv = false;
    last_step_ms_ = now;
    event_("mode %s", mode_name(in.mode));
    last_mode_ = in.mode;
  }
  if (in.cp_state != last_cp_) {
    reset_evidence_();
    floor_discharge_since_ms_ = 0;
    resume_ok_since_ms_ = 0;
    last_step_ms_ = now;
    if (in.cp_state == CpState::A_IDLE) {
      out_.paused_low_pv = false;
      if (setpoint > floor_a) {
        command(floor_a);
        event_("idle: %dA", floor_a);
      }
    }
    last_cp_ = in.cp_state;
  }

  // 1. A setpoint we did not command is a user change: accept it as the new starting point.
  if (in.setpoint_a != last_commanded_a_) {
    reset_evidence_();
    last_step_ms_ = now;
    if (out_.paused_low_pv) {
      out_.paused_low_pv = false;
    }
    if (!commanded) {
      event_("user set %dA", in.setpoint_a);
    }
    last_commanded_a_ = in.setpoint_a;
  }

  // 2. Power per amp from the measured line voltage, and the hold band.
  float line_v = p_.nominal_line_v;
  if (finite_(in.line_v) && in.line_v > 150.0f && in.line_v < 300.0f) {
    line_v = in.line_v;
  }
  out_.kw_per_amp = line_v / 1000.0f;
  out_.deadband_kw = std::max(p_.deadband_kw, p_.deadband_amps * out_.kw_per_amp);

  // 3. Battery power source and filter.
  evaluate_source_(in);
  const bool have_power = filter_seeded_ && !out_.blind;

  // 4. Low-voltage lockout (all modes).
  evaluate_lockout_(in);

  // 5. House SOC stop and current limiter (auto modes).
  evaluate_soc_(in, auto_mode);

  // 6. Is charging allowed at all?
  out_.charging_allowed = (in.mode != Mode::STOP) && !out_.lv_lockout &&
                          !(auto_mode && (out_.paused_low_soc || out_.paused_low_pv));

  // Hard caps apply immediately, in every mode except Stop.
  if (in.mode != Mode::STOP && setpoint > out_.cap_a) {
    command(out_.cap_a);
    event_("cap %dA", out_.cap_a);
    last_step_ms_ = now;
  }

  // 7. The control law.
  out_.target_kw = (in.mode == Mode::FAST) ? p_.target_fast_kw : p_.target_normal_kw;
  out_.error_kw = NAN;
  const uint32_t settle_ms = out_.settle_ms;
  const bool charging_now = auto_mode && out_.charging_allowed && in.cp_state == CpState::C_CHARGING;

  if (charging_now && have_power) {
    const float error = out_.battery_kw_filtered - out_.target_kw;  // > 0: room to raise the car
    out_.error_kw = error;
    if (error > out_.deadband_kw) {
      evidence_up_ = std::min(evidence_up_ + 1, 1000);
      evidence_down_ = 0;
    } else if (error < -out_.deadband_kw) {
      evidence_down_ = std::min(evidence_down_ + 1, 1000);
      evidence_up_ = 0;
    } else {
      reset_evidence_();
    }

    const bool settled = since(now, last_step_ms_) >= settle_ms && since(now, boot_ms_) >= p_.boot_hold_ms;
    const bool enough = (evidence_up_ >= p_.evidence_samples) || (evidence_down_ >= p_.evidence_samples);
    if (settled && enough && !commanded) {
      int step = static_cast<int>(std::lround((error / out_.kw_per_amp) * p_.gain));
      if (step == 0) {
        step = (error > 0.0f) ? 1 : -1;
      }
      step = clamp_i(step, -p_.max_step_down_a, p_.max_step_up_a);
      const int target_a = clamp_i(setpoint + step, floor_a, out_.cap_a);
      if (target_a != setpoint) {
        const int from = setpoint;
        command(target_a);
        last_step_ms_ = now;
        reset_evidence_();
        event_("auto %dA -> %dA (%.2f kW)", from, target_a, out_.battery_kw_filtered);
      } else {
        // At a limit with nowhere to go; keep the evidence from piling up.
        reset_evidence_();
      }
    }
  } else {
    reset_evidence_();
  }

  // 8. Fast mode pauses at the floor when the battery still discharges too hard, and resumes
  //    once the house alone leaves room for the car to restart at the floor.
  if (in.mode == Mode::FAST) {
    const bool at_floor_overdrawn = charging_now && have_power && setpoint <= floor_a &&
                                    (out_.battery_kw_filtered - out_.target_kw) < -out_.deadband_kw;
    if (at_floor_overdrawn) {
      if (floor_discharge_since_ms_ == 0) {
        floor_discharge_since_ms_ = now ? now : 1;
      } else if (since(now, floor_discharge_since_ms_) >= p_.pause_after_ms) {
        out_.paused_low_pv = true;
        out_.charging_allowed = false;
        floor_discharge_since_ms_ = 0;
        event_("paused low pv (%.2f kW)", out_.battery_kw_filtered);
      }
    } else {
      floor_discharge_since_ms_ = 0;
    }

    if (out_.paused_low_pv && have_power && !out_.lv_lockout && !out_.paused_low_soc &&
        (in.cp_state == CpState::B_CONNECTED || in.cp_state == CpState::C_CHARGING)) {
      const float predicted = out_.battery_kw_filtered - static_cast<float>(floor_a) * out_.kw_per_amp;
      const bool resume_ok = predicted > (out_.target_kw + out_.deadband_kw + p_.resume_margin_kw);
      if (resume_ok) {
        if (resume_ok_since_ms_ == 0) {
          resume_ok_since_ms_ = now ? now : 1;
        } else if (since(now, resume_ok_since_ms_) >= p_.resume_after_ms) {
          out_.paused_low_pv = false;
          out_.charging_allowed = (in.mode != Mode::STOP) && !out_.lv_lockout && !out_.paused_low_soc;
          resume_ok_since_ms_ = 0;
          command(floor_a);
          last_step_ms_ = now;
          event_("resumed pv (%.2f kW)", out_.battery_kw_filtered);
        }
      } else {
        resume_ok_since_ms_ = 0;
      }
    } else {
      resume_ok_since_ms_ = 0;
    }
  } else {
    floor_discharge_since_ms_ = 0;
    resume_ok_since_ms_ = 0;
  }

  // 9. No battery data: never step up blind; after the timeout fall back to the floor once.
  if (out_.blind && auto_mode && p_.blind_timeout_ms > 0 && !blind_floor_done_ &&
      since(now, blind_since_ms_) >= p_.blind_timeout_ms) {
    blind_floor_done_ = true;
    if (setpoint > floor_a) {
      command(floor_a);
      event_("input lost: %dA", floor_a);
    }
  }

  // 10. Outputs.
  out_.setpoint_a = setpoint;
  out_.setpoint_changed = (setpoint != in.setpoint_a);
  last_commanded_a_ = setpoint;
  last_in_ = in;
  return out_;
}

void ChargeController::state_text(char *buf, size_t len) const {
  const Inputs &in = last_in_;
  const int sp = out_.setpoint_a;
  const bool batt_v_ok = finite_(in.battery_v);
  const bool above_cutoff = batt_v_ok && in.battery_v > p_.lv_cutoff_v;
  const int latch_s = static_cast<int>(p_.lv_latch_ms / 1000u);

  if (in.mode == Mode::STOP) {
    if (out_.lv_lockout) {
      snprintf(buf, len, "Stopped - LV lockout %s", above_cutoff ? "latched" : "active");
    } else {
      snprintf(buf, len, "Stopped");
    }
    return;
  }

  const char *profile = (in.mode == Mode::MANUAL) ? "Manual" : (in.mode == Mode::FAST ? "Fast" : "Normal");

  if (out_.lv_lockout) {
    snprintf(buf, len, "%s %s - LV lockout %s", profile, (in.mode == Mode::MANUAL) ? "Stopped" : "Paused",
             above_cutoff ? "latched" : "active");
    return;
  }
  if (in.mode != Mode::MANUAL) {
    if (out_.paused_low_soc) {
      snprintf(buf, len, "%s Paused - House SOC %.0f%%", profile, out_.house_soc);
      return;
    }
    if (out_.paused_low_pv) {
      snprintf(buf, len, "%s Paused - Wait for PV surplus", profile);
      return;
    }
  }

  switch (in.cp_state) {
    case CpState::A_IDLE:
      snprintf(buf, len, "%s - No vehicle", profile);
      return;
    case CpState::B_CONNECTED:
      snprintf(buf, len, "%s Ready %dA - Vehicle connected", profile, sp);
      return;
    case CpState::D_VENT:
      snprintf(buf, len, "%s - Vent not supported", profile);
      return;
    case CpState::F_ERROR:
      snprintf(buf, len, "%s - Pilot error", profile);
      return;
    case CpState::C_CHARGING:
      break;
  }

  const char *blind = (in.mode != Mode::MANUAL && out_.blind) ? " - no battery data" : "";
  if (out_.lv_below_s > 0 && batt_v_ok) {
    snprintf(buf, len, "%s Charging %dA - LV guard %ds/%ds (%.2fV)", profile, sp, out_.lv_below_s, latch_s,
             in.battery_v);
    return;
  }
  if (in.mode != Mode::MANUAL && sp <= p_.floor_a) {
    snprintf(buf, len, "%s Charging %dA (Min)%s", profile, sp, blind);
    return;
  }
  snprintf(buf, len, "%s Charging %dA%s", profile, sp, blind);
}

void ChargeController::debug_text(char *buf, size_t len) const {
  const Inputs &in = last_in_;
  char ages[48];
  int n = 0;
  for (int i = 0; i < MAX_POWER_INPUTS && n < static_cast<int>(sizeof(ages)) - 8; i++) {
    if (!in.primary[i].enabled) {
      continue;
    }
    const uint32_t s = in.primary[i].age_ms == 0xFFFFFFFFu ? 999u : in.primary[i].age_ms / 1000u;
    n += snprintf(ages + n, sizeof(ages) - n, "%s%lu", n ? "/" : "",
                  static_cast<unsigned long>(std::min<uint32_t>(s, 999u)));
  }
  if (n == 0) {
    snprintf(ages, sizeof(ages), "-");
  }
  const uint32_t fb_age = in.fallback.enabled ? std::min<uint32_t>(in.fallback.age_ms / 1000u, 999u) : 999u;

  snprintf(buf, len,
           "mode:%d cp:%d en:%d lock:%d below:%ds cut:%.1fV V:%.1f P:%.2f(%s ages:%s fb:%lu int:%.1fs settle:%lus) F:%.2f "
           "SOC:%.0f/%.0f min:%.0f lim:%dA cap:%dA tgt:%.2f err:%.2f",
           static_cast<int>(in.mode), static_cast<int>(in.cp_state), out_.charging_allowed ? 1 : 0,
           out_.lv_lockout ? 1 : 0, out_.lv_below_s, p_.lv_cutoff_v, finite_(in.battery_v) ? in.battery_v : 0.0f,
           finite_(out_.battery_kw_raw) ? out_.battery_kw_raw : 0.0f, source_name(out_.source), ages,
           static_cast<unsigned long>(fb_age), out_.input_interval_ms / 1000.0f,
           static_cast<unsigned long>(out_.settle_ms / 1000u),
           finite_(out_.battery_kw_filtered) ? out_.battery_kw_filtered : 0.0f,
           finite_(in.soc_pct[0]) ? in.soc_pct[0] : -1.0f, finite_(in.soc_pct[1]) ? in.soc_pct[1] : -1.0f,
           p_.min_soc_pct, out_.soc_limit_a, out_.cap_a, out_.target_kw, finite_(out_.error_kw) ? out_.error_kw : 0.0f);
}

}  // namespace evse_cc
