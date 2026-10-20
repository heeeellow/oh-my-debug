cmake_minimum_required(VERSION 3.16)
project(testtools_host_tests LANGUAGES CXX)

find_package(Threads REQUIRED)
enable_testing()

get_filename_component(TESTTOOLS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

add_executable(testtools-core-tests
    core_application_tests.cpp
    ${TESTTOOLS_ROOT}/src/application/agent/agent_target.cpp
    ${TESTTOOLS_ROOT}/src/application/agent/agent_bundle.cpp
    ${TESTTOOLS_ROOT}/src/application/agent/board_profile.cpp
    ${TESTTOOLS_ROOT}/src/application/agent/board_test_job_client.cpp
    ${TESTTOOLS_ROOT}/src/application/serial/serial_session.cpp
    ${TESTTOOLS_ROOT}/src/application/serial/at_command_library.cpp
    ${TESTTOOLS_ROOT}/src/application/serial/serial_workspace_state.cpp
    ${TESTTOOLS_ROOT}/src/application/serial/serial_terminal.cpp
    ${TESTTOOLS_ROOT}/src/application/support/background_task_group.cpp
    ${TESTTOOLS_ROOT}/src/core/protocol/agent_protocol.cpp
    ${TESTTOOLS_ROOT}/src/core/protocol/json_value.cpp
    ${TESTTOOLS_ROOT}/src/core/serial/async_serial_sender.cpp
    ${TESTTOOLS_ROOT}/src/core/serial/codec.cpp
    ${TESTTOOLS_ROOT}/src/core/serial/serial_test_engine.cpp
    ${TESTTOOLS_ROOT}/src/core/support/command_line.cpp
)

target_compile_features(testtools-core-tests PRIVATE cxx_std_17)
target_include_directories(testtools-core-tests PRIVATE ${TESTTOOLS_ROOT}/src)
target_link_libraries(testtools-core-tests PRIVATE Threads::Threads)
if(MSVC)
    target_compile_options(testtools-core-tests PRIVATE /utf-8)
endif()

add_test(NAME core-application-contract COMMAND testtools-core-tests)
