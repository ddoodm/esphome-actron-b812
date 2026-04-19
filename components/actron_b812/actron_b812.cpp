#include "actron_b812.hpp"
#include "esphome/core/log.h"

namespace esphome {
namespace actron_b812 {

static const char *TAG = "actron_b812";

climate::ClimateTraits ActronB812Climate::traits() {
  auto traits = climate::ClimateTraits();
  uint32_t flags = climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE |
                   climate::CLIMATE_SUPPORTS_ACTION;
  traits.add_feature_flags(flags);
  traits.set_visual_min_temperature(19);
  traits.set_visual_max_temperature(29);
  traits.set_visual_target_temperature_step(0.5);
  traits.set_visual_current_temperature_step(0.01);
  traits.set_supported_modes({
    climate::CLIMATE_MODE_OFF,
    climate::CLIMATE_MODE_COOL,
    climate::CLIMATE_MODE_HEAT,
    climate::CLIMATE_MODE_HEAT_COOL,
    climate::CLIMATE_MODE_FAN_ONLY,
  });
  traits.set_supported_fan_modes({
    climate::CLIMATE_FAN_AUTO,
    climate::CLIMATE_FAN_LOW,
    climate::CLIMATE_FAN_MEDIUM,
    climate::CLIMATE_FAN_HIGH,
  });
  return traits;
}

void ActronB812Climate::setup() {
  active_cmd_ = apply_zone_bits_(0);

  // Seed sensible defaults so HA shows the dial immediately on first boot.
  // ESPHome's base Climate class restores these from flash on subsequent boots.
  if (std::isnan(this->target_temperature))
    this->target_temperature = 22.0f;

  if (temperature_sensor_) {
    temperature_sensor_->add_on_state_callback([this](float v) {
      this->current_temperature = v;
      evaluate_thermostat_();
      update_action_();
      this->publish_state();
    });
  }
  publish_sensors_();
}

void ActronB812Climate::control(const climate::ClimateCall &call) {
  if (call.get_mode().has_value()) {
    if (*call.get_mode() != pending_mode_) {
      auto_deadband_direction_ = THERMO_OFF;  // fresh engage — don't apply cross-mode deadband yet
      // Clear stale thermostat direction from the old mode so evaluate_thermostat_()
      // doesn't carry over e.g. THERMO_COOL into a HEAT mode evaluation, which would
      // cause effective_mode_() to return COOL even though the user selected HEAT.
      thermostat_direction_ = THERMO_OFF;
    }
    pending_mode_ = *call.get_mode();
  }
  if (call.get_fan_mode().has_value()) {
    pending_fan_ = *call.get_fan_mode();
    if (pending_fan_ != climate::CLIMATE_FAN_AUTO)
      pending_auto_fan_speed_ = pending_fan_;
  }
  if (call.get_target_temperature().has_value()) {
    if (*call.get_target_temperature() != this->target_temperature)
      auto_deadband_direction_ = THERMO_OFF;
    this->target_temperature = *call.get_target_temperature();
  }

  evaluate_thermostat_();

  // Publish desired state immediately so the HA UI reflects the change
  // even while we may be waiting for the compressor protection timer.
  this->mode = pending_mode_;
  this->fan_mode = pending_fan_;
  this->publish_state();

  update_action_();

  pending_change_ = true;
}

void ActronB812Climate::update() {
  // Fan speed is always applied immediately, even during cooldown waits.
  // Skip when OFF — CMD_OFF zeroes all bits including fan.
  // In AUTO mode the fan only runs while the compressor is physically on.
  if (pending_mode_ != climate::CLIMATE_MODE_OFF) {
    active_cmd_ &= ~(BIT_FS1 | BIT_FS2 | BIT_FS3);
    bool apply_fan = (pending_fan_ != climate::CLIMATE_FAN_AUTO) || comp_running_;
    if (apply_fan) {
      climate::ClimateFanMode speed =
          (pending_fan_ == climate::CLIMATE_FAN_AUTO) ? pending_auto_fan_speed_ : pending_fan_;
      switch (speed) {
        case climate::CLIMATE_FAN_LOW:    active_cmd_ |= BIT_FS1; break;
        case climate::CLIMATE_FAN_MEDIUM: active_cmd_ |= BIT_FS2; break;
        case climate::CLIMATE_FAN_HIGH:   active_cmd_ |= BIT_FS3; break;
        default: break;
      }
    }
  }

  if (pending_change_) {
    uint8_t desired = build_cmd_(effective_mode_(), pending_fan_);
    bool comp_desired = (desired & BIT_COMP) != 0;
    bool heat_direction_change = comp_desired && ((desired & BIT_HEAT) != (active_cmd_ & BIT_HEAT));

    // Branch A: compressor not needed (OFF or FAN_ONLY).
    if (!comp_desired) {
      if (comp_running_) {
        comp_off_time_ = millis();
        comp_off_epoch_ = epoch_now_();
        comp_timer_armed_ = true;
        comp_running_ = false;
        ESP_LOGD(TAG, "Compressor stopped, cooldown armed");
      }

      // If HEAT is still set, keep it until cooldown elapses so the reversing
      // valve doesn't switch while there's residual refrigerant pressure.
      if ((active_cmd_ & BIT_HEAT) && comp_timer_armed_ && !comp_cooldown_elapsed_()) {
        active_cmd_ = desired | BIT_HEAT;
        ESP_LOGD(TAG, "Keeping HEAT on until cooldown elapses (%.0fs remaining)",
                 (comp_cooldown_ms_ - (millis() - comp_off_time_)) / 1000.0f);
      } else {
        // After cooldown, keep the valve energised if we're in HEAT or HEAT_COOL
        // mode — there's no reason to de-energise it until we actually need to cool.
        // Switching it unnecessarily wastes a valve actuation and forces a 30s settle
        // wait before the next heating cycle can start.
        bool keep_heat = (active_cmd_ & BIT_HEAT) &&
                         (pending_mode_ == climate::CLIMATE_MODE_HEAT ||
                          pending_mode_ == climate::CLIMATE_MODE_HEAT_COOL);
        // When the HEAT bit is actually being cleared here, the reversing valve is
        // physically switching.  Arm the settle timer so that if a future setpoint
        // or mode change makes the compressor desired, Step 4 will block until the
        // valve has had time to move.
        if ((active_cmd_ & BIT_HEAT) && !keep_heat) {
          valve_switch_time_ = millis();
          valve_switch_epoch_ = epoch_now_();
          valve_timer_armed_ = true;
          ESP_LOGD(TAG, "Reversing valve switched — settle timer armed");
        }
        active_cmd_ = keep_heat ? (desired | BIT_HEAT) : desired;
        pending_change_ = false;
        ESP_LOGD(TAG, "Applying command 0x%02X%s", active_cmd_,
                 keep_heat ? " (valve held in HEAT)" : "");
      }

    // Step 1: If the heat direction is changing and comp is still running, stop it first.
    } else if (comp_running_ && heat_direction_change) {
      active_cmd_ = (active_cmd_ & ~BIT_COMP) & ~BIT_CALL;
      comp_running_ = false;
      comp_off_time_ = millis();
      comp_off_epoch_ = epoch_now_();
      comp_timer_armed_ = true;
      ESP_LOGW(TAG, "Heat direction change — stopping compressor first");

    // Step 2: Wait for compressor cooldown before restarting.
    } else if (comp_timer_armed_ && !comp_cooldown_elapsed_()) {
      ESP_LOGD(TAG, "Waiting for compressor cooldown (%.0fs remaining)",
               (comp_cooldown_ms_ - (millis() - comp_off_time_)) / 1000.0f);

    // Step 3: HEAT going ON→OFF (Heat→Cool direction) — clear bit and arm valve timer.
    } else if ((active_cmd_ & BIT_HEAT) && !(desired & BIT_HEAT) && !valve_timer_armed_) {
      active_cmd_ &= ~BIT_HEAT;
      valve_switch_time_ = millis();
      valve_switch_epoch_ = epoch_now_();
      valve_timer_armed_ = true;
      ESP_LOGD(TAG, "Reversing valve switching to cool — waiting for valve to settle");

    // Step 4: Wait for reversing valve to settle after going ON→OFF.
    } else if (valve_timer_armed_ && !valve_settled_()) {
      ESP_LOGD(TAG, "Waiting for valve settle (%.0fs remaining)",
               (valve_settle_ms_ - (millis() - valve_switch_time_)) / 1000.0f);

    // Step 5: All clear — apply the full desired command.
    } else {
      active_cmd_ = desired;
      comp_running_ = true;
      comp_timer_armed_ = false;
      valve_timer_armed_ = false;
      pending_change_ = false;
      ESP_LOGD(TAG, "Applying command 0x%02X", active_cmd_);
    }
  }

  // Override CALL bit based on thermostat desire, independent of compressor state.
  // This signals to the AC unit that conditioning is wanted even during cooldown waits.
  if (thermostat_direction_ != THERMO_OFF)
    active_cmd_ |= BIT_CALL;
  else
    active_cmd_ &= ~BIT_CALL;

  send_frame_(active_cmd_);
  publish_sensors_();
  update_action_();
}

bool ActronB812Climate::comp_cooldown_elapsed_() {
  return (millis() - comp_off_time_) >= comp_cooldown_ms_;
}

bool ActronB812Climate::valve_settled_() {
  return (millis() - valve_switch_time_) >= valve_settle_ms_;
}

std::string ActronB812Climate::compute_state_() {
  // Timers take priority — describe what we're waiting for.
  if (comp_timer_armed_ && !comp_cooldown_elapsed_()) {
    // If HEAT bit is still held (Branch A waiting to clear valve), label differently.
    return (active_cmd_ & BIT_HEAT) ? "heat_valve_hold" : "comp_cooldown";
  }
  if (valve_timer_armed_ && !valve_settled_())
    return "valve_settling";
  if (active_cmd_ & BIT_COMP)
    return (active_cmd_ & BIT_HEAT) ? "heating" : "cooling";
  if (active_cmd_ & BIT_HEAT)
    return "heat_idle";  // valve energised, compressor off (between heating cycles)
  if (active_cmd_ & (BIT_FS1 | BIT_FS2 | BIT_FS3))
    return "fan_only";
  return "off";
}

static std::string format_iso8601_(int32_t epoch) {
  if (epoch == 0) return "";
  time_t ts = static_cast<time_t>(epoch);
  struct tm tm_info;
  gmtime_r(&ts, &tm_info);
  char buf[26];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S+00:00", &tm_info);
  return buf;
}

int32_t ActronB812Climate::epoch_now_() {
  if (!time_) return 0;
  auto t = time_->now();
  return t.is_valid() ? static_cast<int32_t>(t.timestamp) : 0;
}

int32_t ActronB812Climate::compute_protection_expires_at_() {
  if (comp_timer_armed_ && !comp_cooldown_elapsed_() && comp_off_epoch_ != 0)
    return comp_off_epoch_ + static_cast<int32_t>(comp_cooldown_ms_ / 1000);
  if (valve_timer_armed_ && !valve_settled_() && valve_switch_epoch_ != 0)
    return valve_switch_epoch_ + static_cast<int32_t>(valve_settle_ms_ / 1000);
  return 0;
}

int32_t ActronB812Climate::compute_deadband_expires_at_() {
  if (!deadband_active_() || auto_deadband_timeout_ms_ == 0 || deadband_idle_epoch_ == 0)
    return 0;
  return deadband_idle_epoch_ + static_cast<int32_t>(auto_deadband_timeout_ms_ / 1000);
}

std::string ActronB812Climate::compute_thermostat_direction_str_() {
  switch (thermostat_direction_) {
    case THERMO_HEAT: return "heat";
    case THERMO_COOL: return "cool";
    default:          return "idle";
  }
}

bool ActronB812Climate::deadband_active_() {
  if (auto_deadband_direction_ == THERMO_OFF) return false;
  if (auto_deadband_timeout_ms_ == 0) return true;
  return (millis() - auto_deadband_idle_since_) < auto_deadband_timeout_ms_;
}


void ActronB812Climate::publish_sensors_() {
  if (compressor_running_sensor_) {
    int running = comp_running_ ? 1 : 0;
    if (running != comp_running_last_) {
      comp_running_last_ = running;
      compressor_running_sensor_->publish_state(comp_running_);
    }
  }

  if (state_sensor_) {
    std::string s = compute_state_();
    if (s != state_last_) {
      state_last_ = s;
      state_sensor_->publish_state(s);
    }
  }

  if (protection_expires_at_sensor_) {
    int32_t expires_at = compute_protection_expires_at_();
    if (expires_at != protection_expires_at_last_) {
      protection_expires_at_last_ = expires_at;
      protection_expires_at_sensor_->publish_state(format_iso8601_(expires_at));
    }
  }

  if (thermostat_direction_sensor_) {
    std::string d = compute_thermostat_direction_str_();
    if (d != thermostat_direction_last_) {
      thermostat_direction_last_ = d;
      thermostat_direction_sensor_->publish_state(d);
    }
  }

  if (deadband_active_sensor_) {
    int active = deadband_active_() ? 1 : 0;
    if (active != deadband_active_last_) {
      deadband_active_last_ = active;
      deadband_active_sensor_->publish_state(active);
    }
  }

  if (deadband_expires_at_sensor_) {
    int32_t expires_at = compute_deadband_expires_at_();
    if (expires_at != deadband_expires_at_last_) {
      deadband_expires_at_last_ = expires_at;
      deadband_expires_at_sensor_->publish_state(format_iso8601_(expires_at));
    }
  }

  if (reversing_valve_sensor_) {
    int val = (active_cmd_ & BIT_HEAT) ? 1 : 0;
    if (val != reversing_valve_last_) {
      reversing_valve_last_ = val;
      reversing_valve_sensor_->publish_state(val);
    }
  }

  if (call_active_sensor_) {
    int val = (active_cmd_ & BIT_CALL) ? 1 : 0;
    if (val != call_active_last_) {
      call_active_last_ = val;
      call_active_sensor_->publish_state(val);
    }
  }
}

void ActronB812Climate::evaluate_thermostat_() {
  ThermostatDirection want = thermostat_direction_;
  float t = this->current_temperature;

  if (pending_mode_ == climate::CLIMATE_MODE_COOL) {
    float tgt = this->target_temperature;
    if (std::isnan(t) || std::isnan(tgt)) return;
    // Start cooling only once temp rises hysteresis above target (avoids short-cycling).
    // Stop as soon as temp reaches target — no overshoot on the off side.
    if (thermostat_direction_ != THERMO_COOL && t > tgt + hysteresis_)
      want = THERMO_COOL;
    else if (thermostat_direction_ == THERMO_COOL && t < tgt)
      want = THERMO_OFF;

  } else if (pending_mode_ == climate::CLIMATE_MODE_HEAT) {
    float tgt = this->target_temperature;
    if (std::isnan(t) || std::isnan(tgt)) return;
    // Start heating only once temp drops hysteresis below target (avoids short-cycling).
    // Stop as soon as temp reaches target — no overshoot on the off side.
    if (thermostat_direction_ != THERMO_HEAT && t < tgt - hysteresis_)
      want = THERMO_HEAT;
    else if (thermostat_direction_ == THERMO_HEAT && t > tgt)
      want = THERMO_OFF;

  } else if (pending_mode_ == climate::CLIMATE_MODE_HEAT_COOL) {
    float tgt = this->target_temperature;
    if (std::isnan(t) || std::isnan(tgt)) return;

    if (thermostat_direction_ == THERMO_HEAT) {
      if (t > tgt)
        want = THERMO_OFF;
    } else if (thermostat_direction_ == THERMO_COOL) {
      if (t < tgt)
        want = THERMO_OFF;
    } else {
      // Idle. If we just came from the opposite direction, require the full
      // auto_deadband_ before engaging it — prevents overshoot from triggering
      // a mode flip. Same-direction re-engage and fresh-engage use hysteresis_.
      // After auto_deadband_timeout_ms_ of being idle the protection expires,
      // allowing normal hysteresis to respond to slow drift (e.g. sun).
      bool deadband_expired = (auto_deadband_timeout_ms_ > 0) &&
                              ((millis() - auto_deadband_idle_since_) >= auto_deadband_timeout_ms_);
      ThermostatDirection guard = deadband_expired ? THERMO_OFF : auto_deadband_direction_;
      float cool_thresh = (guard == THERMO_HEAT) ? tgt + auto_deadband_ : tgt + hysteresis_;
      float heat_thresh = (guard == THERMO_COOL) ? tgt - auto_deadband_ : tgt - hysteresis_;
      if (t > cool_thresh)
        want = THERMO_COOL;
      else if (t < heat_thresh)
        want = THERMO_HEAT;
    }

  } else {
    want = THERMO_OFF;
  }

  if (want != thermostat_direction_) {
    if (want != THERMO_OFF)
      auto_deadband_direction_ = want;
    else {
      auto_deadband_idle_since_ = millis();
      deadband_idle_epoch_ = epoch_now_();
    }
    thermostat_direction_ = want;
    pending_change_ = true;
    ESP_LOGD(TAG, "Thermostat -> %s",
             want == THERMO_COOL ? "cool" : want == THERMO_HEAT ? "heat" : "idle");
  }
}

climate::ClimateMode ActronB812Climate::effective_mode_() {
  if (pending_mode_ == climate::CLIMATE_MODE_COOL ||
      pending_mode_ == climate::CLIMATE_MODE_HEAT ||
      pending_mode_ == climate::CLIMATE_MODE_HEAT_COOL) {
    switch (thermostat_direction_) {
      case THERMO_COOL: return climate::CLIMATE_MODE_COOL;
      case THERMO_HEAT: return climate::CLIMATE_MODE_HEAT;
      default:          return climate::CLIMATE_MODE_FAN_ONLY;
    }
  }
  return pending_mode_;
}

void ActronB812Climate::update_action_() {
  climate::ClimateAction a;
  if (pending_mode_ == climate::CLIMATE_MODE_OFF)
    a = climate::CLIMATE_ACTION_OFF;
  else if (pending_mode_ == climate::CLIMATE_MODE_FAN_ONLY)
    a = climate::CLIMATE_ACTION_FAN;
  else if (comp_running_)
    a = (active_cmd_ & BIT_HEAT) ? climate::CLIMATE_ACTION_HEATING
                                  : climate::CLIMATE_ACTION_COOLING;
  else if (active_cmd_ & (BIT_FS1 | BIT_FS2 | BIT_FS3))
    a = climate::CLIMATE_ACTION_FAN;
  else
    a = climate::CLIMATE_ACTION_IDLE;
  if (this->action != a) {
    this->action = a;
    this->publish_state();
  }
}

uint8_t ActronB812Climate::build_cmd_(climate::ClimateMode mode,
                                      climate::ClimateFanMode fan) {
  uint8_t cmd = 0;

  // In AUTO mode the fan only follows the compressor: suppress fan bits when the
  // effective mode is FAN_ONLY (thermostat idle).  Use the last manual speed otherwise.
  bool apply_fan = (fan != climate::CLIMATE_FAN_AUTO) ||
                   (mode == climate::CLIMATE_MODE_COOL || mode == climate::CLIMATE_MODE_HEAT);
  climate::ClimateFanMode speed =
      (fan == climate::CLIMATE_FAN_AUTO) ? pending_auto_fan_speed_ : fan;

  if (apply_fan) {
    switch (speed) {
      case climate::CLIMATE_FAN_LOW:    cmd |= BIT_FS1; break;
      case climate::CLIMATE_FAN_MEDIUM: cmd |= BIT_FS2; break;
      case climate::CLIMATE_FAN_HIGH:   cmd |= BIT_FS3; break;
      default: break;
    }
  }

  // Mode
  switch (mode) {
    case climate::CLIMATE_MODE_COOL:
      cmd |= BIT_CALL | BIT_COMP;
      break;
    case climate::CLIMATE_MODE_HEAT:
      cmd |= BIT_CALL | BIT_COMP | BIT_HEAT;
      break;
    case climate::CLIMATE_MODE_FAN_ONLY:
      // Fan bits already set above, no CALL or COMP
      break;
    case climate::CLIMATE_MODE_OFF:
    default:
      cmd = 0;  // Clear fan bits too — zone bits applied below
      break;
  }

  return apply_zone_bits_(cmd);
}

uint8_t ActronB812Climate::apply_zone_bits_(uint8_t cmd) {
  cmd &= ~(BIT_ZONE1 | BIT_ZONE2);
  if (zone_1_enabled_) cmd |= BIT_ZONE1;
  if (zone_2_enabled_) cmd |= BIT_ZONE2;
  return cmd;
}

void ActronB812ZoneSwitch::write_state(bool state) {
  this->parent_->set_zone_enabled(this->zone_, state);
}

void ActronB812Climate::set_zone_enabled(uint8_t zone, bool enabled) {
  if (zone != 1 && zone != 2) return;
  bool &target = (zone == 1) ? zone_1_enabled_ : zone_2_enabled_;
  bool &other  = (zone == 1) ? zone_2_enabled_ : zone_1_enabled_;
  ActronB812ZoneSwitch *target_sw = (zone == 1) ? zone_1_switch_ : zone_2_switch_;
  ActronB812ZoneSwitch *other_sw  = (zone == 1) ? zone_2_switch_ : zone_1_switch_;

  if (target == enabled) {
    if (target_sw) target_sw->publish_state(target);
    return;
  }
  target = enabled;

  // Invariant: at least one zone must be enabled.  Last-toggle-wins:
  // disabling the only-enabled zone auto-enables the other.
  bool other_forced_on = false;
  if (!target && !other) {
    other = true;
    other_forced_on = true;
  }

  if (target_sw) target_sw->publish_state(target);
  if (other_forced_on && other_sw) other_sw->publish_state(other);

  // Re-emit the active frame with the new zone bits and ensure any pending
  // mode/fan transition is also re-evaluated.
  active_cmd_ = apply_zone_bits_(active_cmd_);
  pending_change_ = true;

  ESP_LOGD(TAG, "Zone %u -> %s%s", zone, enabled ? "on" : "off",
           other_forced_on ? " (other auto-enabled)" : "");
}

void ActronB812Climate::send_frame_(uint8_t data) {
  auto call = transmitter_->transmit();
  auto *d = call.get_data();
  d->set_carrier_frequency(0);

  for (int i = 7; i >= 0; i--) {
    d->mark(150);
    d->space((data >> i) & 1 ? 6000 : 2000);
  }
  d->mark(150);  // trailing pulse

  call.perform();
}

}  // namespace actron_b812
}  // namespace esphome