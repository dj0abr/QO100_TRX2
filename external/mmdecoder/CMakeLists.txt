# Standalone RX core; use the same pinned liquid-dsp as the transceiver.
set(MM_SOURCES src/rx_probe.cpp src/pilot_afc.cpp
    vendor/hsmodem/symboltracker.cpp vendor/hsmodem/constellation.cpp
    vendor/hsmodem/frame_packer.cpp vendor/hsmodem/scrambler.cpp
    vendor/hsmodem/fec.cpp vendor/hsmodem/crc16.cpp)
add_executable(qo100_mm_probe ${MM_SOURCES})
target_include_directories(qo100_mm_probe PRIVATE src vendor/hsmodem)
target_link_libraries(qo100_mm_probe PRIVATE qo100_liquid)
target_compile_options(qo100_mm_probe PRIVATE -Wall -Wextra)
if(BUILD_TESTING)
    add_executable(qo100_mm_fixture tests/generate_fixture.cpp vendor/hsmodem/scrambler.cpp vendor/hsmodem/crc16.cpp)
    target_include_directories(qo100_mm_fixture PRIVATE src vendor/hsmodem)
    target_link_libraries(qo100_mm_fixture PRIVATE qo100_liquid)
    add_test(NAME mm_decoder_core COMMAND ${Python3_EXECUTABLE} -m unittest discover -s tests -v)
    set_tests_properties(mm_decoder_core PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        ENVIRONMENT "QO100_MM_PROBE=$<TARGET_FILE:qo100_mm_probe>;QO100_MM_FIXTURE=$<TARGET_FILE:qo100_mm_fixture>" TIMEOUT 120)
endif()
