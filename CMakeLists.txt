cmake_minimum_required(VERSION 3.16)

project(sol_mqtt_broker LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(
    ENABLE_SANITIZERS
    "Enable AddressSanitizer and UndefinedBehaviorSanitizer"
    OFF
)

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

if(ENABLE_SANITIZERS)

    if(WIN32)
        message(
            FATAL_ERROR
            "ENABLE_SANITIZERS is supported in this project on Linux with GCC/Clang"
        )
    endif()
    
    target_compile_options(
        test-mqtt
        PRIVATE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
    )

    target_link_options(
        test-mqtt
        PRIVATE
        -fsanitize=address,undefined
    )
endif()

add_test(
    NAME mqtt-unit-tests
    COMMAND test-mqtt
)