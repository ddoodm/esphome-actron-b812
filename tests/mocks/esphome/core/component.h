#pragma once
#include <cstdint>

namespace esphome {

uint32_t millis();

class Component {
 public:
  virtual ~Component() = default;
  virtual void setup() {}
  virtual float get_setup_priority() const { return 0; }
};

class PollingComponent : public Component {
 public:
  explicit PollingComponent(uint32_t /*update_interval*/) {}
  virtual void update() {}
};

}  // namespace esphome
