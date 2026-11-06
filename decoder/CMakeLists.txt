add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../external/ft8 ${CMAKE_CURRENT_BINARY_DIR}/ft8)
add_library(qo100_ft8_stream STATIC utc_slots.cpp FT8DecoderProcess.cpp)
target_include_directories(qo100_ft8_stream PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../webbridge)
target_compile_options(qo100_ft8_stream PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(qo100_ft8_stream PUBLIC Threads::Threads)
add_executable(qo100_ft8_decoder main.cpp)
target_link_libraries(qo100_ft8_decoder PRIVATE qo100_ft8_stream)
target_compile_options(qo100_ft8_decoder PRIVATE -Wall -Wextra -Wpedantic)
add_dependencies(qo100_ft8_decoder ft8-decoder)
# Keep the core beside its UDP service, also for the standalone webbridge build.
if(CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(FT8_BIN_DIR ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
else()
    set(FT8_BIN_DIR ${CMAKE_BINARY_DIR})
endif()
set_target_properties(qo100_ft8_decoder ft8-decoder PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${FT8_BIN_DIR})
# Existing start scripts using the old dummy name now start the real FT8 receiver.
add_custom_command(TARGET qo100_ft8_decoder POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -f $<TARGET_FILE_DIR:qo100_ft8_decoder>/qo100_dummy_decoder
    COMMAND ${CMAKE_COMMAND} -E create_symlink $<TARGET_FILE_NAME:qo100_ft8_decoder>
        $<TARGET_FILE_DIR:qo100_ft8_decoder>/qo100_dummy_decoder)
install(TARGETS qo100_ft8_decoder RUNTIME DESTINATION bin)
if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_executable(test_udp_text_decoder ../tests/udp_text_dummy.cpp)
    target_include_directories(test_udp_text_decoder PRIVATE ../webbridge)
    add_executable(test_ft8_slots ../tests/test_ft8_slots.cpp)
    target_link_libraries(test_ft8_slots PRIVATE qo100_ft8_stream)
    target_include_directories(test_ft8_slots PRIVATE ../webbridge)
    add_test(NAME ft8_utc_slots COMMAND test_ft8_slots)
    if(TARGET qo100_backend)
        add_test(NAME ft8_udp_receiver COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_ft8_udp.py
            $<TARGET_FILE:qo100_webbridge> $<TARGET_FILE:qo100_backend>
            $<TARGET_FILE:qo100_ft8_decoder> $<TARGET_FILE:ft8-test-signal>
            ${CMAKE_CURRENT_SOURCE_DIR}/../config/backend.conf ${CMAKE_CURRENT_SOURCE_DIR}/../web)
        set_tests_properties(ft8_udp_receiver PROPERTIES TIMEOUT 45)
    endif()
endif()
