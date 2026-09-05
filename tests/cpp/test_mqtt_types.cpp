#include "mqtt/types.hpp"

#include <cstdint>
#include <iostream>

namespace {

int failures = 0;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

} // namespace

int main()
{
    using sol::mqtt::PacketType;
    using sol::mqtt::QoS;

    check(
        static_cast<std::uint8_t>(PacketType::Connect) == 1U,
        "CONNECT packet type must be 1"
    );

    check(
        static_cast<std::uint8_t>(PacketType::Publish) == 3U,
        "PUBLISH packet type must be 3"
    );

    check(
        static_cast<std::uint8_t>(PacketType::PubRel) == 6U,
        "PUBREL packet type must be 6"
    );

    check(
        static_cast<std::uint8_t>(QoS::AtMostOnce) == 0U,
        "QoS 0 must be 0"
    );

    check(
        static_cast<std::uint8_t>(QoS::AtLeastOnce) == 1U,
        "QoS 1 must be 1"
    );

    check(
        static_cast<std::uint8_t>(QoS::ExactlyOnce) == 2U,
        "QoS 2 must be 2"
    );

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }

    std::cout << "All MQTT C++ type tests passed\n";
    return 0;
}