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

static void test_remaining_length_encoding(void)
{
    uint8_t out[4];

    int used = mqtt_encode_remaining_length(out, 0U);
    assert(used == 1);
    assert(out[0] == 0x00U);

    used = mqtt_encode_remaining_length(out, 127U);
    assert(used == 1);
    assert(out[0] == 0x7FU);

    used = mqtt_encode_remaining_length(out, 128U);
    assert(used == 2);
    assert(out[0] == 0x80U);
    assert(out[1] == 0x01U);

    used = mqtt_encode_remaining_length(out, 16383U);
    assert(used == 2);
    assert(out[0] == 0xFF);
    assert(out[1] == 0x7F);

    used = mqtt_encode_remaining_length(out, 16384U);
    assert(used == 3);
    assert(out[0] == 0x80U);
    assert(out[1] == 0x80U);
    assert(out[2] == 0x01U);

    used = mqtt_encode_remaining_length(
        out,
        MQTT_MAX_REMAINING_LENGTH
    );

    assert(used == 4);
    assert(out[0] == 0xFFU);
    assert(out[1] == 0xFFU);
    assert(out[2] == 0xFFU);
    assert(out[3] == 0x7FU);
    assert(mqtt_encode_remaining_length(
        out,
        (size_t)MQTT_MAX_REMAINING_LENGTH + 1U
    ) == -1);

    assert(mqtt_encode_remaining_length(
        NULL,
        128U
    ) == -1);

}

static void test_remaining_length_decoding(void)
{
    size_t value = 0U;
    size_t used = 0U;

    const uint8_t one_byte[] = {0x7FU};

    assert(mqtt_decode_remaining_length(
        one_byte,
        sizeof(one_byte),
        &value,
        &used
    ) == 0);

    assert(value == 127U);
    assert(used == 1U);


    const uint8_t two_bytes[] = {0x80U, 0x01U};

    assert(mqtt_decode_remaining_length(
        two_bytes,
        sizeof(two_bytes),
        &value,
        &used
    ) == 0);

    assert(value == 128U);
    assert(used == 2U);


    const uint8_t three_bytes[] = {0x80U, 0x80U, 0x01U};

    assert(mqtt_decode_remaining_length(
        three_bytes,
        sizeof(three_bytes),
        &value,
        &used
    ) == 0);

    assert(value == 16384U);
    assert(used == 3U);

    const uint8_t boundary_two_bytes[] = {0xFFU, 0x7FU};

    assert(mqtt_decode_remaining_length(
        boundary_two_bytes,
        sizeof(boundary_two_bytes),
        &value,
        &used
    ) == 0);

    assert(value == 16383U);
    assert(used == 2U);
}

static void test_remaining_length_invalid_input(void)
{
    size_t value = 0U;
    size_t used = 0U;

    const uint8_t truncated[] = {0x80U};

    assert(mqtt_decode_remaining_length(
        truncated,
        sizeof(truncated),
        &value,
        &used
    ) == -1);


    const uint8_t too_long[] = {
        0x80U,
        0x80U,
        0x80U,
        0x80U,
        0x00U
    };

    assert(mqtt_decode_remaining_length(
        too_long,
        sizeof(too_long),
        &value,
        &used
    ) == -1);


    assert(mqtt_decode_remaining_length(
        NULL,
        0U,
        &value,
        &used
    ) == -1);
}

static void test_remaining_length_round_trip(void)
{
    const size_t values[] = {
        0U,
        1U,
        127U,
        128U,
        129U,
        16383U,
        16384U,
        MQTT_MAX_REMAINING_LENGTH
    };

    const size_t count = sizeof(values) / sizeof(values[0]);

    for (size_t i = 0U; i < count; ++i) {
        uint8_t encoded[4];

        int encoded_size = mqtt_encode_remaining_length(
            encoded,
            values[i]
        );

        assert(encoded_size > 0);

        size_t decoded = 0U;
        size_t decoded_size = 0U;

        assert(mqtt_decode_remaining_length(
            encoded,
            (size_t)encoded_size,
            &decoded,
            &decoded_size
        ) == 0);

        assert(decoded == values[i]);
        assert(decoded_size == (size_t)encoded_size);
    }
}

int main(void)
{
    test_exact_topic_match();
    test_single_level_wildcard();
    test_multi_level_wildcard();
    test_remaining_length_encoding();
    test_remaining_length_decoding();
    test_remaining_length_invalid_input();
    test_remaining_length_round_trip();

    puts("All MQTT unit tests passed.");

    return 0;
}