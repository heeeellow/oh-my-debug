cmake_minimum_required(VERSION 3.24)
project(testtools_native_ui_checks LANGUAGES CXX)
if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "Native checks require the Windows MSVC product build")
endif()
find_package(Qt6 REQUIRED COMPONENTS Widgets Test)
get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
if(NOT EXISTS "${ROOT}/build/intermediate/desktop/x64/Release/main_window.obj")
    message(FATAL_ERROR "Build TestTools Release first; checks link the actual product objects")
endif()
file(GLOB HOST_OBJECTS "${ROOT}/build/intermediate/desktop/x64/Release/*.obj")
list(FILTER HOST_OBJECTS EXCLUDE REGEX "/main\\.obj$")
foreach(target mock-adb backend-smoke ui-smoke serial-smoke service-icons-smoke device-smoke directory-perf mirror-host-probe)
    add_executable(${target} ${target}.cpp ${HOST_OBJECTS})
    target_include_directories(${target} PRIVATE ${ROOT}/src ${ROOT}/third_party/asio/include)
    target_compile_features(${target} PRIVATE cxx_std_17)
    target_compile_definitions(${target} PRIVATE UNICODE _UNICODE NOMINMAX _WIN32_WINNT=0x0A00 ASIO_STANDALONE)
    target_compile_options(${target} PRIVATE /utf-8)
    target_link_options(${target} PRIVATE /LTCG)
    target_link_libraries(${target} PRIVATE Qt6::Widgets Qt6::Test shell32 ole32 setupapi advapi32 user32 gdi32 ws2_32 mswsock bcrypt)
endforeach()
enable_testing()
add_test(NAME file-workspace-ui COMMAND ui-smoke)
add_test(NAME serial-workspace-ui COMMAND serial-smoke "${ROOT}/build/iteration-0.8.0")
add_test(NAME native-file-backend COMMAND backend-smoke $<TARGET_FILE:mock-adb>)
add_test(NAME service-icons-ui COMMAND service-icons-smoke)
set_tests_properties(file-workspace-ui serial-workspace-ui native-file-backend service-icons-ui PROPERTIES
    WORKING_DIRECTORY "${ROOT}"
    RUN_SERIAL TRUE
    TIMEOUT 60
    ENVIRONMENT "QT_SCALE_FACTOR=1;QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::Core>/../plugins/platforms;TESTTOOLS_DATA_DIR=${ROOT}/build/iteration-0.8.0/maintained-fixture-data"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
# device-smoke and mirror-host-probe touch a real device and are never CTest defaults.
