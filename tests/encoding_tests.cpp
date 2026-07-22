#include "listopad/encoding.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("UTF-8 BOM round trips") {
  const std::vector<std::byte> bytes{std::byte{0xef}, std::byte{0xbb}, std::byte{0xbf},
                                     std::byte{'a'}, std::byte{'\r'}, std::byte{'\n'}};
  const auto decoded = listopad::decode_text(bytes);
  CHECK(decoded.utf8 == "a\r\n");
  CHECK(decoded.encoding.kind == listopad::EncodingKind::Utf8);
  CHECK(decoded.encoding.bom);
  CHECK(decoded.eol == listopad::EolMode::CrLf);
  const auto encoded = listopad::encode_text(decoded.utf8, decoded.encoding);
  REQUIRE(encoded.ok);
  CHECK(encoded.bytes == bytes);
}

TEST_CASE("forced UTF encodings consume a matching BOM") {
  const std::vector<std::byte> utf8{
      std::byte{0xef}, std::byte{0xbb}, std::byte{0xbf}, std::byte{'x'}};
  const listopad::Encoding forced{listopad::EncodingKind::Utf8, 65001, false};
  const listopad::DecodedText decoded = listopad::decode_text(utf8, &forced);
  REQUIRE(decoded.utf8 == "x");
  REQUIRE(decoded.encoding.bom);
}

TEST_CASE("UTF-16 LE round trips Cyrillic text") {
  const auto encoding = listopad::encoding_from_name("utf-16le");
  const auto encoded = listopad::encode_text("Привет", encoding);
  REQUIRE(encoded.ok);
  CHECK(listopad::decode_text(encoded.bytes).utf8 == "Привет");
}

TEST_CASE("mixed line endings are detected") {
  CHECK(listopad::detect_eol("a\r\nb\nc") == listopad::EolMode::Mixed);
}
