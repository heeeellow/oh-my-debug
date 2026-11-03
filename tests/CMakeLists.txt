cmake_minimum_required(VERSION 3.16)
project(testtools_host_tests LANGUAGES CXX)

find_package(Threads REQUIRED)
enable_testing()

get_filename_component(TESTTOOLS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

add_executable(testtools-core-tests
    core_application_tests.cpp
    ${TESTTOOLS_ROOT}/src/application/agent/agent_service_status.cpp
    ${TESTTOOLS_ROOT}/src/application/device/screen_mirror_service.cpp
    ${TESTTOOLS_ROOT}/src/application/agent/agent_target.cpp
    ${TESTTOOLS_ROOT}/src/application/agent/agent_bundle.cpp
    ${TESTTOOLS_ROOT}/src/application/agent/board_profile.cpp
    ${TESTTOOLS_ROOT}/src/application/agent/board_test_job_client.cpp
    ${TESTTOOLS_ROOT}/src/application/serial/serial_session.cpp
    ${TESTTOOLS_ROOT}/src/application/serial/serial_recorder.cpp
    ${TESTTOOLS_ROOT}/src/application/serial/serial_port_claims.cpp
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

add_executable(testtools-file-tests file_transfer_tests.cpp
    ${TESTTOOLS_ROOT}/src/application/device/file_transfer_service.cpp)
target_compile_features(testtools-file-tests PRIVATE cxx_std_17)
target_include_directories(testtools-file-tests PRIVATE ${TESTTOOLS_ROOT}/src)
target_link_libraries(testtools-file-tests PRIVATE Threads::Threads)
if(MSVC)
    target_compile_options(testtools-file-tests PRIVATE /utf-8)
endif()
add_test(NAME file-transfer-contract COMMAND testtools-file-tests)

add_executable(testtools-serial-recording-tests serial_recording_tests.cpp
    ${TESTTOOLS_ROOT}/src/application/serial/serial_session.cpp
    ${TESTTOOLS_ROOT}/src/application/serial/serial_recorder.cpp
    ${TESTTOOLS_ROOT}/src/application/serial/serial_receive_queue.cpp
    ${TESTTOOLS_ROOT}/src/core/protocol/json_value.cpp
    ${TESTTOOLS_ROOT}/src/core/serial/async_serial_sender.cpp
    ${TESTTOOLS_ROOT}/src/core/serial/serial_test_engine.cpp
    ${TESTTOOLS_ROOT}/src/core/serial/codec.cpp)
target_compile_features(testtools-serial-recording-tests PRIVATE cxx_std_17)
target_include_directories(testtools-serial-recording-tests PRIVATE ${TESTTOOLS_ROOT}/src)
target_link_libraries(testtools-serial-recording-tests PRIVATE Threads::Threads)
if(MSVC)
    target_compile_options(testtools-serial-recording-tests PRIVATE /utf-8)
endif()
add_test(NAME serial-recording-contract COMMAND testtools-serial-recording-tests)
set_tests_properties(serial-recording-contract PROPERTIES TIMEOUT 30)

add_executable(testtools-maintenance-tests device_maintenance_tests.cpp
    ${TESTTOOLS_ROOT}/src/application/device/device_maintenance_service.cpp)
target_compile_features(testtools-maintenance-tests PRIVATE cxx_std_17)
target_include_directories(testtools-maintenance-tests PRIVATE ${TESTTOOLS_ROOT}/src)
target_link_libraries(testtools-maintenance-tests PRIVATE Threads::Threads)
if(MSVC)
    target_compile_options(testtools-maintenance-tests PRIVATE /utf-8)
endif()
add_test(NAME device-maintenance-contract COMMAND testtools-maintenance-tests)
set_tests_properties(device-maintenance-contract PROPERTIES TIMEOUT 15)

add_executable(testtools-board-info-tests board_info_tests.cpp
    ${TESTTOOLS_ROOT}/src/application/agent/board_info_service.cpp
    ${TESTTOOLS_ROOT}/src/application/agent/board_test_job_client.cpp
    ${TESTTOOLS_ROOT}/src/core/protocol/json_value.cpp)
target_compile_features(testtools-board-info-tests PRIVATE cxx_std_17)
target_include_directories(testtools-board-info-tests PRIVATE ${TESTTOOLS_ROOT}/src)
target_link_libraries(testtools-board-info-tests PRIVATE Threads::Threads)
if(MSVC)
    target_compile_options(testtools-board-info-tests PRIVATE /utf-8)
endif()
add_test(NAME board-info-contract COMMAND testtools-board-info-tests)
set_tests_properties(board-info-contract PROPERTIES TIMEOUT 15)
