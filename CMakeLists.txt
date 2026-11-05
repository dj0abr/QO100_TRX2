cmake_minimum_required(VERSION 3.18)
project(QO100_TRX2 VERSION 0.2.0 LANGUAGES CXX)
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
find_library(LIQUID_LIBRARY liquid REQUIRED)

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
    ${LIQUID_LIBRARY} Threads::Threads rt m)

add_library(qo100_core src/config.cpp src/spectrum.cpp src/protocol.cpp src/rx_channel.cpp)
target_include_directories(qo100_core PUBLIC include)
target_link_libraries(qo100_core PUBLIC PkgConfig::FFTW PkgConfig::SAMPLERATE)
target_compile_options(qo100_core PRIVATE -Wall -Wextra -Wpedantic)
add_executable(qo100_backend src/main.cpp src/pluto_process.cpp)
target_link_libraries(qo100_backend PRIVATE qo100_core Threads::Threads)
target_compile_options(qo100_backend PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(qo100_backend PRIVATE
    QO100_DEFAULT_CONFIG="${CMAKE_CURRENT_SOURCE_DIR}/config/backend.conf")
add_dependencies(qo100_backend pluto)

include(CTest)
if(BUILD_TESTING)
    add_executable(test_core tests/test_core.cpp)
    target_link_libraries(test_core PRIVATE qo100_core)
    add_test(NAME core COMMAND test_core)
    add_executable(test_rx_channel tests/test_rx_channel.cpp)
    target_link_libraries(test_rx_channel PRIVATE qo100_core)
    add_test(NAME usb_dsp COMMAND test_rx_channel)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME protocol COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_protocol.py)
    add_test(NAME audio_buffer COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_audio.py)
    add_test(NAME synthetic_udp COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_integration.py
        $<TARGET_FILE:qo100_backend> ${CMAKE_CURRENT_SOURCE_DIR}/config/backend.conf)
    set_tests_properties(synthetic_udp PROPERTIES TIMEOUT 20)
endif()
