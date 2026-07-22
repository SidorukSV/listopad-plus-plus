include(FetchContent)

FetchContent_Declare(
  scintilla_upstream
  # The project web server occasionally rejects GitHub-hosted runners. The
  # official SourceForge release mirror is primary; the upstream site remains
  # a fallback. Both are verified against the same pinned release hash.
  URL
    https://downloads.sourceforge.net/project/scintilla/scintilla/5.6.4/scintilla564.zip
    https://www.scintilla.org/scintilla564.zip
  URL_HASH SHA256=3FFD69532649556978CDBE7EBE1293D9CF3259B290A7974A71E77A32BAC50E12
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_GetProperties(scintilla_upstream)
if(NOT scintilla_upstream_POPULATED)
  FetchContent_Populate(scintilla_upstream)
endif()
set(SCINTILLA_SOURCE_DIR "${scintilla_upstream_SOURCE_DIR}" CACHE INTERNAL "Scintilla source")

file(GLOB SCINTILLA_CORE_SOURCES CONFIGURE_DEPENDS
  "${SCINTILLA_SOURCE_DIR}/src/*.cxx")
set(SCINTILLA_WIN32_SOURCES
  "${SCINTILLA_SOURCE_DIR}/win32/HanjaDic.cxx"
  "${SCINTILLA_SOURCE_DIR}/win32/ListBox.cxx"
  "${SCINTILLA_SOURCE_DIR}/win32/PlatWin.cxx"
  "${SCINTILLA_SOURCE_DIR}/win32/ScintillaWin.cxx"
  "${SCINTILLA_SOURCE_DIR}/win32/SurfaceD2D.cxx"
  "${SCINTILLA_SOURCE_DIR}/win32/SurfaceGDI.cxx")

add_library(listopad_scintilla STATIC ${SCINTILLA_CORE_SOURCES} ${SCINTILLA_WIN32_SOURCES})
target_include_directories(listopad_scintilla PUBLIC
  "${SCINTILLA_SOURCE_DIR}/include"
  PRIVATE "${SCINTILLA_SOURCE_DIR}/src" "${SCINTILLA_SOURCE_DIR}/win32")
target_compile_definitions(listopad_scintilla PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(listopad_scintilla PRIVATE /W3 /utf-8)
target_link_libraries(listopad_scintilla PUBLIC d2d1 dwrite gdi32 imm32 ole32 oleaut32 uuid)

FetchContent_Declare(
  lexilla_upstream
  GIT_REPOSITORY https://github.com/ScintillaOrg/lexilla.git
  GIT_TAG f37af2252405159db273b455092d6b9b7b186323
  GIT_SHALLOW FALSE
)
FetchContent_GetProperties(lexilla_upstream)
if(NOT lexilla_upstream_POPULATED)
  FetchContent_Populate(lexilla_upstream)
endif()
set(LEXILLA_SOURCE_DIR "${lexilla_upstream_SOURCE_DIR}" CACHE INTERNAL "Lexilla source")

file(GLOB LEXILLA_LEXER_SOURCES CONFIGURE_DEPENDS "${LEXILLA_SOURCE_DIR}/lexers/*.cxx")
file(GLOB LEXILLA_LIBRARY_SOURCES CONFIGURE_DEPENDS "${LEXILLA_SOURCE_DIR}/lexlib/*.cxx")
add_library(listopad_lexilla STATIC
  ${LEXILLA_LEXER_SOURCES}
  ${LEXILLA_LIBRARY_SOURCES}
  "${LEXILLA_SOURCE_DIR}/src/Lexilla.cxx"
  "${CMAKE_SOURCE_DIR}/src/lexers/bsl_lexer.cpp"
  "${CMAKE_SOURCE_DIR}/src/lexers/html_css_lexer.cpp")
target_include_directories(listopad_lexilla PUBLIC
  "${CMAKE_SOURCE_DIR}/include"
  "${LEXILLA_SOURCE_DIR}/include"
  "${SCINTILLA_SOURCE_DIR}/include"
  PRIVATE
  "${LEXILLA_SOURCE_DIR}/lexlib")
target_compile_definitions(listopad_lexilla PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(listopad_lexilla PRIVATE /W3 /utf-8)
