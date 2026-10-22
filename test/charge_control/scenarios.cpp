// Synthetic scenarios for the charge controller: each drives the controller tick by tick with a
// simple plant (battery = house - car) and checks the outcome.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "evse_charge_control/charge_controller.h"

using namespace evse_cc;

static int failures = 0;
#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                     \
      failures++;                                                                \
    }                                                                            \
  } while (0)

struct Sim {
  ChargeController ctl;
  Params p;
  Inputs in;
  uint32_t t = 0;
  float house_kw = 0.0f;   // battery power with the car off (+ charging)
  float line_v = 207.0f;
  int steps = 0;
  const char *last_event = "";
  char event_buf[64] = {};
  int pending_setpoint = -1;  // models the car seeing the new current 2 s later
  uint32_t pending_at = 0;
  int car_a = 6;
  int applied_a = 6;          // last setpoint handed to the car (ours or the user's)
  uint32_t age_ms = 500;      // age reported for the primary input

  Sim() {
    p.boot_hold_ms = 0;
    ctl.set_params(p);
    in.mode = Mode::FAST;
    in.cp_state = CpState::C_CHARGING;
    in.setpoint_a = 6;
    in.max_current_a = 40;
    in.line_v = line_v;
    in.primary[0].enabled = true;
    in.soc_pct[0] = 80;
    car_a = 6;
  }
  void apply_params() { ctl.set_params(p); }
  const Outputs &tick() {
    t += 1000;
    in.now_ms = t;
    if (in.setpoint_a != applied_a) {  // a user change: the car sees it too
      pending_setpoint = in.setpoint_a;
      pending_at = t + 2000;
      applied_a = in.setpoint_a;
    }
    if (pending_setpoint >= 0 && t >= pending_at) {
      car_a = pending_setpoint;
      pending_setpoint = -1;
    }
    const bool charging = in.cp_state == CpState::C_CHARGING && ctl.outputs().charging_allowed;
    const float ev_kw = charging ? car_a * line_v / 1000.0f : 0.0f;
    in.primary[0].kw = house_kw - ev_kw;
    in.primary[0].age_ms = age_ms;
    const Outputs &o = ctl.update(in);
    in.lockout_clear_request = false;
    if (o.setpoint_changed) {
      steps++;
      in.setpoint_a = o.setpoint_a;
      applied_a = o.setpoint_a;
      pending_setpoint = o.setpoint_a;
      pending_at = t + 2000;
    }
    if (o.has_event) {
      snprintf(event_buf, sizeof(event_buf), "%s", o.event);
      last_event = event_buf;
    }
    return o;
  }
  void run(int seconds) {
    for (int i = 0; i < seconds; i++) tick();
  }
};

static void test_user_override_settles() {
  Sim s;
  s.house_kw = -0.5f;  // house alone discharges 0.5 kW; target -3 -> room for ~12 A
  s.run(5);
  s.in.setpoint_a = 40;  // user jumps to 40 A: battery -> -0.5 - 8.3 = -8.8 kW
  s.steps = 0;
  s.run(120);
  const Outputs &o = s.ctl.outputs();
  printf("  override: %d steps, setpoint %dA, batt %.2f\n", s.steps, o.setpoint_a, o.battery_kw_filtered);
  CHECK(s.steps <= 4);
  CHECK(std::fabs(o.battery_kw_filtered + 3.0f) <= o.deadband_kw + 0.05f);
  CHECK(o.setpoint_a >= 11 && o.setpoint_a <= 14);
}

static void test_normal_ramps_to_max_with_surplus() {
  Sim s;
  s.in.mode = Mode::NORMAL;
  s.house_kw = 9.0f;  // big PV surplus
  s.run(300);
  const Outputs &o = s.ctl.outputs();
  printf("  normal surplus: setpoint %dA batt %.2f\n", o.setpoint_a, o.battery_kw_filtered);
  CHECK(o.setpoint_a == 40);
  CHECK(o.charging_allowed);
}

static void test_normal_holds_about_1kw() {
  Sim s;
  s.in.mode = Mode::NORMAL;
  s.house_kw = 2.0f;  // 2 kW surplus; target -1 -> car should take ~3 kW = ~14 A
  s.run(300);
  const Outputs &o = s.ctl.outputs();
  printf("  normal hold: setpoint %dA batt %.2f\n", o.setpoint_a, o.battery_kw_filtered);
  CHECK(std::fabs(o.battery_kw_filtered + 1.0f) <= o.deadband_kw + 0.05f);
  CHECK(!o.paused_low_pv);
}

static void test_normal_never_pauses_at_floor() {
  Sim s;
  s.in.mode = Mode::NORMAL;
  s.house_kw = -3.0f;  // night, house discharging 3 kW
  s.run(400);
  const Outputs &o = s.ctl.outputs();
  CHECK(o.setpoint_a == 6);
  CHECK(o.charging_allowed);
  CHECK(!o.paused_low_pv);
}

static void test_fast_pause_and_resume() {
  Sim s;
  s.house_kw = -3.5f;  // house alone already over the 3 kW target
  s.run(5);
  CHECK(s.ctl.outputs().setpoint_a == 6);
  s.run(90);  // pause_after 60 s
  CHECK(s.ctl.outputs().paused_low_pv);
  CHECK(!s.ctl.outputs().charging_allowed);
  // The car drops to B while not allowed.
  s.in.cp_state = CpState::B_CONNECTED;
  s.house_kw = -0.5f;  // surplus returns: predicted -0.5 - 1.24 = -1.74 > -3 + 0.31 + 0.5
  s.run(90);
  CHECK(!s.ctl.outputs().paused_low_pv);
  CHECK(s.ctl.outputs().charging_allowed);
  CHECK(s.ctl.outputs().setpoint_a == 6);
}

static void test_user_setpoint_clears_pause() {
  Sim s;
  s.house_kw = -3.5f;
  s.run(100);
  CHECK(s.ctl.outputs().paused_low_pv);
  s.in.setpoint_a = 10;
  s.tick();
  CHECK(!s.ctl.outputs().paused_low_pv);
  CHECK(s.ctl.outputs().charging_allowed);
}

static void test_soc_stop_resume_and_limiter() {
  Sim s;
  s.house_kw = 5.0f;
  s.in.soc_pct[0] = 50;
  s.run(200);
  CHECK(s.ctl.outputs().setpoint_a >= 36);  // 5 kW surplus: 38 A already sits inside the band
  s.in.soc_pct[0] = 25;  // inside the 20-30 % ramp: limit = 6 + 0.5*34 = 23
  s.run(5);
  CHECK(s.ctl.outputs().soc_limiter_active);
  CHECK(s.ctl.outputs().cap_a == 23);
  CHECK(s.ctl.outputs().setpoint_a == 23);
  s.in.soc_pct[0] = 20;
  s.run(3);
  CHECK(s.ctl.outputs().paused_low_soc);
  CHECK(!s.ctl.outputs().charging_allowed);
  s.in.soc_pct[0] = 20.5f;
  s.run(3);
  CHECK(s.ctl.outputs().paused_low_soc);
  s.in.soc_pct[0] = 21;
  s.run(3);
  CHECK(!s.ctl.outputs().paused_low_soc);
  CHECK(s.ctl.outputs().charging_allowed);
  // Two batteries: the lower one counts.
  s.in.soc_pct[0] = 90;
  s.in.soc_pct[1] = 15;
  s.run(3);
  CHECK(s.ctl.outputs().paused_low_soc);
  s.in.soc_pct[1] = NAN;
  s.in.soc_pct[0] = 31.5f;  // release at 31
  s.run(3);
  CHECK(!s.ctl.outputs().soc_limiter_active);
  CHECK(s.ctl.outputs().cap_a == 40);
}

static void test_low_voltage_lockout() {
  Sim s;
  s.p.lv_cutoff_v = 47.0f;
  s.apply_params();
  s.house_kw = -0.5f;
  s.in.battery_v = 48.0f;
  s.run(10);
  CHECK(s.ctl.outputs().charging_allowed);
  s.in.battery_v = 46.5f;
  s.run(20);
  CHECK(!s.ctl.outputs().lv_lockout);
  CHECK(s.ctl.outputs().lv_below_s >= 19);
  s.run(15);
  CHECK(s.ctl.outputs().lv_lockout);
  CHECK(!s.ctl.outputs().charging_allowed);
  // Clear refused while still below the cutoff.
  s.in.lockout_clear_request = true;
  s.tick();
  CHECK(s.ctl.outputs().lv_lockout);
  CHECK(strncmp(s.last_event, "lockout clear blocked", 21) == 0);
  // Voltage recovers but the lockout stays latched until cleared.
  s.in.battery_v = 48.5f;
  s.run(20);
  CHECK(s.ctl.outputs().lv_lockout);
  s.in.lockout_clear_request = true;
  s.tick();
  CHECK(!s.ctl.outputs().lv_lockout);
  CHECK(s.ctl.outputs().charging_allowed);
  // Lockout survives restore(), cutoff 0 disables it.
  ChargeController c2;
  c2.set_params(s.p);
  c2.restore(true);
  Inputs in = s.in;
  in.now_ms = 1000;
  c2.update(in);
  CHECK(c2.outputs().lv_lockout);
  Params p0 = s.p;
  p0.lv_cutoff_v = 0;
  c2.set_params(p0);
  in.now_ms = 2000;
  c2.update(in);
  CHECK(!c2.outputs().lv_lockout);
}

static void test_mode_and_vehicle_transitions() {
  Sim s;
  s.house_kw = 2.0f;
  s.run(200);
  CHECK(s.ctl.outputs().setpoint_a > 6);
  s.in.cp_state = CpState::A_IDLE;
  s.steps = 0;
  s.run(3);
  CHECK(s.ctl.outputs().setpoint_a == 6);
  CHECK(s.steps == 1);  // reset to the floor once
  s.run(30);
  CHECK(s.steps == 1);
  char buf[96];
  s.ctl.state_text(buf, sizeof(buf));
  CHECK(strcmp(buf, "Fast - No vehicle") == 0);
  s.in.cp_state = CpState::B_CONNECTED;
  s.tick();
  s.ctl.state_text(buf, sizeof(buf));
  CHECK(strcmp(buf, "Fast Ready 6A - Vehicle connected") == 0);
  s.in.cp_state = CpState::C_CHARGING;
  s.run(60);
  s.ctl.state_text(buf, sizeof(buf));
  printf("  state text after reconnect: %s\n", buf);
  CHECK(strncmp(buf, "Fast Charging ", 14) == 0);
  s.in.mode = Mode::STOP;
  s.tick();
  CHECK(!s.ctl.outputs().charging_allowed);
  s.ctl.state_text(buf, sizeof(buf));
  CHECK(strcmp(buf, "Stopped") == 0);
  s.in.mode = Mode::MANUAL;
  s.tick();
  CHECK(s.ctl.outputs().charging_allowed);
  s.ctl.state_text(buf, sizeof(buf));
  CHECK(strncmp(buf, "Manual Charging ", 16) == 0);
  s.in.mode = Mode::NORMAL;
  s.tick();
  CHECK(strcmp(s.last_event, "mode Normal") == 0);
}

static void test_boot_hold() {
  Sim s;
  s.p.boot_hold_ms = 30000;
  s.apply_params();
  s.house_kw = 5.0f;
  s.run(25);
  CHECK(s.steps == 0);
  s.run(20);
  CHECK(s.steps >= 1);
}

static void test_blind_hold_then_floor() {
  Sim s;
  s.house_kw = 2.0f;
  s.run(200);
  const int before = s.ctl.outputs().setpoint_a;
  CHECK(before > 6);
  s.age_ms = 60000;  // stale, no fallback
  s.steps = 0;
  s.run(300);
  CHECK(s.ctl.outputs().blind);
  CHECK(s.steps == 0);
  CHECK(s.ctl.outputs().setpoint_a == before);
  s.age_ms = 0;  // back
  s.run(3);
  CHECK(!s.ctl.outputs().blind);
  s.age_ms = 60000;
  s.run(650);
  CHECK(s.ctl.outputs().setpoint_a == 6);
  CHECK(strcmp(s.last_event, "input lost: 6A") == 0);
  // Fallback keeps it running.
  Sim f;
  f.house_kw = 2.0f;
  f.in.fallback.enabled = true;
  f.run(200);
  f.age_ms = 60000;
  for (int i = 0; i < 100; i++) {
    f.in.fallback.kw = f.in.primary[0].kw;
    f.in.fallback.age_ms = 10000;
    f.tick();
  }
  CHECK(f.ctl.outputs().source == Source::FALLBACK);
  CHECK(!f.ctl.outputs().blind);
}

static void test_cap_enforced_immediately() {
  Sim s;
  s.house_kw = 9.0f;
  s.run(200);
  CHECK(s.ctl.outputs().setpoint_a == 40);
  s.in.max_current_a = 20;
  s.tick();
  CHECK(s.ctl.outputs().setpoint_a == 20);
  CHECK(strcmp(s.last_event, "cap 20A") == 0);
}

static void test_cloud_dip_corrected_in_one_step() {
  Sim s;
  s.house_kw = 4.0f;
  s.run(300);
  const int before = s.ctl.outputs().setpoint_a;
  s.house_kw = 3.0f;  // 1 kW cloud
  s.steps = 0;
  s.run(40);
  printf("  cloud dip: %dA -> %dA in %d steps\n", before, s.ctl.outputs().setpoint_a, s.steps);
  CHECK(s.steps <= 2);
  CHECK(before - s.ctl.outputs().setpoint_a >= 3);
}

int main() {
  printf("override settles\n"); test_user_override_settles();
  printf("normal ramps to max\n"); test_normal_ramps_to_max_with_surplus();
  printf("normal holds ~1 kW\n"); test_normal_holds_about_1kw();
  printf("normal never pauses\n"); test_normal_never_pauses_at_floor();
  printf("fast pause/resume\n"); test_fast_pause_and_resume();
  printf("user setpoint clears pause\n"); test_user_setpoint_clears_pause();
  printf("soc stop/resume/limiter\n"); test_soc_stop_resume_and_limiter();
  printf("low voltage lockout\n"); test_low_voltage_lockout();
  printf("mode and vehicle transitions\n"); test_mode_and_vehicle_transitions();
  printf("boot hold\n"); test_boot_hold();
  printf("blind hold then floor\n"); test_blind_hold_then_floor();
  printf("cap enforced\n"); test_cap_enforced_immediately();
  printf("cloud dip\n"); test_cloud_dip_corrected_in_one_step();
  if (failures) {
    printf("%d FAILURES\n", failures);
    return 1;
  }
  printf("ALL PASS\n");
  return 0;
}
