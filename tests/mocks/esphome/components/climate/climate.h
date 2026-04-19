#pragma once
#include <cmath>
#include <vector>

namespace esphome {
namespace climate {

enum ClimateMode {
  CLIMATE_MODE_OFF,
  CLIMATE_MODE_COOL,
  CLIMATE_MODE_HEAT,
  CLIMATE_MODE_HEAT_COOL,
  CLIMATE_MODE_FAN_ONLY,
};

enum ClimateFanMode {
  CLIMATE_FAN_AUTO,
  CLIMATE_FAN_LOW,
  CLIMATE_FAN_MEDIUM,
  CLIMATE_FAN_HIGH,
};

enum ClimateAction {
  CLIMATE_ACTION_OFF,
  CLIMATE_ACTION_COOLING,
  CLIMATE_ACTION_HEATING,
  CLIMATE_ACTION_FAN,
  CLIMATE_ACTION_IDLE,
};

static const uint32_t CLIMATE_SUPPORTS_CURRENT_TEMPERATURE = 1;
static const uint32_t CLIMATE_SUPPORTS_ACTION = 2;

template <typename T>
class optional {
  bool has_ = false;
  T val_{};

 public:
  optional() = default;
  optional(T v) : has_(true), val_(v) {}
  bool has_value() const { return has_; }
  const T &operator*() const { return val_; }
};

class ClimateTraits {
 public:
  void add_feature_flags(uint32_t) {}
  void set_visual_min_temperature(float) {}
  void set_visual_max_temperature(float) {}
  void set_visual_target_temperature_step(float) {}
  void set_visual_current_temperature_step(float) {}
  void set_supported_modes(std::vector<ClimateMode>) {}
  void set_supported_fan_modes(std::vector<ClimateFanMode>) {}
};

class ClimateCall {
  optional<ClimateMode> mode_;
  optional<ClimateFanMode> fan_;
  optional<float> temp_;

 public:
  void set_mode(ClimateMode m) { mode_ = m; }
  void set_fan_mode(ClimateFanMode f) { fan_ = f; }
  void set_target_temperature(float t) { temp_ = t; }
  optional<ClimateMode> get_mode() const { return mode_; }
  optional<ClimateFanMode> get_fan_mode() const { return fan_; }
  optional<float> get_target_temperature() const { return temp_; }
};

class Climate {
 public:
  virtual ~Climate() = default;

  float target_temperature{NAN};
  float current_temperature{NAN};
  ClimateMode mode{CLIMATE_MODE_OFF};
  ClimateFanMode fan_mode{CLIMATE_FAN_LOW};
  ClimateAction action{CLIMATE_ACTION_OFF};

  virtual ClimateTraits traits() = 0;
  virtual void control(const ClimateCall &call) = 0;
  void publish_state() {}
};

}  // namespace climate
}  // namespace esphome
