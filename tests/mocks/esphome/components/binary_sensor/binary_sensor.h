#pragma once

namespace esphome {
namespace binary_sensor {

class BinarySensor {
 public:
  void publish_state(bool) {}
};

}  // namespace binary_sensor
}  // namespace esphome
