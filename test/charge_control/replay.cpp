// Replays recorded Home Assistant data through the charge controller.
//
// Plant: other(t) = recorded battery power + recorded EV power (what the house battery would
// see with the car off); the simulated car draws setpoint x V/1000 x factor with a 2 s lag;
// batt_sim = other - ev_sim. The controller sees batt_sim through a model of the sensor path.
//
//   replay.exe --data data\umauma-2026-10-21.txt --from 1792644600 --to 1792701000 --mode fast
//              --voltage 207 --path fast --report night=1792658700:1792684800 day=1792684800:1792701000
//   --path slow            one input, 15 s samples, 20 s delay (the old inverter-total path)
//   --stale A:N            primaries stop updating for N s from epoch A (fallback takes over)
//   --no-fallback          no fallback input (stale -> blind)
//   --law old              the previous +-1 A law, for validating the plant against the recording
//   --cooldown 12,T:40     old law: 12 s cooldown until epoch T, then 40 s
//   --target KW            override the mode's target
//   --trace file.csv       per-second trace
//   --verbose              print every event
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "evse_charge_control/charge_controller.h"
#include "plant.h"

using evse_cc::ChargeController;
using evse_cc::CpState;
using evse_cc::Inputs;
using evse_cc::Mode;
using evse_cc::Outputs;
using evse_cc::Params;

namespace {

constexpr int ID_CK0 = 11229, ID_CK1 = 9125, ID_EV = 3291, ID_SP = 4315, ID_INV = 6694, ID_SOC1 = 12136,
              ID_SOC2 = 12137;

struct Window {
  std::string name;
  double a, b;
};

struct Args {
  const char *data = nullptr;
  double from = 0, to = 0;
  std::string mode = "fast";
  float target = NAN;
  float voltage = 207.0f;
  std::string path = "fast";
  double stale_from = -1, stale_len = 0;
  bool no_fallback = false;
  std::string law = "new";
  double cooldown_a = 12, cooldown_switch_t = -1, cooldown_b = 40;
  std::vector<Window> windows;
  const char *trace = nullptr;
  bool verbose = false;
  float car_factor = NAN;
  int start_a = -1;
  std::vector<std::pair<std::string, double>> sets;  // --set name=value parameter overrides
};

// Apply --set overrides to the controller parameters (names as in evse_cc::Params).
void apply_set(Params &p, const std::string &k, double v) {
  if (k == "deadband") p.deadband_kw = static_cast<float>(v);
  else if (k == "deadband_amps") p.deadband_amps = static_cast<float>(v);
  else if (k == "gain") p.gain = static_cast<float>(v);
  else if (k == "max_step_up") p.max_step_up_a = static_cast<int>(v);
  else if (k == "max_step_down") p.max_step_down_a = static_cast<int>(v);
  else if (k == "settle") p.settle_ms = static_cast<uint32_t>(v * 1000);
  else if (k == "settle_fallback") p.settle_fallback_ms = static_cast<uint32_t>(v * 1000);
  else if (k == "settle_intervals") p.settle_intervals = static_cast<float>(v);
  else if (k == "evidence") p.evidence_samples = static_cast<int>(v);
  else if (k == "alpha") p.filter_alpha = static_cast<float>(v);
  else if (k == "alpha_fallback") p.filter_alpha_fallback = static_cast<float>(v);
  else if (k == "pause_after") p.pause_after_ms = static_cast<uint32_t>(v * 1000);
  else if (k == "resume_after") p.resume_after_ms = static_cast<uint32_t>(v * 1000);
  else if (k == "resume_margin") p.resume_margin_kw = static_cast<float>(v);
  else if (k == "stale") p.stale_ms = static_cast<uint32_t>(v * 1000);
  else if (k == "blind_timeout") p.blind_timeout_ms = static_cast<uint32_t>(v * 1000);
  else { fprintf(stderr, "unknown --set %s\n", k.c_str()); exit(2); }
}

Args parse(int argc, char **argv) {
  Args a;
  for (int i = 1; i < argc; i++) {
    std::string k = argv[i];
    auto next = [&]() -> const char * { return (i + 1 < argc) ? argv[++i] : ""; };
    if (k == "--data") a.data = next();
    else if (k == "--from") a.from = atof(next());
    else if (k == "--to") a.to = atof(next());
    else if (k == "--mode") a.mode = next();
    else if (k == "--target") a.target = static_cast<float>(atof(next()));
    else if (k == "--voltage") a.voltage = static_cast<float>(atof(next()));
    else if (k == "--path") a.path = next();
    else if (k == "--stale") { const char *s = next(); a.stale_from = atof(s); const char *c = strchr(s, ':'); a.stale_len = c ? atof(c + 1) : 60; }
    else if (k == "--no-fallback") a.no_fallback = true;
    else if (k == "--law") a.law = next();
    else if (k == "--cooldown") { const char *s = next(); a.cooldown_a = atof(s); const char *c = strchr(s, ','); if (c) { a.cooldown_switch_t = atof(c + 1); const char *d = strchr(c, ':'); if (d) a.cooldown_b = atof(d + 1); } }
    else if (k == "--report") {
      while (i + 1 < argc && strncmp(argv[i + 1], "--", 2) != 0) {
        std::string w = argv[++i];
        auto eq = w.find('='), col = w.find(':');
        a.windows.push_back({w.substr(0, eq), atof(w.substr(eq + 1, col - eq - 1).c_str()), atof(w.substr(col + 1).c_str())});
      }
    }
    else if (k == "--trace") a.trace = next();
    else if (k == "--verbose") a.verbose = true;
    else if (k == "--car-factor") a.car_factor = static_cast<float>(atof(next()));
    else if (k == "--start") a.start_a = atoi(next());
    else if (k == "--set") { std::string s = next(); auto eq = s.find('='); a.sets.push_back({s.substr(0, eq), atof(s.substr(eq + 1).c_str())}); }
    else { fprintf(stderr, "unknown arg %s\n", argv[i]); exit(2); }
  }
  if (a.data == nullptr || a.to <= a.from) {
    fprintf(stderr, "need --data, --from, --to\n");
    exit(2);
  }
  if (a.windows.empty()) {
    a.windows.push_back({"all", a.from, a.to});
  }
  return a;
}

// The old firmware's 5 s PV-auto law (Fast/Normal), without the pause/resume and SOC parts.
struct OldLaw {
  float filtered = 0; bool seeded = false;
  int discharge_samples = 0, surplus_samples = 0;
  double last_step = -1e9;
  double next_tick = 0;
  int update(double t, float raw_kw, int setpoint, bool fast, float target_kw, double cooldown_s, bool &stepped) {
    stepped = false;
    if (t < next_tick) return setpoint;
    next_tick = t + 5;
    if (std::isnan(raw_kw)) return setpoint;
    filtered = seeded ? 0.30f * raw_kw + 0.70f * filtered : raw_kw;
    seeded = true;
    const float limit = -target_kw;
    const float discharge = filtered < 0 ? -filtered : 0;
    discharge_samples = (discharge > limit) ? discharge_samples + 1 : 0;
    const bool allow_inc = fast ? (discharge < limit - 0.10f) : (filtered > 0.20f);
    surplus_samples = allow_inc ? surplus_samples + 1 : 0;
    if (t - last_step < cooldown_s) return setpoint;
    int delta = 0;
    if (discharge > limit + 0.30f && discharge_samples >= 2) delta = -2;
    else if (discharge > limit && discharge_samples >= 2) delta = -1;
    else if (surplus_samples >= 4) delta = 1;
    if (delta == 0) return setpoint;
    int n = std::min(40, std::max(6, setpoint + delta));
    if (n == setpoint) return setpoint;
    last_step = t;
    stepped = true;
    return n;
  }
};

struct Stats {
  int steps = 0; double abs_step = 0; int ticks = 0, in_band = 0;
  float max_excursion = 0; double excursion_s = 0;
  int pauses = 0, resumes = 0; double paused_s = 0, floor_s = 0;
  double ev_kwh_sim = 0, ev_kwh_rec = 0;
  std::vector<float> b30, o30; float acc_b = 0, acc_o = 0; int acc_n = 0;
  double sum_abs_err = 0;
};

float variance(const std::vector<float> &x) {
  if (x.empty()) return 0;
  double m = 0; for (float v : x) m += v; m /= x.size();
  double s = 0; for (float v : x) s += (v - m) * (v - m);
  return static_cast<float>(s / x.size());
}

}  // namespace

int main(int argc, char **argv) {
  Args a = parse(argc, argv);
  auto data = replay::load_data(a.data, a.from, a.to);
  const replay::Series &ck0 = data[ID_CK0], &ck1 = data[ID_CK1], &ev = data[ID_EV], &sp = data[ID_SP],
                       &inv = data[ID_INV], &soc1 = data[ID_SOC1], &soc2 = data[ID_SOC2];
  if (ck0.s.empty() || ev.s.empty() || sp.s.empty()) {
    fprintf(stderr, "missing series in data (ck0 %zu, ev %zu, sp %zu)\n", ck0.s.size(), ev.s.size(), sp.s.size());
    return 2;
  }

  // True battery power with the car off.
  auto batt_rec = [&](double t) { float v = ck0.at(t) + ck1.at(t); return v; };
  auto other = [&](double t) { return batt_rec(t) + std::max(0.0f, ev.at(t)); };

  // Fit the car factor: actual kW / (offered A x V/1000) while charging.
  float factor = a.car_factor;
  if (std::isnan(factor)) {
    std::vector<float> r;
    for (double t = a.from; t < a.to; t += 10) {
      const float e = ev.at(t), s = sp.at(t - 5);
      if (e > 0.5f && s >= 6) r.push_back(e / (s * a.voltage / 1000.0f));
    }
    std::sort(r.begin(), r.end());
    factor = r.empty() ? 1.0f : r[r.size() / 2];
  }
  printf("car factor %.3f (%s)\n", factor, std::isnan(a.car_factor) ? "fitted" : "given");

  const bool fast = (a.mode == "fast");
  Params p;
  const float target = std::isnan(a.target) ? (fast ? p.target_fast_kw : p.target_normal_kw) : a.target;
  p.target_fast_kw = fast ? target : p.target_fast_kw;
  p.target_normal_kw = fast ? p.target_normal_kw : target;
  p.nominal_line_v = a.voltage;
  p.boot_hold_ms = 0;
  for (const auto &kv : a.sets) apply_set(p, kv.first, kv.second);

  // Sensor paths.
  replay::SensorPath fast0{0.4, 0.5}, fast1{0.9, 0.5}, slow{15.0, 20.0};
  if (a.stale_from >= 0) {
    fast0.stale_from = fast1.stale_from = a.stale_from;
    fast0.stale_to = fast1.stale_to = a.stale_from + a.stale_len;
    if (a.path == "slow") { slow.stale_from = a.stale_from; slow.stale_to = a.stale_from + a.stale_len; }
  }

  // Simulation state.
  std::vector<std::pair<double, float>> batt_hist;  // (t, batt_sim) for the sensor paths to sample
  batt_hist.reserve(static_cast<size_t>(a.to - a.from) + 10);
  auto batt_sim_at = [&](double t) -> float {
    // sample-and-hold lookup into the simulated battery history
    if (batt_hist.empty() || t < batt_hist.front().first) return other(t);
    auto it = std::upper_bound(batt_hist.begin(), batt_hist.end(), t,
                               [](double x, const std::pair<double, float> &e) { return x < e.first; });
    return (it == batt_hist.begin()) ? other(t) : (it - 1)->second;
  };

  replay::CarModel car;
  car.factor = factor;
  car.line_v = a.voltage;
  ChargeController ctl;
  ctl.set_params(p);
  OldLaw old;
  int setpoint = a.start_a > 0 ? a.start_a : static_cast<int>(std::lround(sp.at(a.from)));
  if (setpoint < 6) setpoint = 6;
  bool allowed = true;

  FILE *trace = a.trace ? fopen(a.trace, "w") : nullptr;
  if (trace) fprintf(trace, "t,other,ev_sim,batt_sim,batt_seen,filtered,setpoint,allowed,source,event\n");

  std::vector<Stats> st(a.windows.size());
  bool was_paused = false;
  (void) was_paused;

  for (double t = a.from; t < a.to; t += 1.0) {
    const float ev_sim = car.draw(t);
    const float batt_sim = other(t) - ev_sim;
    batt_hist.push_back({t, batt_sim});

    Inputs in;
    in.now_ms = static_cast<uint32_t>((t - a.from) * 1000.0);
    in.mode = fast ? Mode::FAST : Mode::NORMAL;
    in.cp_state = CpState::C_CHARGING;
    in.setpoint_a = setpoint;
    in.max_current_a = 40;
    in.line_v = a.voltage;
    in.soc_pct[0] = soc1.at(t);
    in.soc_pct[1] = soc2.at(t);

    float seen = NAN;
    double age0 = 0, age1 = 0, age_fb = 0;
    const float share = [&] { float c0 = ck0.at(t), c1 = ck1.at(t); float d = c0 + c1; return (std::fabs(d) > 0.05f) ? c0 / d : 0.5f; }();
    if (a.path == "fast") {
      in.primary[0].enabled = true;
      in.primary[0].kw = fast0.visible(t, [&](double ts) { return batt_sim_at(ts) * share; }, age0);
      in.primary[0].age_ms = static_cast<uint32_t>(age0 * 1000);
      in.primary[1].enabled = true;
      in.primary[1].kw = fast1.visible(t, [&](double ts) { return batt_sim_at(ts) * (1.0f - share); }, age1);
      in.primary[1].age_ms = static_cast<uint32_t>(age1 * 1000);
      if (!a.no_fallback) {
        in.fallback.enabled = true;
        in.fallback.kw = slow.visible(t, [&](double ts) { return batt_sim_at(ts); }, age_fb);
        in.fallback.age_ms = static_cast<uint32_t>(age_fb * 1000);
      }
      seen = in.primary[0].kw + in.primary[1].kw;
    } else {
      in.primary[0].enabled = true;
      in.primary[0].kw = slow.visible(t, [&](double ts) { return batt_sim_at(ts); }, age0);
      in.primary[0].age_ms = static_cast<uint32_t>(age0 * 1000);
      seen = in.primary[0].kw;
    }
    (void) inv;

    bool stepped = false;
    const char *event = "";
    float filtered = NAN;
    const char *source = "";
    if (a.law == "old") {
      const double cd = (a.cooldown_switch_t > 0 && t >= a.cooldown_switch_t) ? a.cooldown_b : a.cooldown_a;
      const int n = old.update(t, seen, setpoint, fast, target, cd, stepped);
      if (stepped) { setpoint = n; }
      filtered = old.filtered;
      source = "old";
    } else {
      const Outputs &o = ctl.update(in);
      if (o.setpoint_changed) { stepped = true; setpoint = o.setpoint_a; }
      allowed = o.charging_allowed;
      filtered = o.battery_kw_filtered;
      source = ChargeController::source_name(o.source);
      if (o.has_event) {
        event = o.event;
        if (a.verbose) printf("%10.0f  %s\n", t, o.event);
      }
      was_paused = o.paused_low_pv;
    }
    car.offer(t, setpoint, allowed);

    // Metrics.
    for (size_t w = 0; w < a.windows.size(); w++) {
      if (t < a.windows[w].a || t >= a.windows[w].b) continue;
      Stats &s = st[w];
      s.ticks++;
      if (stepped) { s.steps++; s.abs_step += std::fabs(static_cast<float>(setpoint - in.setpoint_a)); }
      const float err = batt_sim - target;
      s.sum_abs_err += std::fabs(err);
      if (std::fabs(err) <= 0.3f) s.in_band++;
      const float exc = -err;  // positive = discharging harder than the target
      if (exc > s.max_excursion) s.max_excursion = exc;
      if (exc > 0.3f) s.excursion_s += 1;
      if (!allowed) s.paused_s += 1;
      if (allowed && setpoint <= 6) s.floor_s += 1;
      s.ev_kwh_sim += ev_sim / 3600.0;
      s.ev_kwh_rec += std::max(0.0f, ev.at(t)) / 3600.0;
      s.acc_b += batt_sim; s.acc_o += other(t); s.acc_n++;
      if (s.acc_n == 30) { s.b30.push_back(s.acc_b / 30); s.o30.push_back(s.acc_o / 30); s.acc_b = s.acc_o = 0; s.acc_n = 0; }
      if (event[0] && strncmp(event, "paused", 6) == 0) s.pauses++;
      if (event[0] && strncmp(event, "resumed pv", 10) == 0) s.resumes++;
    }

    if (trace) fprintf(trace, "%.0f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%d,%s,%s\n", t, other(t), ev_sim, batt_sim, seen, filtered, setpoint, allowed ? 1 : 0, source, event);
  }
  if (trace) fclose(trace);

  printf("%-8s %7s %7s %7s %8s %8s %6s %7s %7s %8s %8s %7s\n", "window", "steps/h", "avgstep", "inband%", "maxexc", "exc>0.3s", "pauses", "paused", "floor", "evkWh", "reckWh", "varrat");
  for (size_t w = 0; w < a.windows.size(); w++) {
    const Stats &s = st[w];
    if (s.ticks == 0) continue;
    const double hours = s.ticks / 3600.0;
    const float vr = variance(s.o30) > 0 ? variance(s.b30) / variance(s.o30) : 0;
    printf("%-8s %7.1f %7.2f %7.1f %8.2f %8.0f %6d %7.0f %7.0f %8.2f %8.2f %7.2f\n", a.windows[w].name.c_str(), s.steps / hours,
           s.steps ? s.abs_step / s.steps : 0.0, 100.0 * s.in_band / s.ticks, s.max_excursion, s.excursion_s, s.pauses,
           s.paused_s / 60, s.floor_s / 60, s.ev_kwh_sim, s.ev_kwh_rec, vr);
    printf("CSV,%s,%s,%s,%.1f,%.1f,%.2f,%d,%.2f,%.2f,%.2f\n", a.law.c_str(), a.path.c_str(), a.windows[w].name.c_str(),
           s.steps / hours, 100.0 * s.in_band / s.ticks, s.max_excursion, s.pauses, s.ev_kwh_sim, s.ev_kwh_rec, vr);
  }
  // Recorded reference for the same windows.
  for (size_t w = 0; w < a.windows.size(); w++) {
    const Window &win = a.windows[w];
    int inband = 0, n = 0; float maxexc = 0;
    for (double t = win.a; t < win.b; t += 1.0) {
      const float b = batt_rec(t);
      if (std::isnan(b)) continue;
      n++;
      if (std::fabs(b - target) <= 0.3f) inband++;
      maxexc = std::max(maxexc, -(b - target));
    }
    const double hours = (win.b - win.a) / 3600.0;
    if (n) printf("REC,%s,steps/h %.1f, inband %.1f%%, maxexc %.2f\n", win.name.c_str(), sp.count_between(win.a, win.b) / hours, 100.0 * inband / n, maxexc);
  }
  return 0;
}
