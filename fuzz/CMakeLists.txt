cmake_minimum_required(VERSION 3.25)
project(ListopadLexerFuzz LANGUAGES CXX)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "The lexer fuzz project requires Clang with libFuzzer")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
FetchContent_Declare(
  scintilla_upstream
  URL
    https://downloads.sourceforge.net/project/scintilla/scintilla/5.6.4/scintilla564.zip
    https://www.scintilla.org/scintilla564.zip
  URL_HASH
    SHA256=3FFD69532649556978CDBE7EBE1293D9CF3259B290A7974A71E77A32BAC50E12
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_GetProperties(scintilla_upstream)
if(NOT scintilla_upstream_POPULATED)
  FetchContent_Populate(scintilla_upstream)
endif()

FetchContent_Declare(
  lexilla_upstream
  GIT_REPOSITORY https://github.com/ScintillaOrg/lexilla.git
  GIT_TAG f37af2252405159db273b455092d6b9b7b186323
  GIT_SHALLOW FALSE)
FetchContent_GetProperties(lexilla_upstream)
if(NOT lexilla_upstream_POPULATED)
  FetchContent_Populate(lexilla_upstream)
endif()

file(GLOB LEXILLA_LIBRARY_SOURCES CONFIGURE_DEPENDS
  "${lexilla_upstream_SOURCE_DIR}/lexlib/*.cxx")
set(LISTOPAD_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")

add_executable(listopad_lexer_fuzz
  lexer_fuzz.cpp
  lexilla_factory.cpp
  "${LISTOPAD_ROOT}/src/lexers/bsl_lexer.cpp"
  "${LISTOPAD_ROOT}/src/lexers/html_css_lexer.cpp"
  "${lexilla_upstream_SOURCE_DIR}/lexers/LexCSS.cxx"
  "${lexilla_upstream_SOURCE_DIR}/lexers/LexHTML.cxx"
  ${LEXILLA_LIBRARY_SOURCES})
target_include_directories(listopad_lexer_fuzz PRIVATE
  "${LISTOPAD_ROOT}/include"
  "${LISTOPAD_ROOT}/tests"
  "${lexilla_upstream_SOURCE_DIR}/include"
  "${lexilla_upstream_SOURCE_DIR}/lexlib"
  "${scintilla_upstream_SOURCE_DIR}/include")
target_compile_options(listopad_lexer_fuzz PRIVATE
  -Wall -Wextra -Wpedantic
  -fsanitize=fuzzer,address,undefined
  -fno-omit-frame-pointer)
target_link_options(listopad_lexer_fuzz PRIVATE
  -fsanitize=fuzzer,address,undefined
  -fno-omit-frame-pointer)
