cmake_minimum_required(VERSION 3.16)
project(testtools_agent_fake LANGUAGES CXX)

find_package(Threads REQUIRED)

add_executable(testtools-agent
    fake_agent_main.cpp
    ../core/agent_protocol.cpp
)

target_compile_features(testtools-agent PRIVATE cxx_std_17)
target_compile_definitions(testtools-agent PRIVATE ASIO_STANDALONE)
target_include_directories(testtools-agent PRIVATE
    ..
    ../third_party/asio/include
)
target_link_libraries(testtools-agent PRIVATE Threads::Threads)

if(WIN32)
    target_compile_definitions(testtools-agent PRIVATE NOMINMAX _WIN32_WINNT=0x0A00)
    target_link_libraries(testtools-agent PRIVATE ws2_32 mswsock)
endif()
