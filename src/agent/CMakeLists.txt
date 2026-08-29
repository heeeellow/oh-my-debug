cmake_minimum_required(VERSION 3.16)
project(testtools_agent LANGUAGES CXX)

find_package(Threads REQUIRED)

add_executable(testtools-agent
    agent_main.cpp
    agent_service.cpp
    serial_bridge.cpp
    ../core/agent_protocol.cpp
)

target_compile_features(testtools-agent PRIVATE cxx_std_17)
target_compile_definitions(testtools-agent PRIVATE ASIO_STANDALONE)
target_include_directories(testtools-agent PRIVATE
    ..
    ../../third_party/asio/include
)
target_link_libraries(testtools-agent PRIVATE Threads::Threads)

if(WIN32)
    target_compile_definitions(testtools-agent PRIVATE NOMINMAX _WIN32_WINNT=0x0A00)
    target_link_libraries(testtools-agent PRIVATE ws2_32 mswsock)
endif()

option(TESTTOOLS_BUILD_FAKE_AGENT "Build the protocol-only local test fixture" OFF)
if(TESTTOOLS_BUILD_FAKE_AGENT)
    add_executable(testtools-agent-fixture
        fake_agent_main.cpp
        ../core/agent_protocol.cpp
    )
    target_compile_features(testtools-agent-fixture PRIVATE cxx_std_17)
    target_compile_definitions(testtools-agent-fixture PRIVATE ASIO_STANDALONE)
    target_include_directories(testtools-agent-fixture PRIVATE
        ..
        ../../third_party/asio/include
    )
    target_link_libraries(testtools-agent-fixture PRIVATE Threads::Threads)
    if(WIN32)
        target_compile_definitions(testtools-agent-fixture PRIVATE NOMINMAX _WIN32_WINNT=0x0A00)
        target_link_libraries(testtools-agent-fixture PRIVATE ws2_32 mswsock)
    endif()
endif()
