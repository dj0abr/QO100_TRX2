cmake_minimum_required(VERSION 3.18)
project(QO100_TRX2 VERSION 0.8.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFTW REQUIRED IMPORTED_TARGET fftw3f)
pkg_check_modules(SAMPLERATE REQUIRED IMPORTED_TARGET samplerate)
find_path(IIO_INCLUDE_DIR iio.h REQUIRED)
find_library(IIO_LIBRARY iio REQUIRED)
find_path(AD9361_INCLUDE_DIR ad9361.h REQUIRED)
find_library(AD9361_LIBRARY ad9361 REQUIRED)
# Pin header and archive together. Never pick up an older system liquid.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Bundled liquid-dsp binaries support Linux; see external/liquid-dsp-1.8.3/README.md")
endif()
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" LIQUID_ARCH)
if(LIQUID_ARCH STREQUAL "amd64")
    set(LIQUID_ARCH x86_64)
elseif(LIQUID_ARCH STREQUAL "arm64")
    set(LIQUID_ARCH aarch64)
endif()
set(LIQUID_BUNDLE "${CMAKE_CURRENT_SOURCE_DIR}/external/liquid-dsp-1.8.3")
set(LIQUID_ARCHIVE "${LIQUID_BUNDLE}/lib/linux-${LIQUID_ARCH}/libliquid.a")
if(NOT EXISTS "${LIQUID_ARCHIVE}" OR NOT EXISTS "${LIQUID_BUNDLE}/include/liquid/liquid.h")
    message(FATAL_ERROR "Missing liquid-dsp 1.8.3 archive for Linux ${LIQUID_ARCH}; run python3 tools/build_liquid.py on the target machine")
endif()
message(STATUS "Bundled liquid-dsp 1.8.3: ${LIQUID_ARCHIVE}")
add_library(qo100_liquid STATIC IMPORTED GLOBAL)
set_target_properties(qo100_liquid PROPERTIES
    IMPORTED_LOCATION "${LIQUID_ARCHIVE}"
    INTERFACE_INCLUDE_DIRECTORIES "${LIQUID_BUNDLE}/include"
    INTERFACE_LINK_LIBRARIES "m;Threads::Threads")

# Compile the vendored driver from source on every normal project build.
set(PLUTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/Pluto")
add_executable(pluto
    ${PLUTO_DIR}/pluto.cpp ${PLUTO_DIR}/pluto_finder.cpp
    ${PLUTO_DIR}/pluto_driver.cpp ${PLUTO_DIR}/pluto_setup.cpp
    ${PLUTO_DIR}/pluto_run.cpp ${PLUTO_DIR}/udp/udp.cpp
    ${PLUTO_DIR}/kmlib/km_helper.cpp ${PLUTO_DIR}/kmlib/kmfifo.cpp
    ${PLUTO_DIR}/kmlib/kmtimer.cpp)
target_include_directories(pluto PRIVATE ${IIO_INCLUDE_DIR} ${AD9361_INCLUDE_DIR})
target_compile_definitions(pluto PRIVATE QO100_LOCAL_ONLY=1
    UDP_TXSAMPLEPORT=40809 UDP_RXSAMPLEPORT=40808 UDP_STATUSPORT=40810
    RXBUFSIZE=24000 BUFSIZE=24000)
target_compile_options(pluto PRIVATE -Wno-write-strings -Wno-narrowing)
target_link_libraries(pluto PRIVATE ${IIO_LIBRARY} ${AD9361_LIBRARY}
    qo100_liquid Threads::Threads rt m)

add_library(qo100_core src/dsp.cpp src/config.cpp src/spectrum.cpp src/protocol.cpp src/rx_channel.cpp src/rx_filter.cpp src/tx_channel.cpp src/speech_compressor.cpp)
target_include_directories(qo100_core PUBLIC include)
target_link_libraries(qo100_core PUBLIC qo100_liquid PkgConfig::FFTW PkgConfig::SAMPLERATE)
target_compile_options(qo100_core PRIVATE -Wall -Wextra -Wpedantic)
add_executable(qo100_backend src/main.cpp src/pluto_process.cpp)
target_link_libraries(qo100_backend PRIVATE qo100_core Threads::Threads)
target_compile_options(qo100_backend PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(qo100_backend PRIVATE
    QO100_DEFAULT_CONFIG="${CMAKE_CURRENT_SOURCE_DIR}/config/backend.conf")
add_dependencies(qo100_backend pluto)

add_executable(benchmark_dsp EXCLUDE_FROM_ALL tools/benchmark_dsp.cpp)
target_link_libraries(benchmark_dsp PRIVATE qo100_core)

include(CTest)
if(BUILD_TESTING)
    add_executable(test_dsp tests/test_dsp.cpp)
    target_link_libraries(test_dsp PRIVATE qo100_core)
    add_test(NAME liquid_dsp_quality COMMAND test_dsp)
    add_executable(test_core tests/test_core.cpp)
    target_link_libraries(test_core PRIVATE qo100_core)
    add_test(NAME core COMMAND test_core)
    add_executable(test_tx_channel tests/test_tx_channel.cpp)
    target_link_libraries(test_tx_channel PRIVATE qo100_core)
    add_test(NAME tx_usb_dsp COMMAND test_tx_channel)
    add_executable(test_speech_compressor tests/test_speech_compressor.cpp)
    target_link_libraries(test_speech_compressor PRIVATE qo100_core)
    add_test(NAME speech_compressor_dsp COMMAND test_speech_compressor)
    add_executable(test_rx_filter tests/test_rx_filter.cpp)
    target_link_libraries(test_rx_filter PRIVATE qo100_core)
    add_test(NAME rx_filter_dsp COMMAND test_rx_filter)
    add_executable(test_rx_channel tests/test_rx_channel.cpp)
    target_link_libraries(test_rx_channel PRIVATE qo100_core)
    add_test(NAME usb_dsp COMMAND test_rx_channel)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME bundled_liquid COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_liquid_bundle.py
        ${CMAKE_CURRENT_SOURCE_DIR} $<TARGET_FILE:qo100_backend> $<TARGET_FILE:pluto>)
    add_test(NAME protocol COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_protocol.py)
    add_test(NAME gui_tuning COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_gui_tuning.py)
    add_test(NAME audio_buffer COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_audio.py)
    add_test(NAME synthetic_udp COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_integration.py
        $<TARGET_FILE:qo100_backend> ${CMAKE_CURRENT_SOURCE_DIR}/config/backend.conf)
    add_test(NAME tx_udp COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_tx_integration.py
        $<TARGET_FILE:qo100_backend> ${CMAKE_CURRENT_SOURCE_DIR}/config/backend.conf)
    set_tests_properties(tx_udp PROPERTIES TIMEOUT 20 RUN_SERIAL TRUE)
    set_tests_properties(synthetic_udp PROPERTIES TIMEOUT 20)
    add_test(NAME rx_filter_udp COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_filter_integration.py
        $<TARGET_FILE:qo100_backend> ${CMAKE_CURRENT_SOURCE_DIR}/config/backend.conf)
    set_tests_properties(rx_filter_udp PROPERTIES TIMEOUT 20)
    add_test(NAME compressor_control_udp COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compressor_control.py
        $<TARGET_FILE:qo100_backend> ${CMAKE_CURRENT_SOURCE_DIR}/config/backend.conf)
    set_tests_properties(compressor_control_udp PROPERTIES TIMEOUT 15)
    add_test(NAME test_loops_udp COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_loops_integration.py
        $<TARGET_FILE:qo100_backend> ${CMAKE_CURRENT_SOURCE_DIR}/config/backend.conf)
    set_tests_properties(test_loops_udp PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
endif()
