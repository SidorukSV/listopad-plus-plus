cmake_minimum_required(VERSION 3.29)
set(LISTOPAD_VERSION "0.1.2" CACHE STRING "Listopad++ semantic version")
project(ListopadPP VERSION "${LISTOPAD_VERSION}" LANGUAGES C CXX RC)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Listopad++ v1 supports Windows x64 only")
endif()

option(LISTOPAD_BUILD_TESTS "Build unit tests" ON)
set(LISTOPAD_PACKAGE_NAME "ListopadPP" CACHE STRING "Sparse identity package name")
set(LISTOPAD_PACKAGE_PUBLISHER "CN=ListopadPP Development" CACHE STRING
    "Publisher subject shared by the PE and sparse identity signatures")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(pcre2 CONFIG REQUIRED)
find_package(pugixml CONFIG REQUIRED)
find_package(qjs CONFIG REQUIRED)
find_package(unofficial-tidy-html5 CONFIG REQUIRED)
find_package(uchardet CONFIG REQUIRED)
find_package(yyjson CONFIG REQUIRED)

include(cmake/Scintilla.cmake)

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

# The resource scripts pull the version through a generated header, and the
# generator does not scan .rc includes. Without this a version bump reconfigures
# and relinks but leaves the old VERSIONINFO in place, so the binaries ship
# claiming whatever version was built first.
set(LISTOPAD_VERSION_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/listopad/version.h")
set_source_files_properties(
  src/app/resources.rc src/elevated/resources.rc src/shell/resources.rc
  PROPERTIES OBJECT_DEPENDS "${LISTOPAD_VERSION_HEADER}")

add_library(listopad_core STATIC
  src/core/command_line.cpp
  src/core/document.cpp
  src/core/emmet_engine.cpp
  src/core/encoding.cpp
  src/core/file_io.cpp
  src/core/formatter.cpp
  src/core/ipc_protocol.cpp
  src/core/language.cpp
  src/core/search.cpp
  src/core/settings.cpp
  src/core/shell_registration.cpp
  src/core/signature.cpp
  src/core/strings.cpp)
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
  advapi32 bcrypt crypt32 shell32 shlwapi wintrust)

add_executable(ListopadPP WIN32
  src/app/app.cpp
  src/app/editor_window.cpp
  src/app/elevated_client.cpp
  src/app/file_watcher.cpp
  src/app/large_file_view.cpp
  src/app/single_instance.cpp
  src/app/resources.rc
  "${CMAKE_CURRENT_BINARY_DIR}/ListopadIcon.rc"
  "${CMAKE_CURRENT_BINARY_DIR}/ListopadPP.manifest")
target_include_directories(ListopadPP PRIVATE include src/app)
target_compile_definitions(ListopadPP PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(ListopadPP PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_libraries(ListopadPP PRIVATE
  listopad_core listopad_scintilla listopad_lexilla
  comctl32 comdlg32 dwmapi ole32 shell32 shlwapi uxtheme version wintrust)
set_target_properties(ListopadPP PROPERTIES OUTPUT_NAME "ListopadPP")

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

add_executable(ListopadShellHost WIN32
  src/shellhost/main.cpp
  "${CMAKE_CURRENT_BINARY_DIR}/ListopadIcon.rc")
target_compile_definitions(ListopadShellHost PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(ListopadShellHost PRIVATE /W4 /permissive- /utf-8 /EHsc)

add_library(ListopadShell SHARED
  src/shell/shell_extension.cpp
  src/shell/shell.def
  src/shell/resources.rc)
target_include_directories(ListopadShell PRIVATE include)
target_compile_definitions(ListopadShell PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(ListopadShell PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_libraries(ListopadShell PRIVATE listopad_core ole32 shell32 shlwapi)

install(TARGETS ListopadPP ListopadElevated ListopadShell ListopadShellHost
  RUNTIME DESTINATION . LIBRARY DESTINATION .)
install(FILES portable.flag DESTINATION . OPTIONAL)

if(LISTOPAD_BUILD_TESTS)
  enable_testing()
  find_package(Catch2 3 CONFIG REQUIRED)
  add_executable(listopad_tests
    tests/command_line_tests.cpp
    tests/emmet_tests.cpp
    tests/encoding_tests.cpp
    tests/file_io_tests.cpp
    tests/formatter_tests.cpp
    tests/ipc_tests.cpp
    tests/language_tests.cpp
    tests/lexer_tests.cpp
    tests/search_tests.cpp
    tests/shell_registration_tests.cpp)
  target_compile_definitions(listopad_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(listopad_tests PRIVATE /W4 /permissive- /utf-8 /EHsc)
  target_link_libraries(listopad_tests PRIVATE listopad_core listopad_lexilla Catch2::Catch2WithMain)
  include(CTest)
  include(Catch)
  catch_discover_tests(listopad_tests)
endif()
