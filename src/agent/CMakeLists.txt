cmake_minimum_required(VERSION 3.16)
project(testtools_agent LANGUAGES CXX)

find_package(Threads REQUIRED)

add_executable(testtools-agent
    agent_main.cpp
    agent_service.cpp
    serial_bridge.cpp
    ../core/support/command_line.cpp
    ../core/protocol/agent_protocol.cpp
)

target_compile_features(testtools-agent PRIVATE cxx_std_17)
target_compile_definitions(testtools-agent PRIVATE ASIO_STANDALONE)
target_include_directories(testtools-agent PRIVATE
    ..
    ../../third_party/asio/include
)
target_link_libraries(testtools-agent PRIVATE Threads::Threads)
install(TARGETS testtools-agent RUNTIME DESTINATION .)
if(MSVC)
    target_compile_options(testtools-agent PRIVATE /utf-8)
endif()

if(WIN32)
    target_compile_definitions(testtools-agent PRIVATE NOMINMAX _WIN32_WINNT=0x0A00)
    target_link_libraries(testtools-agent PRIVATE ws2_32 mswsock)
endif()

option(TESTTOOLS_BUILD_FAKE_AGENT "Build the protocol-only local test fixture" OFF)
if(TESTTOOLS_BUILD_FAKE_AGENT)
    add_executable(testtools-agent-fixture
        fake_agent_main.cpp
        ../core/support/command_line.cpp
        ../core/protocol/agent_protocol.cpp
    )
    target_compile_features(testtools-agent-fixture PRIVATE cxx_std_17)
    target_compile_definitions(testtools-agent-fixture PRIVATE ASIO_STANDALONE)
    target_include_directories(testtools-agent-fixture PRIVATE
        ..
        ../../third_party/asio/include
    )
    target_link_libraries(testtools-agent-fixture PRIVATE Threads::Threads)
    if(MSVC)
        target_compile_options(testtools-agent-fixture PRIVATE /utf-8)
    endif()
    if(WIN32)
        target_compile_definitions(testtools-agent-fixture PRIVATE NOMINMAX _WIN32_WINNT=0x0A00)
        target_link_libraries(testtools-agent-fixture PRIVATE ws2_32 mswsock)
    endif()
endif()

if(ANDROID)
    set(TESTTOOLS_BUILD_TESTS_DEFAULT OFF)
else()
    set(TESTTOOLS_BUILD_TESTS_DEFAULT ON)
endif()
option(TESTTOOLS_BUILD_TESTS "Build host-side Agent contract tests"
       ${TESTTOOLS_BUILD_TESTS_DEFAULT})
if(TESTTOOLS_BUILD_TESTS)
    enable_testing()
    add_executable(testtools-agent-tests
        agent_service_test.cpp
        agent_service.cpp
        serial_bridge.cpp
        ../core/support/command_line.cpp
    )
    target_compile_features(testtools-agent-tests PRIVATE cxx_std_17)
    target_include_directories(testtools-agent-tests PRIVATE ..)
    target_link_libraries(testtools-agent-tests PRIVATE Threads::Threads)
    if(MSVC)
        target_compile_options(testtools-agent-tests PRIVATE /utf-8)
    endif()
    if(WIN32)
        target_compile_definitions(testtools-agent-tests PRIVATE NOMINMAX _WIN32_WINNT=0x0A00)
    endif()
    add_test(NAME agent-request-contract COMMAND testtools-agent-tests)
endif()
