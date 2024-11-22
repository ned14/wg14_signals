cmake_minimum_required(VERSION 3.10 FATAL_ERROR)

project(wg14_signals LANGUAGES C CXX)
include(GNUInstallDirs)
enable_testing()

set(LIBRARY_SOURCES
  "src/wg14_signals/async_signal_safe_tls.c"
  "src/wg14_signals/current_thread_id.c"

  # Not mandatory and can be substituted
  "src/wg14_signals/thread_atexit.cpp"
)

add_library(${PROJECT_NAME}_static STATIC ${LIBRARY_SOURCES})
add_library(${PROJECT_NAME}_shared SHARED ${LIBRARY_SOURCES})
foreach(target static shared)
  target_compile_features(${PROJECT_NAME}_${target} PRIVATE c_std_23 cxx_std_11)
  target_include_directories(${PROJECT_NAME}_${target} PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
  )
  set_target_properties(${PROJECT_NAME}_${target} PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  )
  if(WIN32)
  else()
    target_compile_options(${PROJECT_NAME}_${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endforeach()
install(TARGETS ${PROJECT_NAME}_static ${PROJECT_NAME}_shared
        EXPORT ${PROJECT_NAME}Exports
        INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
set_target_properties(${PROJECT_NAME}_static PROPERTIES EXPORT_NAME sl)
set_target_properties(${PROJECT_NAME}_shared PROPERTIES EXPORT_NAME dl)

configure_file(
  "${CMAKE_CURRENT_LIST_DIR}/cmake/ProjectConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}Config.cmake"
  @ONLY
)
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}Config.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}"
)
install(EXPORT ${PROJECT_NAME}Exports
  NAMESPACE ${PROJECT_NAME}::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}"
)

if(PROJECT_IS_TOP_LEVEL AND (NOT DEFINED BUILD_TESTING OR BUILD_TESTING))
  function(add_code_example target)
    cmake_parse_arguments(ADD_EXAMPLE "" "" "SOURCES;FEATURES;PROPERTIES" ${ARGN})
    foreach(type static shared)
      add_executable(${target}_${type} ${ADD_EXAMPLE_SOURCES})
      target_compile_features(${target}_${type} PRIVATE ${ADD_EXAMPLE_FEATURES})
      target_link_libraries(${target}_${type} PRIVATE ${PROJECT_NAME}_${type})
      set_target_properties(${target}_${type} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        ${ADD_EXAMPLE_PROPERTIES}
      )
    endforeach()
  endfunction()
  function(add_code_test target)
    add_code_example(${target} ${ARGN})
    add_test(NAME ${target}_static COMMAND $<TARGET_FILE:${target}_static>)
    add_test(NAME ${target}_shared COMMAND $<TARGET_FILE:${target}_shared>)
  endfunction()

  add_subdirectory("test")
endif()

