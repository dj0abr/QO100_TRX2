find_package(PkgConfig REQUIRED)
pkg_check_modules(BEACON_FFTW REQUIRED IMPORTED_TARGET fftw3f)
add_library(qo100_decoder_budget STATIC cpu_budget.cpp)
target_include_directories(qo100_decoder_budget PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(qo100_decoder_budget PUBLIC Threads::Threads)
target_compile_options(qo100_decoder_budget PRIVATE -Wall -Wextra -Wpedantic)
add_library(qo100_beacon_dsp STATIC beacon_dsp.cpp beacon_fec.cpp)
target_include_directories(qo100_beacon_dsp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(qo100_beacon_dsp PUBLIC PkgConfig::BEACON_FFTW)
target_compile_options(qo100_beacon_dsp PRIVATE -Wall -Wextra -Wpedantic)
add_library(qo100_ft_audio STATIC ft_audio.cpp)
target_include_directories(qo100_ft_audio PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(qo100_ft_audio PUBLIC libft8::libft8)
target_compile_options(qo100_ft_audio PRIVATE -Wall -Wextra -Wpedantic)
add_executable(qo100_ft8_decoder main.cpp)
target_include_directories(qo100_ft8_decoder PRIVATE ../include)
target_link_libraries(qo100_ft8_decoder PRIVATE qo100_ft_audio qo100_decoder_budget)
target_compile_options(qo100_ft8_decoder PRIVATE -Wall -Wextra -Wpedantic)
install(TARGETS qo100_ft8_decoder RUNTIME DESTINATION bin)
# The RX4 beacon receiver runs beside the RX3 FT8 service.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_executable(qo100_beacon_decoder beacon_main.cpp)
target_link_libraries(qo100_beacon_decoder PRIVATE qo100_beacon_dsp qo100_decoder_budget Threads::Threads)
target_compile_options(qo100_beacon_decoder PRIVATE -Wall -Wextra -Wpedantic)
if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set_target_properties(qo100_ft8_decoder qo100_beacon_decoder PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()
install(TARGETS qo100_beacon_decoder RUNTIME DESTINATION bin)
if(BUILD_TESTING)
    add_executable(test_beacon_fec ../tests/test_beacon_fec.cpp)
    target_link_libraries(test_beacon_fec PRIVATE qo100_beacon_dsp)
    add_test(NAME beacon_cpp_fec COMMAND test_beacon_fec ${CMAKE_CURRENT_SOURCE_DIR}/../tests/data/beacon)
    add_executable(test_decoder_budget_load ../tests/decoder_budget_load.cpp)
    target_link_libraries(test_decoder_budget_load PRIVATE qo100_decoder_budget Threads::Threads)
    add_test(NAME decoder_cpu_budget COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_decoder_cpu_budget.py $<TARGET_FILE:test_decoder_budget_load>)
    set_tests_properties(decoder_cpu_budget PROPERTIES TIMEOUT 25)
    add_test(NAME beacon_recording COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_beacon_decoder.py $<TARGET_FILE:qo100_beacon_decoder>)
    set_tests_properties(beacon_recording PROPERTIES TIMEOUT 60)
    add_executable(test_udp_text_decoder ../tests/udp_text_dummy.cpp)
    target_include_directories(test_udp_text_decoder PRIVATE ../webbridge ../include)
    add_executable(ft-test-pcm ../tests/ft_test_pcm.cpp)
    target_link_libraries(ft-test-pcm PRIVATE libft8::libft8)
    add_executable(test_ft8_slots ../tests/test_ft8_slots.cpp)
    target_link_libraries(test_ft8_slots PRIVATE qo100_ft_audio)
    target_include_directories(test_ft8_slots PRIVATE ../webbridge)
    add_test(NAME ft8_utc_slots COMMAND test_ft8_slots)
    if(TARGET qo100_backend)
        add_test(NAME parallel_monitor_recordings COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_monitor_recordings.py
            $<TARGET_FILE:qo100_webbridge> $<TARGET_FILE:qo100_backend>
            $<TARGET_FILE:qo100_ft8_decoder> $<TARGET_FILE:qo100_beacon_decoder> $<TARGET_FILE:ft-test-pcm>
            ${CMAKE_CURRENT_SOURCE_DIR}/../config/backend.conf ${CMAKE_CURRENT_SOURCE_DIR}/../web)
        set_tests_properties(parallel_monitor_recordings PROPERTIES TIMEOUT 45)
        add_test(NAME beacon_udp_receiver COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_beacon_udp.py
            $<TARGET_FILE:qo100_webbridge> $<TARGET_FILE:qo100_backend>
            $<TARGET_FILE:qo100_beacon_decoder>
            ${CMAKE_CURRENT_SOURCE_DIR}/../config/backend.conf ${CMAKE_CURRENT_SOURCE_DIR}/../web)
        set_tests_properties(beacon_udp_receiver PROPERTIES TIMEOUT 60)
        add_test(NAME ft8_udp_receiver COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_ft8_udp.py
            $<TARGET_FILE:qo100_webbridge> $<TARGET_FILE:qo100_backend>
            $<TARGET_FILE:qo100_ft8_decoder> $<TARGET_FILE:ft-test-pcm>
            ${CMAKE_CURRENT_SOURCE_DIR}/../config/backend.conf ${CMAKE_CURRENT_SOURCE_DIR}/../web)
        set_tests_properties(ft8_udp_receiver PROPERTIES TIMEOUT 45)
        add_test(NAME ft8_rx2_tx2_websocket COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_ft8_rx2.py
            $<TARGET_FILE:qo100_webbridge> $<TARGET_FILE:qo100_backend>
            $<TARGET_FILE:qo100_ft8_decoder> $<TARGET_FILE:ft-test-pcm>
            ${CMAKE_CURRENT_SOURCE_DIR}/../config/backend.conf ${CMAKE_CURRENT_SOURCE_DIR}/../web)
        set_tests_properties(ft8_rx2_tx2_websocket PROPERTIES TIMEOUT 155 RUN_SERIAL TRUE)
    endif()
endif()
