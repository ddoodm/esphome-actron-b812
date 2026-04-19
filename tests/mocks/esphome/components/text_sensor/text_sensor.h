#pragma once
#include <string>

namespace esphome {
namespace text_sensor {

class TextSensor {
 public:
  void publish_state(const std::string &) {}
};

}  // namespace text_sensor
}  // namespace esphome
