// Recorded-data loader, sensor-path model and EV/battery plant for the host replay.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace replay {

struct Sample {
  double t;
  float v;  // NaN = unavailable
};

// A recorded Home Assistant entity: sample-and-hold lookup.
struct Series {
  std::vector<Sample> s;
  // Value of the last sample at or before t (NaN before the first sample).
  float at(double t) const {
    if (s.empty() || t < s.front().t) {
      return NAN;
    }
    auto it = std::upper_bound(s.begin(), s.end(), t, [](double x, const Sample &a) { return x < a.t; });
    return (it == s.begin()) ? NAN : (it - 1)->v;
  }
  // Time of the last sample at or before t (-1 if none).
  double time_at(double t) const {
    if (s.empty() || t < s.front().t) {
      return -1;
    }
    auto it = std::upper_bound(s.begin(), s.end(), t, [](double x, const Sample &a) { return x < a.t; });
    return (it == s.begin()) ? -1 : (it - 1)->t;
  }
  size_t count_between(double a, double b) const {
    size_t n = 0;
    for (const auto &x : s) {
      if (x.t >= a && x.t < b) {
        n++;
      }
    }
    return n;
  }
};

// Lines: "epoch metadata_id state". Anything else is skipped.
inline std::map<int, Series> load_data(const char *path, double from, double to) {
  std::map<int, Series> out;
  FILE *f = fopen(path, "r");
  if (f == nullptr) {
    fprintf(stderr, "cannot open %s\n", path);
    exit(2);
  }
  char line[256];
  while (fgets(line, sizeof(line), f) != nullptr) {
    char *end = nullptr;
    const double t = strtod(line, &end);
    if (end == line) {
      continue;
    }
    char *p = end;
    const long id = strtol(p, &end, 10);
    if (end == p) {
      continue;
    }
    p = end;
    while (*p == ' ') {
      p++;
    }
    char *nl = strpbrk(p, "\r\n");
    if (nl != nullptr) {
      *nl = '\0';
    }
    if (strchr(p, ' ') != nullptr) {
      continue;  // extra tokens: not one of our numeric rows
    }
    if (t < from - 3600 || t > to + 60) {
      continue;
    }
    float v = NAN;
    if (strcmp(p, "unavailable") != 0 && strcmp(p, "unknown") != 0) {
      char *vend = nullptr;
      v = strtof(p, &vend);
      if (vend == p) {
        continue;
      }
    }
    out[static_cast<int>(id)].s.push_back({t, v});
  }
  fclose(f);
  for (auto &kv : out) {
    std::sort(kv.second.s.begin(), kv.second.s.end(), [](const Sample &a, const Sample &b) { return a.t < b.t; });
  }
  return out;
}

// Delivery of a true signal to the device: sampled every `period` s, arriving `delay` s later.
// Between a and a+n (stale injection) no samples arrive.
struct SensorPath {
  double period = 1.0;
  double delay = 0.0;
  double stale_from = -1, stale_to = -1;

  // The device-visible value at time t of signal f (sample-and-hold), and its age.
  template <class F> float visible(double t, F f, double &age) const {
    const double t_sampled = std::floor((t - delay) / period) * period;
    double ts = t_sampled;
    if (stale_from >= 0 && ts >= stale_from && ts < stale_to) {
      ts = std::floor((stale_from - 1e-9) / period) * period;  // last sample before the outage
    }
    if (stale_from >= 0 && t >= stale_from + delay && ts < stale_from && t < stale_to + delay) {
      // still inside the outage: nothing newer than the last pre-outage sample
    }
    age = t - (ts + delay);
    if (age < 0) {
      age = 0;
    }
    return f(ts);
  }
};

// Delay line for the car's response to a new offered current.
struct CarModel {
  double delay_s = 2.0;
  float factor = 1.0f;     // actual kW / (A x V/1000), fitted from the recording
  float line_v = 207.0f;
  std::vector<std::pair<double, float>> hist;  // (t, offered kW) history

  void offer(double t, int setpoint_a, bool allowed) {
    const float kw = allowed ? static_cast<float>(setpoint_a) * line_v / 1000.0f * factor : 0.0f;
    hist.push_back({t, kw});
    if (hist.size() > 64) {
      hist.erase(hist.begin());
    }
  }
  float draw(double t) const {
    float kw = 0.0f;
    for (const auto &h : hist) {
      if (h.first <= t - delay_s) {
        kw = h.second;
      }
    }
    return kw;
  }
};

}  // namespace replay
