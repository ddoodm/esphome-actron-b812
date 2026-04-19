#pragma once
#include <cstdint>

namespace esphome {
namespace remote_transmitter {

struct RemoteTransmitData {
  void set_carrier_frequency(uint32_t) {}
  void mark(uint32_t) {}
  void space(uint32_t) {}
};

struct TransmitCall {
  RemoteTransmitData data;
  RemoteTransmitData *get_data() { return &data; }
  void perform() {}
};

class RemoteTransmitterComponent {
 public:
  TransmitCall transmit() { return {}; }
};

}  // namespace remote_transmitter
}  // namespace esphome
