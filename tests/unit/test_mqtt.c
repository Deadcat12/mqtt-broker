#include <stdio.h>
#include <string.h>
#include "mqtt.h"
#include <stdlib.h>

static int checks_run = 0;
static int checks_failed = 0;

#define CHECK(condition)                                                   \
    do {                                                                   \
        ++checks_run;                                                      \
                                                                           \
        if (!(condition)) {                                                \
            ++checks_failed;                                               \
            fprintf(                                                       \
                stderr,                                                    \
                "FAIL: %s:%d: %s\n",                                       \
                __FILE__,                                                  \
                __LINE__,                                                  \
                #condition                                                 \
            );                                                             \
        }                                                                  \
    } while (0)

#define CHECK_EQ_INT(expected, actual)                                  \
    do {                                                                \
        int check_expected = (expected);                                \
        int check_actual = (actual);                                    \
                                                                        \
        ++checks_run;                                                   \
                                                                        \
        if (check_expected != check_actual) {                           \
            ++checks_failed;                                            \
            fprintf(                                                    \
                stderr,                                                 \
                "FAIL: %s:%d\n"                                         \
                "  expected: %d\n"                                      \
                "  actual:   %d\n",                                     \
                __FILE__,                                               \
                __LINE__,                                               \
                check_expected,                                         \
                check_actual                                            \
            );                                                          \
        }                                                               \
    } while (0)


#define CHECK_EQ_SIZE(expected, actual)                                 \
    do {                                                               \
        size_t check_expected = (expected);                            \
        size_t check_actual = (actual);                                \
                                                                       \
        ++checks_run;                                                  \
                                                                       \
        if (check_expected != check_actual) {                           \
            ++checks_failed;                                           \
            fprintf(                                                   \
                stderr,                                                \
                "FAIL: %s:%d\n"                                        \
                "  expected: %zu\n"                                    \
                "  actual:   %zu\n",                                   \
                __FILE__,                                              \
                __LINE__,                                              \
                check_expected,                                        \
                check_actual                                           \
            );                                                         \
        }                                                              \
    } while (0)

static void test_exact_topic_match(void)
{
    CHECK(mqtt_topic_matches(
        "home/kitchen/temperature",
        "home/kitchen/temperature"
    ));

    CHECK(!mqtt_topic_matches(
        "home/kitchen/temperature",
        "home/kitchen/humidity"
    ));
}

static void test_reject_truncated_packet(void)
{
    const uint8_t packet[] = {
        0x30U,
        0x05U,
        0x00U
    };

    mqtt_packet parsed;
    char err[128] = {0};

    CHECK_EQ_INT(
        -1,
        mqtt_parse_packet(
            packet,
            sizeof(packet),
            &parsed,
            err,
            sizeof(err)
        )
    );

    CHECK(strlen(err) > 0U);
}

static void test_publish_build_parse_round_trip(void)
{
    const char *topic = "sensors/room1/temperature";
    const uint8_t payload[] = "24.5";

    size_t encoded_len = 0U;

    uint8_t *encoded = mqtt_build_publish(
        topic,
        payload,
        sizeof(payload) - 1U,
        MQTT_QOS1,
        true,
        42U,
        &encoded_len
    );

    CHECK(encoded != NULL);
    CHECK(encoded_len > 0U);

    if (!encoded) {
        return;
    }

    mqtt_packet packet;
    char err[128] = {0};

    CHECK_EQ_INT(
        0,
        mqtt_parse_packet(
            encoded,
            encoded_len,
            &packet,
            err,
            sizeof(err)
        )
    );

    CHECK_EQ_INT(MQTT_PUBLISH, packet.type);
    CHECK_EQ_INT(MQTT_QOS1, packet.body.publish.qos);

    CHECK(packet.body.publish.retain);
    CHECK(!packet.body.publish.dup);

    CHECK_EQ_INT(42, packet.body.publish.packet_id);

    CHECK(
        strcmp(
            topic,
            packet.body.publish.topic
        ) == 0
    );

    CHECK_EQ_SIZE(
        sizeof(payload) - 1U,
        packet.body.publish.payload_len
    );

    CHECK(
        memcmp(
            payload,
            packet.body.publish.payload,
            sizeof(payload) - 1U
        ) == 0
    );

    mqtt_packet_free(&packet);
    free(encoded);
}

static void test_single_level_wildcard(void)
{
    CHECK(mqtt_topic_matches(
        "home/+/temperature",
        "home/kitchen/temperature"
    ));

    CHECK(!mqtt_topic_matches(
        "home/+/temperature",
        "home/kitchen/humidity"
    ));
}

static void test_multi_level_wildcard(void)
{
    CHECK(mqtt_topic_matches(
        "sensors/#",
        "sensors/room1/temperature"
    ));

    CHECK(mqtt_topic_matches(
        "sensors/#",
        "sensors"
    ));
}

static void test_remaining_length_encoding(void)
{
    uint8_t out[4];

    int used = mqtt_encode_remaining_length(out, 0U);
    CHECK(used == 1);
    CHECK(out[0] == 0x00U);

    used = mqtt_encode_remaining_length(out, 127U);
    CHECK(used == 1);
    CHECK(out[0] == 0x7FU);

    used = mqtt_encode_remaining_length(out, 128U);
    CHECK(used == 2);
    CHECK(out[0] == 0x80U);
    CHECK(out[1] == 0x01U);

    used = mqtt_encode_remaining_length(out, 16383U);
    CHECK(used == 2);
    CHECK(out[0] == 0xFF);
    CHECK(out[1] == 0x7F);

    used = mqtt_encode_remaining_length(out, 16384U);
    CHECK(used == 3);
    CHECK(out[0] == 0x80U);
    CHECK(out[1] == 0x80U);
    CHECK(out[2] == 0x01U);

    used = mqtt_encode_remaining_length(
        out,
        MQTT_MAX_REMAINING_LENGTH
    );

    CHECK(used == 4);
    CHECK(out[0] == 0xFFU);
    CHECK(out[1] == 0xFFU);
    CHECK(out[2] == 0xFFU);
    CHECK(out[3] == 0x7FU);
    CHECK(mqtt_encode_remaining_length(
        out,
        (size_t)MQTT_MAX_REMAINING_LENGTH + 1U
    ) == -1);

    CHECK(mqtt_encode_remaining_length(
        NULL,
        128U
    ) == -1);

}

static void test_remaining_length_decoding(void)
{
    size_t value = 0U;
    size_t used = 0U;

    const uint8_t one_byte[] = {0x7FU};

    CHECK(mqtt_decode_remaining_length(
        one_byte,
        sizeof(one_byte),
        &value,
        &used
    ) == 0);

    CHECK(value == 127U);
    CHECK(used == 1U);


    const uint8_t two_bytes[] = {0x80U, 0x01U};

    CHECK(mqtt_decode_remaining_length(
        two_bytes,
        sizeof(two_bytes),
        &value,
        &used
    ) == 0);

    CHECK(value == 128U);
    CHECK(used == 2U);


    const uint8_t three_bytes[] = {0x80U, 0x80U, 0x01U};

    CHECK(mqtt_decode_remaining_length(
        three_bytes,
        sizeof(three_bytes),
        &value,
        &used
    ) == 0);

    CHECK(value == 16384U);
    CHECK(used == 3U);

    const uint8_t boundary_two_bytes[] = {0xFFU, 0x7FU};

    CHECK(mqtt_decode_remaining_length(
        boundary_two_bytes,
        sizeof(boundary_two_bytes),
        &value,
        &used
    ) == 0);

    CHECK(value == 16383U);
    CHECK(used == 2U);
}

static void test_remaining_length_invalid_input(void)
{
    size_t value = 0U;
    size_t used = 0U;

    const uint8_t truncated[] = {0x80U};

    CHECK(mqtt_decode_remaining_length(
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

    CHECK(mqtt_decode_remaining_length(
        too_long,
        sizeof(too_long),
        &value,
        &used
    ) == -1);


    CHECK(mqtt_decode_remaining_length(
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

    CHECK(encoded_size > 0);

    size_t decoded = 0U;
    size_t decoded_size = 0U;

    CHECK_EQ_INT(
        0,
        mqtt_decode_remaining_length(
            encoded,
            (size_t)encoded_size,
            &decoded,
            &decoded_size
        )
    );

    CHECK_EQ_SIZE(values[i], decoded);

    CHECK_EQ_SIZE(
        (size_t)encoded_size,
        decoded_size
    );
    }
}

static void test_topic_name_validation(void)
{
    CHECK(mqtt_topic_name_is_valid("home/kitchen"));
    CHECK(mqtt_topic_name_is_valid("sensors/room1/temperature"));

    CHECK(!mqtt_topic_name_is_valid(""));
    CHECK(!mqtt_topic_name_is_valid(NULL));

    CHECK(!mqtt_topic_name_is_valid("home/+/temperature"));
    CHECK(!mqtt_topic_name_is_valid("home/#"));
}

static void test_topic_filter_validation(void)
{   

    CHECK(mqtt_topic_filter_is_valid("home/kitchen"));
    CHECK(mqtt_topic_filter_is_valid("home/+/temperature"));
    CHECK(mqtt_topic_filter_is_valid("home/#"));
    CHECK(mqtt_topic_filter_is_valid("#"));
    CHECK(mqtt_topic_filter_is_valid("+"));

    CHECK(!mqtt_topic_filter_is_valid(""));
    CHECK(!mqtt_topic_filter_is_valid(NULL));

    CHECK(!mqtt_topic_filter_is_valid("home/room+"));
    CHECK(!mqtt_topic_filter_is_valid("home/#/temperature"));
    CHECK(!mqtt_topic_filter_is_valid("home/test#"));

    //

    CHECK(mqtt_topic_filter_is_valid("home/kitchen/+"));
    CHECK(mqtt_topic_filter_is_valid("+/kitchen/temperature"));
    CHECK(!mqtt_topic_filter_is_valid("#/kitchen/+"));
    CHECK(mqtt_topic_filter_is_valid("+/kitchen/#"));

}

static void test_reject_pingreq_with_invalid_flags(void)
{
    const uint8_t packet[] = {
        0xC1U,
        0x00U
    };

    mqtt_packet parsed;
    char err[128] = {0};

    CHECK_EQ_INT(
        -1,
        mqtt_parse_packet(
            packet,
            sizeof(packet),
            &parsed,
            err,
            sizeof(err)
        )
    );

    CHECK(strlen(err) > 0U);
}

static void test_accept_pubrel_with_required_flags(void)
{
    const uint8_t packet[] = {
        0x62U,
        0x02U,
        0x00U,
        0x2AU
    };

    mqtt_packet parsed;
    char err[128] = {0};

    CHECK_EQ_INT(
        0,
        mqtt_parse_packet(
            packet,
            sizeof(packet),
            &parsed,
            err,
            sizeof(err)
        )
    );

    CHECK_EQ_INT(MQTT_PUBREL, parsed.type);

    mqtt_packet_free(&parsed);
}

static void test_reject_pubrel_with_invalid_flags(void)
{
    const uint8_t packet[] = {
        0x60U,
        0x02U,
        0x00U,
        0x2AU
    };

    mqtt_packet parsed;
    char err[128] = {0};

    CHECK_EQ_INT(
        -1,
        mqtt_parse_packet(
            packet,
            sizeof(packet),
            &parsed,
            err,
            sizeof(err)
        )
    );

    CHECK(strlen(err) > 0U);
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
    test_topic_name_validation();
    test_topic_filter_validation();
    test_publish_build_parse_round_trip();
    test_reject_truncated_packet();
    test_reject_pingreq_with_invalid_flags();
    test_accept_pubrel_with_required_flags();
    test_reject_pubrel_with_invalid_flags();


    if (checks_failed != 0) {
        fprintf(
            stderr,
            "%d of %d MQTT checks failed.\n",
            checks_failed,
            checks_run
        );

        return 1;
    }

    printf(
        "All %d MQTT checks passed.\n",
        checks_run
    );

    return 0;
}