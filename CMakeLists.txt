cmake_minimum_required(VERSION 3.29)
set(LISTOPAD_VERSION "0.3.1" CACHE STRING "Listopad++ semantic version")
project(ListopadPP VERSION "${LISTOPAD_VERSION}" LANGUAGES C CXX RC)

# Listopad++ does not use C++ modules. With MSVC/Ninja, the module-scanned
# rules omitted ordinary header depfiles and could retain stale ABI consumers.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Listopad++ v1 supports Windows x64 only")
endif()

option(LISTOPAD_BUILD_TESTS "Build unit tests" ON)
option(LISTOPAD_ENABLE_ASAN "Instrument application and dependency targets with AddressSanitizer" OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# The localized MSVC /showIncludes prefix is mis-decoded by this CMake/Ninja
# toolchain, so Ninja records zero implicit header dependencies. Keep explicit
# first-party header dependencies: this is intentionally coarse-grained, but it
# prevents stale object layouts and the project is small enough for the rebuild.
file(GLOB_RECURSE LISTOPAD_PROJECT_HEADERS CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")
file(GLOB_RECURSE LISTOPAD_PROJECT_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")
set_property(SOURCE ${LISTOPAD_PROJECT_SOURCES} APPEND PROPERTY
  OBJECT_DEPENDS "${LISTOPAD_PROJECT_HEADERS}")

function(listopad_enable_address_sanitizer target)
  if(NOT LISTOPAD_ENABLE_ASAN)
    return()
  endif()
  if(NOT MSVC)
    message(FATAL_ERROR "LISTOPAD_ENABLE_ASAN currently supports the MSVC-compatible Windows toolchain")
  endif()

  target_compile_options(${target} PRIVATE /fsanitize=address /Zi)
  get_target_property(target_type ${target} TYPE)
  if(target_type STREQUAL "EXECUTABLE" OR
     target_type STREQUAL "SHARED_LIBRARY" OR
     target_type STREQUAL "MODULE_LIBRARY")
    target_link_options(${target} PRIVATE /INCREMENTAL:NO)
  endif()
endfunction()

find_package(pcre2 CONFIG REQUIRED)
find_package(pugixml CONFIG REQUIRED)
find_package(qjs CONFIG REQUIRED)
find_package(unofficial-tidy-html5 CONFIG REQUIRED)
find_package(uchardet CONFIG REQUIRED)
find_package(yyjson CONFIG REQUIRED)

include(cmake/Scintilla.cmake)
listopad_enable_address_sanitizer(listopad_scintilla)
listopad_enable_address_sanitizer(listopad_lexilla)

configure_file(src/app/app.manifest.in
               "${CMAKE_CURRENT_BINARY_DIR}/ListopadPP.manifest" @ONLY)
configure_file(include/listopad/version.h.in
               "${CMAKE_CURRENT_BINARY_DIR}/generated/listopad/version.h" @ONLY)
set(LISTOPAD_ICON_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/listopad-plus-plus-multisize.ico")
configure_file(src/common/icon.rc.in
               "${CMAKE_CURRENT_BINARY_DIR}/ListopadIcon.rc" @ONLY)
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/ListopadIcon.rc"
  PROPERTIES OBJECT_DEPENDS "${LISTOPAD_ICON_FILE}")

# Resource scripts and the current Ninja/MSVC configuration do not reliably
# report dependencies on the generated version header. Keep the direct C++ and
# .rc consumers explicit so an incremental version bump cannot leave either the
# About dialog or VERSIONINFO at the previous version.
set(LISTOPAD_VERSION_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/listopad/version.h")
set_source_files_properties(
  src/app/command_controller.cpp
  src/app/document_view_controller.cpp
  src/app/editor_window.cpp
  PROPERTIES OBJECT_DEPENDS "${LISTOPAD_VERSION_HEADER}")
set_source_files_properties(
  src/app/resources.rc
  PROPERTIES OBJECT_DEPENDS
    "${LISTOPAD_VERSION_HEADER};${CMAKE_CURRENT_SOURCE_DIR}/src/app/resource.h")
set_source_files_properties(
  src/elevated/resources.rc
  PROPERTIES OBJECT_DEPENDS "${LISTOPAD_VERSION_HEADER}")

add_library(listopad_core STATIC
  src/core/command_line.cpp
  src/core/document.cpp
  src/core/document_map_cache.cpp
  src/core/emmet_engine.cpp
  src/core/encoding.cpp
  src/core/file_io.cpp
  src/core/formatter.cpp
  src/core/hex_view.cpp
  src/core/ipc_protocol.cpp
  src/core/language.cpp
  src/core/performance_log.cpp
  src/core/performance_log_binary.cpp
  src/core/search.cpp
  src/core/session.cpp
  src/core/settings.cpp
  src/core/shell_registration.cpp
  src/core/signature.cpp
  src/core/strings.cpp
  src/core/technology_log.cpp
  src/core/ui_layout.cpp)
target_include_directories(listopad_core PUBLIC
  "${CMAKE_CURRENT_BINARY_DIR}/generated" include PRIVATE assets)
target_compile_definitions(listopad_core PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN PCRE2_CODE_UNIT_WIDTH=8)
target_compile_options(listopad_core PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_libraries(listopad_core PUBLIC
  PCRE2::8BIT
  pugixml::pugixml
  qjs
  unofficial::tidy-html5::tidy
  uchardet::libuchardet
  yyjson::yyjson
  advapi32 bcrypt crypt32 pdh shell32 shlwapi wintrust)
listopad_enable_address_sanitizer(listopad_core)

add_executable(ListopadPP WIN32
  src/app/app.cpp
  src/app/command_controller.cpp
  src/app/document_map.cpp
  src/app/document_view_controller.cpp
  src/app/editor_window.cpp
  src/app/elevated_client.cpp
  src/app/file_watcher.cpp
  src/app/hex_view_window.cpp
  src/app/large_file_view.cpp
  src/app/performance_log_controller.cpp
  src/app/performance_log_view.cpp
  src/app/recovery_controller.cpp
  src/app/recovery_dialog.cpp
  src/app/search_controller.cpp
  src/app/settings_dialog.cpp
  src/app/single_instance.cpp
  src/app/tab_controller.cpp
  src/app/technology_log_controller.cpp
  src/app/technology_log_view.cpp
  src/app/window_layout_controller.cpp
  src/app/resources.rc
  "${CMAKE_CURRENT_BINARY_DIR}/ListopadIcon.rc"
  "${CMAKE_CURRENT_BINARY_DIR}/ListopadPP.manifest")
target_include_directories(ListopadPP PRIVATE include src/app)
target_compile_definitions(ListopadPP PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(ListopadPP PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_libraries(ListopadPP PRIVATE
  listopad_core listopad_scintilla listopad_lexilla
  comctl32 comdlg32 dwmapi msimg32 ole32 shell32 shlwapi uxtheme version wintrust)
set_target_properties(ListopadPP PROPERTIES OUTPUT_NAME "ListopadPP")
listopad_enable_address_sanitizer(ListopadPP)

add_executable(ListopadElevated WIN32
  src/elevated/main.cpp
  src/elevated/elevated.manifest
  src/elevated/resources.rc
  "${CMAKE_CURRENT_BINARY_DIR}/ListopadIcon.rc")
target_include_directories(ListopadElevated PRIVATE include)
target_compile_definitions(ListopadElevated PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(ListopadElevated PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_libraries(ListopadElevated PRIVATE listopad_core advapi32 bcrypt shell32 wintrust)
target_link_options(ListopadElevated PRIVATE "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")
listopad_enable_address_sanitizer(ListopadElevated)

install(TARGETS ListopadPP ListopadElevated
  RUNTIME DESTINATION . LIBRARY DESTINATION .)
install(FILES portable.flag DESTINATION . OPTIONAL)

if(LISTOPAD_BUILD_TESTS)
  enable_testing()
  find_package(Catch2 3 CONFIG REQUIRED)
  add_executable(listopad_tests
    src/app/recovery_controller.cpp
    src/app/tab_controller.cpp
    tests/command_line_tests.cpp
    tests/document_map_cache_tests.cpp
    tests/document_map_tests.cpp
    tests/document_mutation_tests.cpp
    tests/document_session_tests.cpp
    tests/emmet_tests.cpp
    tests/encoding_tests.cpp
    tests/file_io_tests.cpp
    tests/formatter_tests.cpp
    tests/hex_view_tests.cpp
    tests/ipc_tests.cpp
    tests/language_tests.cpp
    tests/lexer_tests.cpp
    tests/performance_log_tests.cpp
    tests/recovery_controller_tests.cpp
    tests/search_tests.cpp
    tests/session_tests.cpp
    tests/settings_tests.cpp
    tests/tab_controller_tests.cpp
    tests/technology_log_tests.cpp
    tests/ui_layout_tests.cpp)
  target_compile_definitions(listopad_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_include_directories(listopad_tests PRIVATE src/app)
  target_compile_options(listopad_tests PRIVATE /W4 /permissive- /utf-8 /EHsc)
  target_link_libraries(listopad_tests PRIVATE listopad_core listopad_lexilla Catch2::Catch2WithMain)
  listopad_enable_address_sanitizer(listopad_tests)

  add_executable(listopad_benchmarks
    benchmarks/core_benchmarks.cpp)
  target_compile_definitions(listopad_benchmarks PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(listopad_benchmarks PRIVATE
    /W4 /permissive- /utf-8 /EHsc)
  target_link_libraries(listopad_benchmarks PRIVATE listopad_core)
  listopad_enable_address_sanitizer(listopad_benchmarks)

  include(CTest)
  include(Catch)
  catch_discover_tests(listopad_tests)
  if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_test(NAME performance_budgets COMMAND listopad_benchmarks)
    set_tests_properties(performance_budgets PROPERTIES
      LABELS "performance"
      TIMEOUT 15)
  endif()
endif()
