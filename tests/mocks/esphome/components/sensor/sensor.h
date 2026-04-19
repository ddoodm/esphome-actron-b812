#pragma once
#include <functional>

namespace esphome {
namespace sensor {

class Sensor {
 public:
  void publish_state(float) {}
  void add_on_state_callback(std::function<void(float)>) {}
};

}  // namespace sensor
}  // namespace esphome
