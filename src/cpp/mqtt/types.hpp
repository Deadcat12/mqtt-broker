#pragma once

#include <cstdint>

namespace sol::mqtt {

enum class PacketType : std::uint8_t {
    Connect     = 1,
    ConnAck     = 2,
    Publish     = 3,
    PubAck      = 4,
    PubRec      = 5,
    PubRel      = 6,
    PubComp     = 7,
    Subscribe   = 8,
    SubAck      = 9,
    Unsubscribe = 10,
    UnsubAck    = 11,
    PingReq     = 12,
    PingResp    = 13,
    Disconnect  = 14
};

enum class QoS : std::uint8_t {
    AtMostOnce  = 0,
    AtLeastOnce = 1,
    ExactlyOnce = 2
};

} // namespace sol::mqtt