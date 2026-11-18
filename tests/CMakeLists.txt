add_executable(decode_probe decode_probe.c)
target_link_libraries(decode_probe PRIVATE faad)
target_compile_options(decode_probe PRIVATE ${FAAD_FLAGS})

add_executable(decode_dump decode_dump.c)
target_link_libraries(decode_dump PRIVATE faad)
target_compile_options(decode_dump PRIVATE ${FAAD_FLAGS})

find_package(Python3 COMPONENTS Interpreter QUIET)
if (Python3_Interpreter_FOUND)
  add_test(
    NAME faad2_property_audio
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run_property_tests.py
      --probe $<TARGET_FILE:decode_probe>
      --dump $<TARGET_FILE:decode_dump>
      --work-dir ${CMAKE_CURRENT_BINARY_DIR}/property
  )
endif()
