#include "listopad/ipc_protocol.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("open request IPC round trips Unicode and location") {
  listopad::ipc::OpenFilesRequest source;
  source.files = {L"C:\\данные\\тест.json", L"\\\\server\\share\\a b.txt"};
  source.line = 10; source.column = 3; source.encoding = "utf-8";
  const auto payload = listopad::ipc::encode(source);
  listopad::ipc::OpenFilesRequest decoded;
  REQUIRE(listopad::ipc::decode(payload, decoded));
  CHECK(decoded.files == source.files);
  CHECK(decoded.line == 10);
  CHECK(decoded.column == 3);
  CHECK(decoded.encoding == "utf-8");
}

TEST_CASE("save request rejects truncated metadata") {
  listopad::ipc::SaveRequest source;
  source.request_id = 7; source.path = L"C:\\x.txt"; source.content_length = 4;
  auto payload = listopad::ipc::encode(source);
  payload.pop_back();
  listopad::ipc::SaveRequest decoded;
  CHECK_FALSE(listopad::ipc::decode(payload, decoded));
}

