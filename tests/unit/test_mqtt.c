#include <assert.h>
#include <stdio.h>

#include "mqtt.h"


static void test_exact_topic_match(void)
{
    assert(mqtt_topic_matches(
        "home/kitchen/temperature",
        "home/kitchen/temperature"
    ));

    assert(!mqtt_topic_matches(
        "home/kitchen/temperature",
        "home/kitchen/humidity"
    ));
}


static void test_single_level_wildcard(void)
{
    assert(mqtt_topic_matches(
        "home/+/temperature",
        "home/kitchen/temperature"
    ));

    assert(!mqtt_topic_matches(
        "home/+/temperature",
        "home/kitchen/humidity"
    ));
}


static void test_multi_level_wildcard(void)
{
    assert(mqtt_topic_matches(
        "sensors/#",
        "sensors/room1/temperature"
    ));

    assert(mqtt_topic_matches(
        "sensors/#",
        "sensors"
    ));
}


int main(void)
{
    test_exact_topic_match();
    test_single_level_wildcard();
    test_multi_level_wildcard();

    puts("All MQTT topic matching unit tests passed.");

    return 0;
}