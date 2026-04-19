#pragma once
#include <cstdint>
#include <ctime>

namespace esphome {
namespace time {

struct ESPTime {
  time_t timestamp{0};
  bool is_valid() const { return timestamp != 0; }
};

class RealTimeClock {
 public:
  ESPTime now() { return now_; }
  void set_now(time_t t) { now_.timestamp = t; }

 private:
  ESPTime now_{};
};

}  // namespace time
}  // namespace esphome
