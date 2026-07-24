cmake_minimum_required(VERSION 3.10)
project(sol_mqtt_broker C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Wpedantic -O0)

add_executable(sol-broker
    src/main.c
    src/mqtt.c
    src/broker.c
)

target_include_directories(sol-broker PRIVATE src)


enable_testing()

add_executable(test-mqtt
    tests/unit/test_mqtt.c
    src/mqtt.c
)

target_include_directories(test-mqtt PRIVATE src)

add_test(
    NAME mqtt-unit-tests
    COMMAND test-mqtt
)