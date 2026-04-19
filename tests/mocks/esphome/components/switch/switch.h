#pragma once

namespace esphome {
namespace switch_ {

class Switch {
 public:
  virtual ~Switch() = default;
  void publish_state(bool) {}
  void set_restore_mode(int) {}
 protected:
  virtual void write_state(bool) {}
};

}  // namespace switch_
}  // namespace esphome
