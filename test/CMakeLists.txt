cmake_minimum_required(VERSION 3.16)
project(s5_node_otbr_factory_host_tests C)

enable_testing()

if(NOT DEFINED IDF_PATH)
    if(DEFINED ENV{IDF_PATH})
        set(IDF_PATH "$ENV{IDF_PATH}")
    else()
        message(FATAL_ERROR "Set IDF_PATH to the ESP-IDF 5.5.3 directory")
    endif()
endif()

add_executable(factory_host_tests
    test_factory.c
    ../main/factory_manifest.c
    ../main/factory_request.c
    ../main/factory_safety.c
    ../main/factory_session.c
    ../main/factory_types.c
    "${IDF_PATH}/components/json/cJSON/cJSON.c"
)

target_include_directories(factory_host_tests PRIVATE
    ../main/include
    "${IDF_PATH}/components/json/cJSON"
)

if(MSVC)
    target_compile_options(factory_host_tests PRIVATE /W4 /WX)
else()
    target_compile_options(factory_host_tests PRIVATE
        -Wall -Wextra -Werror -Wconversion -Wshadow -pedantic)
    target_link_libraries(factory_host_tests PRIVATE m)
endif()

add_test(NAME factory_host_tests COMMAND factory_host_tests)
