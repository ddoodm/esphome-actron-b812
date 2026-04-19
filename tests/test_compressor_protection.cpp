#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Fake millis() — controls time for the component under test.
// Must be defined before including the component (the linker resolves calls
// from actron_b812.cpp to this definition).
// ---------------------------------------------------------------------------
static uint32_t s_fake_millis = 0;

namespace esphome {
uint32_t millis() { return s_fake_millis; }
}  // namespace esphome

#include "actron_b812.hpp"

using namespace esphome;
using namespace esphome::actron_b812;

// ---------------------------------------------------------------------------
// TestableClimate — exposes protected members for white-box testing
// ---------------------------------------------------------------------------
class TestableClimate : public ActronB812Climate {
 public:
  using ActronB812Climate::active_cmd_;
  using ActronB812Climate::comp_running_;
  using ActronB812Climate::comp_timer_armed_;
  using ActronB812Climate::valve_timer_armed_;
  using ActronB812Climate::pending_change_;
  using ActronB812Climate::pending_mode_;
  using ActronB812Climate::thermostat_direction_;
  using ActronB812Climate::comp_off_time_;
  using ActronB812Climate::valve_switch_time_;
  using ActronB812Climate::comp_cooldown_ms_;
  using ActronB812Climate::valve_settle_ms_;
  using ActronB812Climate::evaluate_thermostat_;
  using ActronB812Climate::update_action_;
};

// ---------------------------------------------------------------------------
// Frame history — one entry per update() tick, for post-hoc invariant checking
// ---------------------------------------------------------------------------
struct Frame {
  uint32_t time_ms;
  uint8_t cmd;
  bool comp_running;
};

static std::string cmd_hex(uint8_t v) {
  char buf[8];
  snprintf(buf, sizeof(buf), "0x%02X", v);
  return buf;
}

// ---------------------------------------------------------------------------
// Harness — drives the component through scenarios and checks invariants
// ---------------------------------------------------------------------------
static constexpr uint32_t COOLDOWN_MS = 180'000;
static constexpr uint32_t SETTLE_MS = 30'000;
static constexpr uint32_t TICK_MS = 222;

class Harness {
 public:
  TestableClimate climate;
  remote_transmitter::RemoteTransmitterComponent tx;
  std::vector<Frame> frames;

  Harness() {
    s_fake_millis = 10'000;  // non-zero start to catch unsigned underflow bugs
    climate.set_transmitter(&tx);
    climate.set_compressor_cooldown(COOLDOWN_MS);
    climate.set_valve_settle_time(SETTLE_MS);
    climate.set_hysteresis(0.5f);
    climate.target_temperature = 22.0f;
  }

  void set_mode(climate::ClimateMode mode) {
    climate::ClimateCall call;
    call.set_mode(mode);
    climate.control(call);
  }

  void tick(uint32_t ms = TICK_MS) {
    s_fake_millis += ms;
    climate.update();
    frames.push_back({s_fake_millis, climate.active_cmd_, climate.comp_running_});
  }

  void run_for(uint32_t total_ms) {
    uint32_t start = s_fake_millis;
    while (s_fake_millis - start < total_ms)
      tick();
  }

  // Simulate a temperature sensor callback: update the reading and let the
  // thermostat re-evaluate (exactly what the real sensor callback does).
  void set_temperature(float t) {
    climate.current_temperature = t;
    climate.evaluate_thermostat_();
    climate.update_action_();
  }

  // ------------------------------------------------------------------
  // Post-hoc invariant checker — scans the full frame history and fails
  // on any violation.  Timing-based, so it doesn't rely on internal flags.
  //
  //  1. Valve (HEAT bit) never toggles while the compressor was running
  //  2. Valve never toggles during the compressor cooldown window
  //  3. Compressor never starts before the cooldown has elapsed
  //  4. Compressor never starts before the valve settle time (Heat→Cool)
  // ------------------------------------------------------------------
  void check_invariants() {
    bool had_comp_run = false;
    uint32_t comp_stop_time = 0;

    bool prev_comp = false;
    bool prev_heat = false;
    bool first = true;

    uint32_t last_heat_off_time = 0;
    bool awaiting_valve_settle = false;

    for (auto &f : frames) {
      bool comp = f.comp_running;
      bool heat = (f.cmd & BIT_HEAT) != 0;

      if (!first) {
        // --- Valve (HEAT bit) just changed? ---
        if (heat != prev_heat) {
          INFO("t=" << f.time_ms << "ms: HEAT "
                    << (prev_heat ? "ON" : "OFF") << " -> "
                    << (heat ? "ON" : "OFF")
                    << "  cmd=" << cmd_hex(f.cmd));

          // Invariant 1: must not switch while compressor was already on
          CHECK_FALSE(prev_comp);

          // Invariant 2: must not switch during cooldown
          if (had_comp_run) {
            uint32_t elapsed = f.time_ms - comp_stop_time;
            INFO("  elapsed since comp stop: " << elapsed
                  << "ms  (cooldown=" << COOLDOWN_MS << "ms)");
            CHECK(elapsed >= COOLDOWN_MS);
          }

          // Track Heat→Cool valve switch for invariant 4
          if (prev_heat && !heat) {
            last_heat_off_time = f.time_ms;
            awaiting_valve_settle = true;
          }
        }

        // --- Compressor just started? ---
        if (!prev_comp && comp) {
          INFO("t=" << f.time_ms << "ms: compressor STARTED  cmd=" << cmd_hex(f.cmd));

          // Invariant 3: must wait for cooldown
          if (had_comp_run) {
            uint32_t elapsed = f.time_ms - comp_stop_time;
            INFO("  elapsed since comp stop: " << elapsed
                  << "ms  (cooldown=" << COOLDOWN_MS << "ms)");
            CHECK(elapsed >= COOLDOWN_MS);
          }

          // Invariant 4: must wait for valve settle after Heat→Cool switch
          if (awaiting_valve_settle) {
            uint32_t elapsed = f.time_ms - last_heat_off_time;
            INFO("  elapsed since valve switch: " << elapsed
                  << "ms  (settle=" << SETTLE_MS << "ms)");
            CHECK(elapsed >= SETTLE_MS);
            awaiting_valve_settle = false;
          }
        }

        // --- Compressor just stopped? ---
        if (prev_comp && !comp) {
          comp_stop_time = f.time_ms;
          had_comp_run = true;
        }
      }

      prev_comp = comp;
      prev_heat = heat;
      first = false;
    }
  }
};

// ===========================================================================
// Test cases
// ===========================================================================

TEST_CASE("Heat-to-cool: all four protections hold", "[protection]") {
  Harness h;

  // --- Phase 1: establish HEAT with compressor running ---
  h.climate.current_temperature = 19.0f;  // well below target (22)
  h.set_mode(climate::CLIMATE_MODE_HEAT);
  h.run_for(2000);

  REQUIRE(h.climate.comp_running_);
  REQUIRE((h.climate.active_cmd_ & BIT_HEAT) != 0);
  REQUIRE((h.climate.active_cmd_ & BIT_COMP) != 0);

  // --- Phase 2: switch to COOL ---
  h.climate.current_temperature = 25.0f;  // well above target
  h.set_mode(climate::CLIMATE_MODE_COOL);

  // Run through the full cooldown + valve settle + extra
  h.run_for(COOLDOWN_MS + SETTLE_MS + 5000);

  // Compressor should now be running in cool mode
  REQUIRE(h.climate.comp_running_);
  REQUIRE((h.climate.active_cmd_ & BIT_HEAT) == 0);
  REQUIRE((h.climate.active_cmd_ & BIT_COMP) != 0);

  h.check_invariants();
}

TEST_CASE("Compressor cooldown respected on simple restart", "[protection]") {
  Harness h;

  // Start cooling
  h.climate.current_temperature = 25.0f;
  h.set_mode(climate::CLIMATE_MODE_COOL);
  h.run_for(2000);
  REQUIRE(h.climate.comp_running_);

  // Turn off
  h.set_mode(climate::CLIMATE_MODE_OFF);
  h.tick();
  REQUIRE_FALSE(h.climate.comp_running_);

  // Immediately request COOL again
  h.climate.current_temperature = 25.0f;
  h.set_mode(climate::CLIMATE_MODE_COOL);

  // During cooldown the compressor must stay off
  h.run_for(COOLDOWN_MS - 2000);
  CHECK_FALSE(h.climate.comp_running_);

  // After cooldown it should restart
  h.run_for(5000);
  REQUIRE(h.climate.comp_running_);

  h.check_invariants();
}

TEST_CASE("Cool-to-heat: cooldown but no valve settle required", "[protection]") {
  Harness h;

  // Start cooling
  h.climate.current_temperature = 25.0f;
  h.set_mode(climate::CLIMATE_MODE_COOL);
  h.run_for(2000);
  REQUIRE(h.climate.comp_running_);
  REQUIRE((h.climate.active_cmd_ & BIT_HEAT) == 0);

  // Switch to HEAT
  h.climate.current_temperature = 19.0f;
  h.set_mode(climate::CLIMATE_MODE_HEAT);

  // Compressor should not restart until cooldown elapses...
  h.run_for(COOLDOWN_MS - 2000);
  CHECK_FALSE(h.climate.comp_running_);

  // ...but it should NOT need the extra valve settle time (Cool→Heat direction)
  h.run_for(5000);
  REQUIRE(h.climate.comp_running_);
  REQUIRE((h.climate.active_cmd_ & BIT_HEAT) != 0);

  // Verify the compressor restarted BEFORE cooldown + settle would have elapsed,
  // confirming no valve settle was applied in this direction.
  uint32_t restart_time = 0;
  for (auto &f : h.frames) {
    if (f.comp_running) {
      restart_time = f.time_ms;
    }
  }
  // Find the comp stop time
  bool was_running = false;
  uint32_t stop_time = 0;
  for (auto &f : h.frames) {
    if (was_running && !f.comp_running)
      stop_time = f.time_ms;
    was_running = f.comp_running;
  }
  uint32_t actual_delay = restart_time - stop_time;
  CHECK(actual_delay < COOLDOWN_MS + SETTLE_MS);

  h.check_invariants();
}

TEST_CASE("Valve held during cooldown when switching heat to off", "[protection]") {
  Harness h;

  // Establish HEAT mode
  h.climate.current_temperature = 19.0f;
  h.set_mode(climate::CLIMATE_MODE_HEAT);
  h.run_for(2000);
  REQUIRE(h.climate.comp_running_);
  REQUIRE((h.climate.active_cmd_ & BIT_HEAT) != 0);

  // Switch to OFF
  h.set_mode(climate::CLIMATE_MODE_OFF);
  h.tick();
  REQUIRE_FALSE(h.climate.comp_running_);

  // HEAT bit must stay set during cooldown (protects reversing valve)
  h.run_for(COOLDOWN_MS / 2);
  CHECK((h.climate.active_cmd_ & BIT_HEAT) != 0);

  // After cooldown elapses, HEAT should clear (mode is OFF, no reason to hold)
  h.run_for(COOLDOWN_MS);
  CHECK((h.climate.active_cmd_ & BIT_HEAT) == 0);

  h.check_invariants();
}

TEST_CASE("Regression: switching COOL->HEAT does not start cooling", "[regression]") {
  // Bug: thermostat_direction_ was not cleared on mode change. If THERMO_COOL was
  // active from a prior COOL cycle and the user switched to HEAT mode while the
  // room was warm (no heat demand), evaluate_thermostat_() never cleared
  // THERMO_COOL, so effective_mode_() returned COOL and the compressor ran in
  // cooling mode even though the user had selected HEAT.
  Harness h;

  // Start cooling (room is warm, compressor runs)
  h.climate.current_temperature = 25.0f;
  h.set_mode(climate::CLIMATE_MODE_COOL);
  h.run_for(2000);
  REQUIRE(h.climate.comp_running_);
  REQUIRE((h.climate.active_cmd_ & BIT_HEAT) == 0);

  // Switch to HEAT while still warm — no heat demand (23.58 > 23.5 - 0.5 = 23.0)
  h.climate.current_temperature = 23.58f;
  h.climate.target_temperature = 23.5f;
  h.frames.clear();  // only check frames from this point on
  h.set_mode(climate::CLIMATE_MODE_HEAT);

  // After the mode switch the thermostat direction must not be COOL
  REQUIRE(h.climate.thermostat_direction_ != THERMO_COOL);

  // Let timers run out — at no point should the compressor run in COOL mode
  h.run_for(COOLDOWN_MS + SETTLE_MS + 5000);

  for (auto &f : h.frames) {
    bool comp = f.comp_running;
    bool heat = (f.cmd & BIT_HEAT) != 0;
    if (comp) {
      INFO("t=" << f.time_ms << "ms: compressor running  cmd=" << cmd_hex(f.cmd));
      CHECK(heat);  // if compressor ever runs, it must be in HEAT (not COOL) mode
    }
  }

  h.check_invariants();
}

TEST_CASE("Regression: setpoint change after mode switch bypasses valve settle",
          "[regression]") {
  // Exact scenario from production logs 2026-04-15:
  //   13:48:57 — mode HEAT→COOL while temp (23.7) was below engage threshold
  //             (target 23.5 + hysteresis 0.5 = 24.0).  Thermostat stayed idle,
  //             Branch A cleared the HEAT bit WITHOUT arming the valve timer.
  //   13:48:58 — setpoint lowered to 23.0, making 23.7 > 23.5 engage threshold.
  //             Compressor started 1.3 s after the valve switched — no settle.
  Harness h;

  // Phase 1: establish HEAT with compressor, then let thermostat go idle
  h.climate.current_temperature = 19.0f;
  h.set_mode(climate::CLIMATE_MODE_HEAT);
  h.run_for(2000);
  REQUIRE(h.climate.comp_running_);

  h.set_temperature(23.7f);
  h.climate.target_temperature = 23.5f;
  h.tick();
  REQUIRE_FALSE(h.climate.comp_running_);

  // Let cooldown + idle period pass, valve stays energised (keep_heat)
  h.run_for(COOLDOWN_MS + 5000);
  REQUIRE((h.climate.active_cmd_ & BIT_HEAT) != 0);

  // Phase 2: switch to COOL — but temp 23.7 < 23.5+0.5 = 24.0, so thermostat
  // does NOT engage.  Branch A runs with keep_heat=false, clearing HEAT.
  h.frames.clear();
  h.set_mode(climate::CLIMATE_MODE_COOL);
  h.tick();
  CHECK((h.climate.active_cmd_ & BIT_HEAT) == 0);   // valve switched
  CHECK_FALSE(h.climate.comp_running_);              // comp still off
  CHECK(h.climate.valve_timer_armed_);               // settle timer MUST be armed

  // Phase 3: ~1 second later, lower the setpoint so thermostat engages
  h.run_for(1000);
  h.climate.target_temperature = 23.0f;
  h.set_mode(climate::CLIMATE_MODE_COOL);  // re-send mode to trigger evaluate
  h.tick();

  // Compressor must NOT have started yet — valve hasn't settled
  CHECK_FALSE(h.climate.comp_running_);

  // After settle elapses, compressor starts
  h.run_for(SETTLE_MS + 2000);
  REQUIRE(h.climate.comp_running_);
  REQUIRE((h.climate.active_cmd_ & BIT_HEAT) == 0);

  h.check_invariants();
}

TEST_CASE("HEAT->OFF->COOL quick succession: valve settle carries through",
          "[protection]") {
  // Valve settle is armed in Branch A when going HEAT→OFF (keep_heat=false).
  // If the user then quickly switches OFF→COOL, the settle timer from the
  // HEAT→OFF event must still block the compressor.
  Harness h;

  // Heat until thermostat is satisfied, then idle with valve held
  h.climate.current_temperature = 19.0f;
  h.set_mode(climate::CLIMATE_MODE_HEAT);
  h.run_for(2000);
  REQUIRE(h.climate.comp_running_);
  h.set_temperature(23.0f);
  h.tick();
  h.run_for(COOLDOWN_MS + 5000);
  REQUIRE((h.climate.active_cmd_ & BIT_HEAT) != 0);

  // Switch to OFF — Branch A clears HEAT, arms valve timer
  h.frames.clear();
  h.set_mode(climate::CLIMATE_MODE_OFF);
  h.tick();
  CHECK((h.climate.active_cmd_ & BIT_HEAT) == 0);
  CHECK(h.climate.valve_timer_armed_);

  // Immediately switch to COOL — valve timer must still be armed
  h.climate.current_temperature = 25.0f;
  h.set_mode(climate::CLIMATE_MODE_COOL);
  h.tick();
  CHECK_FALSE(h.climate.comp_running_);
  CHECK(h.climate.valve_timer_armed_);

  // Comp must stay off during settle
  h.run_for(SETTLE_MS - 2000);
  CHECK_FALSE(h.climate.comp_running_);

  // After settle, comp starts
  h.run_for(5000);
  REQUIRE(h.climate.comp_running_);

  h.check_invariants();
}

TEST_CASE("HEAT->COOL during cooldown then setpoint change: both timers enforced",
          "[protection]") {
  // When switching HEAT→COOL while the compressor is still in cooldown, the
  // HEAT bit is held during cooldown (correctly), then cleared after cooldown
  // with the valve timer armed (the fix).  A setpoint change should then wait
  // for the valve settle on top of the already-elapsed cooldown.
  Harness h;

  h.climate.current_temperature = 19.0f;
  h.set_mode(climate::CLIMATE_MODE_HEAT);
  h.run_for(2000);
  REQUIRE(h.climate.comp_running_);

  // Switch to COOL while comp just stopped (during cooldown).
  // Temp 23.7 < target 23.5 + hysteresis 0.5 = 24.0, so thermostat stays idle.
  h.climate.current_temperature = 23.7f;
  h.climate.target_temperature = 23.5f;
  h.set_mode(climate::CLIMATE_MODE_COOL);
  h.tick();
  REQUIRE_FALSE(h.climate.comp_running_);

  // During cooldown the HEAT bit should stay held
  h.run_for(COOLDOWN_MS - 2000);
  CHECK((h.climate.active_cmd_ & BIT_HEAT) != 0);

  // Just past cooldown: HEAT clears and valve timer arms
  h.run_for(5000);
  CHECK((h.climate.active_cmd_ & BIT_HEAT) == 0);
  CHECK(h.climate.valve_timer_armed_);

  // Immediately lower setpoint so thermostat engages — comp must wait for settle.
  // A few seconds have elapsed since the valve switched (between the cooldown
  // elapsing and this point), so check at a time clearly within the 30 s window.
  h.frames.clear();
  h.climate.target_temperature = 23.0f;
  h.set_mode(climate::CLIMATE_MODE_COOL);
  h.tick();
  CHECK_FALSE(h.climate.comp_running_);

  h.run_for(SETTLE_MS / 2);
  CHECK_FALSE(h.climate.comp_running_);

  h.run_for(SETTLE_MS);
  REQUIRE(h.climate.comp_running_);

  h.check_invariants();
}

TEST_CASE("HEAT->COOL->HEAT during cooldown: valve stays, no settle needed",
          "[protection]") {
  // If the user switches HEAT→COOL→HEAT while still in cooldown, the valve
  // never actually switches (HEAT held during cooldown, then keep_heat=true
  // when back in HEAT mode).  No valve settle should be needed.
  Harness h;

  h.climate.current_temperature = 19.0f;
  h.set_mode(climate::CLIMATE_MODE_HEAT);
  h.run_for(2000);
  REQUIRE(h.climate.comp_running_);

  // Stop comp, go idle
  h.set_temperature(23.0f);
  h.tick();
  REQUIRE_FALSE(h.climate.comp_running_);

  // Switch to COOL during cooldown — HEAT held
  h.set_mode(climate::CLIMATE_MODE_COOL);
  h.run_for(1000);
  CHECK((h.climate.active_cmd_ & BIT_HEAT) != 0);

  // Switch back to HEAT — valve should never have moved
  h.climate.current_temperature = 19.0f;
  h.set_mode(climate::CLIMATE_MODE_HEAT);

  // After cooldown elapses, comp should restart in HEAT with NO valve settle
  // (valve was held the entire time — keep_heat=true for HEAT mode)
  h.run_for(COOLDOWN_MS + 5000);
  REQUIRE(h.climate.comp_running_);
  REQUIRE((h.climate.active_cmd_ & BIT_HEAT) != 0);
  // Valve timer should never have been armed — valve never switched
  CHECK_FALSE(h.climate.valve_timer_armed_);

  h.check_invariants();
}

// ---------------------------------------------------------------------------
// Cycle statistics — counts compressor starts, valve switches, and measures
// minimum intervals across a frame history.
// ---------------------------------------------------------------------------
struct CycleStats {
  int comp_starts = 0;
  int valve_switches = 0;
  uint32_t min_comp_run_ms = UINT32_MAX;
  uint32_t min_comp_off_ms = UINT32_MAX;
  uint32_t min_valve_interval_ms = UINT32_MAX;
};

static CycleStats compute_cycle_stats(const std::vector<Frame> &frames) {
  CycleStats s;
  // Initial state before any frame: comp off, no HEAT — matches hardware
  // power-on defaults, so the first real transition is correctly counted.
  bool prev_comp = false;
  bool prev_heat = false;
  uint32_t comp_on_time = 0;
  uint32_t comp_off_time = 0;
  bool had_comp_stop = false;
  uint32_t last_valve_time = 0;
  bool had_valve_switch = false;

  for (auto &f : frames) {
    bool comp = f.comp_running;
    bool heat = (f.cmd & BIT_HEAT) != 0;

    if (!prev_comp && comp) {
      s.comp_starts++;
      comp_on_time = f.time_ms;
      if (had_comp_stop) {
        uint32_t gap = f.time_ms - comp_off_time;
        if (gap < s.min_comp_off_ms)
          s.min_comp_off_ms = gap;
      }
    }
    if (prev_comp && !comp) {
      comp_off_time = f.time_ms;
      had_comp_stop = true;
      uint32_t run = f.time_ms - comp_on_time;
      if (run < s.min_comp_run_ms)
        s.min_comp_run_ms = run;
    }
    if (heat != prev_heat) {
      s.valve_switches++;
      if (had_valve_switch) {
        uint32_t gap = f.time_ms - last_valve_time;
        if (gap < s.min_valve_interval_ms)
          s.min_valve_interval_ms = gap;
      }
      last_valve_time = f.time_ms;
      had_valve_switch = true;
    }

    prev_comp = comp;
    prev_heat = heat;
  }
  return s;
}

// ===========================================================================
// Rapid-cycling tests
// ===========================================================================

TEST_CASE("Sawtooth temperature: comp cycles bounded by cooldown", "[cycling]") {
  Harness h;

  h.climate.current_temperature = 25.0f;
  h.set_mode(climate::CLIMATE_MODE_COOL);
  h.run_for(2000);
  REQUIRE(h.climate.comp_running_);

  // Swing temp wildly every 10 s — crosses both engage (22.5) and
  // disengage (22.0) thresholds.  Each swing wants to start/stop the
  // compressor, but cooldown must limit the actual cycle rate.
  for (int i = 0; i < 30; i++) {
    h.set_temperature(25.0f);
    h.run_for(10'000);
    h.set_temperature(20.0f);
    h.run_for(10'000);
  }
  // 30 swings × 20 s = 600 s total.
  // Cooldown is 180 s, so max restarts ≈ 600/180 + 1 ≈ 4–5.
  auto stats = compute_cycle_stats(h.frames);
  INFO("comp_starts=" << stats.comp_starts
        << "  min_comp_off_ms=" << stats.min_comp_off_ms);
  CHECK(stats.comp_starts <= 5);
  CHECK(stats.min_comp_off_ms >= COOLDOWN_MS);
  CHECK(stats.valve_switches == 0);  // COOL mode — valve never touched

  h.check_invariants();
}

TEST_CASE("Noisy sensor near threshold: thermostat stays stable", "[cycling]") {
  Harness h;

  h.climate.current_temperature = 22.4f;
  h.set_mode(climate::CLIMATE_MODE_COOL);

  // Oscillate rapidly across the engage threshold (22.5).
  // Once cooling engages, it should stick until temp drops below target (22.0),
  // which these small swings never do.
  for (int i = 0; i < 200; i++) {
    h.set_temperature(22.4f + (i % 2) * 0.2f);  // alternates 22.4 / 22.6
    h.tick();
  }

  auto stats = compute_cycle_stats(h.frames);
  CHECK(stats.comp_starts <= 1);  // at most the initial engage

  // Now oscillate across the disengage threshold (22.0) while cooling.
  // Once cooling disengages, it should NOT re-engage until temp > 22.5,
  // which these swings never reach.
  h.set_temperature(21.9f);  // disengage
  h.tick();
  int direction_changes = 0;
  ThermostatDirection prev = h.climate.thermostat_direction_;

  for (int i = 0; i < 200; i++) {
    h.set_temperature(21.9f + (i % 2) * 0.2f);  // alternates 21.9 / 22.1
    h.tick();
    if (h.climate.thermostat_direction_ != prev) {
      direction_changes++;
      prev = h.climate.thermostat_direction_;
    }
  }
  // At most 1 direction change (the initial disengage). No re-engagement.
  CHECK(direction_changes <= 1);

  h.check_invariants();
}

TEST_CASE("HEAT_COOL: deadband prevents valve ping-pong from overshoot", "[cycling]") {
  Harness h;
  h.climate.set_auto_deadband(1.0f);

  // Start warm — cooling should engage (23 > 22 + 0.5)
  h.climate.current_temperature = 23.0f;
  h.set_mode(climate::CLIMATE_MODE_HEAT_COOL);
  h.run_for(2000);
  REQUIRE(h.climate.comp_running_);
  REQUIRE((h.climate.active_cmd_ & BIT_HEAT) == 0);  // cooling

  // Cooling brings temp slightly below target (overshoot) — should go idle,
  // NOT flip to heating.  Heat threshold with deadband = 22 - 1.0 = 21.0.
  h.set_temperature(21.8f);
  h.run_for(2000);
  CHECK(h.climate.thermostat_direction_ == THERMO_OFF);

  // Even after all timers expire, heating must not engage at 21.8
  h.run_for(COOLDOWN_MS + SETTLE_MS + 5000);
  CHECK_FALSE(h.climate.comp_running_);

  // But if temp truly drops past the deadband, heating engages
  h.set_temperature(20.5f);
  h.run_for(COOLDOWN_MS + SETTLE_MS + 5000);
  REQUIRE(h.climate.comp_running_);
  REQUIRE((h.climate.active_cmd_ & BIT_HEAT) != 0);

  // And after heating, the reverse deadband protects against cooling re-engage.
  // Cool threshold with deadband = 22 + 1.0 = 23.0
  h.set_temperature(22.3f);  // above target but below 23.0 — should idle, not cool
  h.run_for(COOLDOWN_MS + SETTLE_MS + 5000);
  CHECK(h.climate.thermostat_direction_ == THERMO_OFF);

  auto stats = compute_cycle_stats(h.frames);
  INFO("comp_starts=" << stats.comp_starts << "  valve_switches=" << stats.valve_switches);
  CHECK(stats.comp_starts == 2);      // exactly: one cool, one heat
  CHECK(stats.valve_switches == 1);   // one transition: cool→heat

  h.check_invariants();
}

TEST_CASE("HEAT_COOL: 30 min session with realistic temp swings", "[cycling]") {
  Harness h;
  h.climate.set_auto_deadband(1.0f);

  h.climate.current_temperature = 23.5f;
  h.set_mode(climate::CLIMATE_MODE_HEAT_COOL);

  // Simulate 30 minutes of temperature drifting in a sinusoidal-ish pattern
  // around the setpoint (22°C) with ±2°C amplitude.  Temperature updates
  // arrive every ~10 s (realistic for a BLE sensor).
  constexpr uint32_t SIM_DURATION = 30 * 60 * 1000;  // 30 min
  constexpr uint32_t SENSOR_INTERVAL = 10'000;        // 10 s
  constexpr int STEPS = SIM_DURATION / SENSOR_INTERVAL;

  for (int i = 0; i < STEPS; i++) {
    // Crude sine: amplitude 2°C, period ~8 min
    float phase = static_cast<float>(i) / 48.0f * 6.2832f;
    float temp = 22.0f + 2.0f * sinf(phase);
    h.set_temperature(temp);
    h.run_for(SENSOR_INTERVAL);
  }

  auto stats = compute_cycle_stats(h.frames);
  INFO("comp_starts=" << stats.comp_starts
        << "  valve_switches=" << stats.valve_switches
        << "  min_comp_off_ms=" << stats.min_comp_off_ms
        << "  min_valve_interval_ms=" << stats.min_valve_interval_ms);

  // With 3 min cooldown + 30 s settle, a full heat↔cool cycle takes at least
  // ~7 min.  In 30 min that's at most ~4 full reversals.
  CHECK(stats.valve_switches <= 8);
  if (stats.min_comp_off_ms != UINT32_MAX)
    CHECK(stats.min_comp_off_ms >= COOLDOWN_MS);
  if (stats.min_valve_interval_ms != UINT32_MAX)
    CHECK(stats.min_valve_interval_ms >= COOLDOWN_MS);

  h.check_invariants();
}

TEST_CASE("User spamming HEAT/COOL 20 times", "[cycling]") {
  Harness h;

  // Start with compressor running in COOL
  h.climate.current_temperature = 25.0f;
  h.set_mode(climate::CLIMATE_MODE_COOL);
  h.run_for(2000);
  REQUIRE(h.climate.comp_running_);

  // Spam 20 mode toggles, 1 second apart — a truly chaotic user
  for (int i = 0; i < 20; i++) {
    if (i % 2 == 0) {
      h.climate.current_temperature = 19.0f;
      h.set_mode(climate::CLIMATE_MODE_HEAT);
    } else {
      h.climate.current_temperature = 25.0f;
      h.set_mode(climate::CLIMATE_MODE_COOL);
    }
    h.run_for(1000);
  }

  // Let the system settle
  h.run_for(COOLDOWN_MS + SETTLE_MS + 10'000);

  auto stats = compute_cycle_stats(h.frames);
  INFO("comp_starts=" << stats.comp_starts
        << "  valve_switches=" << stats.valve_switches
        << "  min_comp_off_ms=" << stats.min_comp_off_ms);

  // 20 mode changes in 20 s, but cooldown is 180 s.
  // The compressor can only start once initially plus at most once more
  // after the spam settles and cooldown elapses.
  CHECK(stats.comp_starts <= 3);
  if (stats.min_comp_off_ms != UINT32_MAX)
    CHECK(stats.min_comp_off_ms >= COOLDOWN_MS);

  h.check_invariants();
}

TEST_CASE("Long idle in HEAT then switch to COOL: valve settle respected", "[protection]") {
  Harness h;

  // Phase 1: heat until thermostat is satisfied
  h.climate.current_temperature = 19.0f;
  h.set_mode(climate::CLIMATE_MODE_HEAT);
  h.run_for(2000);
  REQUIRE(h.climate.comp_running_);
  REQUIRE((h.climate.active_cmd_ & BIT_HEAT) != 0);

  // Thermostat goes idle (temp reached target)
  h.set_temperature(23.0f);
  h.tick();
  REQUIRE_FALSE(h.climate.comp_running_);

  // Let cooldown elapse — valve should stay energised (keep_heat)
  h.run_for(COOLDOWN_MS + 5000);
  REQUIRE((h.climate.active_cmd_ & BIT_HEAT) != 0);

  // Idle for ~1 hour (advance time without generating thousands of frames)
  s_fake_millis += 60 * 60 * 1000;
  h.tick();
  REQUIRE((h.climate.active_cmd_ & BIT_HEAT) != 0);
  REQUIRE_FALSE(h.climate.comp_running_);

  // Phase 2: switch to COOL
  h.climate.current_temperature = 25.0f;
  h.frames.clear();
  h.set_mode(climate::CLIMATE_MODE_COOL);

  // First tick: Step 3 should clear HEAT and arm valve timer
  h.tick();
  CHECK((h.climate.active_cmd_ & BIT_HEAT) == 0);  // valve switched
  CHECK_FALSE(h.climate.comp_running_);             // comp must NOT start yet
  CHECK(h.climate.valve_timer_armed_);              // settle timer armed

  // During settle period the compressor must stay off
  h.run_for(SETTLE_MS - 2000);
  CHECK_FALSE(h.climate.comp_running_);

  // After settle elapses, compressor starts
  h.run_for(5000);
  REQUIRE(h.climate.comp_running_);
  REQUIRE((h.climate.active_cmd_ & BIT_HEAT) == 0);

  // Verify timing from frame history
  uint32_t heat_off_time = 0;
  uint32_t comp_start_time = 0;
  bool prev_heat = true;
  bool prev_comp = false;
  for (auto &f : h.frames) {
    bool heat = (f.cmd & BIT_HEAT) != 0;
    if (prev_heat && !heat)
      heat_off_time = f.time_ms;
    if (!prev_comp && f.comp_running)
      comp_start_time = f.time_ms;
    prev_heat = heat;
    prev_comp = f.comp_running;
  }
  REQUIRE(heat_off_time > 0);
  REQUIRE(comp_start_time > 0);
  uint32_t gap = comp_start_time - heat_off_time;
  INFO("valve off at " << heat_off_time << "ms, comp on at "
        << comp_start_time << "ms, gap=" << gap << "ms");
  CHECK(gap >= SETTLE_MS);

  h.check_invariants();
}

TEST_CASE("Valve never switches within cooldown of comp stop", "[protection]") {
  // Stress test: the comp stops and the mode changes in quick succession.
  // In each scenario the HEAT bit must not change until cooldown_ms after
  // the compressor stopped.  This tests the Branch A cooldown hold (line 123)
  // and the Step 1→2→3 sequence.
  Harness h;

  SECTION("HEAT mode comp stops, immediate switch to OFF") {
    h.climate.current_temperature = 19.0f;
    h.set_mode(climate::CLIMATE_MODE_HEAT);
    h.run_for(2000);
    REQUIRE(h.climate.comp_running_);
    REQUIRE((h.climate.active_cmd_ & BIT_HEAT) != 0);

    // Switch OFF on the very next tick after comp is running
    h.set_mode(climate::CLIMATE_MODE_OFF);
    h.tick();
    // Comp stopped THIS tick — HEAT must still be set
    CHECK_FALSE(h.climate.comp_running_);
    CHECK((h.climate.active_cmd_ & BIT_HEAT) != 0);

    // HEAT must stay set for the entire cooldown
    for (uint32_t elapsed = 0; elapsed + 5000 < COOLDOWN_MS; elapsed += 5000) {
      h.run_for(5000);
      INFO("elapsed=" << elapsed + 5000 << "ms after comp stop");
      CHECK((h.climate.active_cmd_ & BIT_HEAT) != 0);
    }

    // After cooldown, HEAT clears
    h.run_for(10000);
    CHECK((h.climate.active_cmd_ & BIT_HEAT) == 0);

    h.check_invariants();
  }

  SECTION("HEAT mode comp stops, immediate switch to COOL") {
    h.climate.current_temperature = 19.0f;
    h.set_mode(climate::CLIMATE_MODE_HEAT);
    h.run_for(2000);
    REQUIRE(h.climate.comp_running_);

    h.climate.current_temperature = 25.0f;
    h.set_mode(climate::CLIMATE_MODE_COOL);
    h.tick();
    CHECK_FALSE(h.climate.comp_running_);
    CHECK((h.climate.active_cmd_ & BIT_HEAT) != 0);

    h.run_for(COOLDOWN_MS - 2000);
    CHECK((h.climate.active_cmd_ & BIT_HEAT) != 0);

    h.run_for(5000);
    CHECK((h.climate.active_cmd_ & BIT_HEAT) == 0);

    h.check_invariants();
  }

  SECTION("HEAT mode comp stops, immediate switch to FAN_ONLY") {
    h.climate.current_temperature = 19.0f;
    h.set_mode(climate::CLIMATE_MODE_HEAT);
    h.run_for(2000);
    REQUIRE(h.climate.comp_running_);

    h.set_mode(climate::CLIMATE_MODE_FAN_ONLY);
    h.tick();
    CHECK_FALSE(h.climate.comp_running_);
    CHECK((h.climate.active_cmd_ & BIT_HEAT) != 0);

    h.run_for(COOLDOWN_MS - 2000);
    CHECK((h.climate.active_cmd_ & BIT_HEAT) != 0);

    h.run_for(5000);
    CHECK((h.climate.active_cmd_ & BIT_HEAT) == 0);

    h.check_invariants();
  }

  SECTION("HEAT comp running, rapid OFF->COOL->OFF->COOL during cooldown") {
    h.climate.current_temperature = 19.0f;
    h.set_mode(climate::CLIMATE_MODE_HEAT);
    h.run_for(2000);
    REQUIRE(h.climate.comp_running_);

    // Rapid mode changes during cooldown — HEAT must be held through all of them
    h.set_mode(climate::CLIMATE_MODE_OFF);
    h.run_for(1000);
    CHECK((h.climate.active_cmd_ & BIT_HEAT) != 0);

    h.climate.current_temperature = 25.0f;
    h.set_mode(climate::CLIMATE_MODE_COOL);
    h.run_for(1000);
    CHECK((h.climate.active_cmd_ & BIT_HEAT) != 0);

    h.set_mode(climate::CLIMATE_MODE_OFF);
    h.run_for(1000);
    CHECK((h.climate.active_cmd_ & BIT_HEAT) != 0);

    h.climate.current_temperature = 25.0f;
    h.set_mode(climate::CLIMATE_MODE_COOL);
    h.run_for(1000);
    CHECK((h.climate.active_cmd_ & BIT_HEAT) != 0);

    // Still in cooldown — HEAT must be held
    h.run_for(COOLDOWN_MS - 10000);
    CHECK((h.climate.active_cmd_ & BIT_HEAT) != 0);

    // After cooldown, HEAT clears
    h.run_for(10000);
    CHECK((h.climate.active_cmd_ & BIT_HEAT) == 0);

    h.check_invariants();
  }
}

TEST_CASE("Rapid mode cycling respects all protections", "[protection]") {
  Harness h;

  // Start cooling
  h.climate.current_temperature = 25.0f;
  h.set_mode(climate::CLIMATE_MODE_COOL);
  h.run_for(3000);
  REQUIRE(h.climate.comp_running_);

  // Barrage of rapid changes — none of these should violate protections
  h.climate.current_temperature = 19.0f;
  h.set_mode(climate::CLIMATE_MODE_HEAT);
  h.run_for(1000);

  h.climate.current_temperature = 25.0f;
  h.set_mode(climate::CLIMATE_MODE_COOL);
  h.run_for(500);

  h.set_mode(climate::CLIMATE_MODE_OFF);
  h.run_for(200);

  h.climate.current_temperature = 19.0f;
  h.set_mode(climate::CLIMATE_MODE_HEAT);
  h.run_for(500);

  h.set_mode(climate::CLIMATE_MODE_FAN_ONLY);
  h.run_for(300);

  h.climate.current_temperature = 25.0f;
  h.set_mode(climate::CLIMATE_MODE_COOL);

  // Let everything settle
  h.run_for(COOLDOWN_MS + SETTLE_MS + 10'000);
  REQUIRE(h.climate.comp_running_);

  h.check_invariants();
}
