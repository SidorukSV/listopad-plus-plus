#include "document_session.h"

#include <catch2/catch_test_macros.hpp>

using namespace listopad::app;

namespace {

HWND fake_window(const INT_PTR value) {
  return reinterpret_cast<HWND>(value);
}

}  // namespace

TEST_CASE("document session preserves the text surface across projections") {
  DocumentSession session;
  session.view = fake_window(1);
  session.map = fake_window(2);

  REQUIRE(session.preserve_text_surface());
  CHECK(session.view == nullptr);
  CHECK(session.map == nullptr);
  CHECK(session.has_preserved_text_surface());
  CHECK(session.owns_view(fake_window(1)));

  session.view = fake_window(3);
  CHECK_FALSE(session.restore_text_surface());
  session.view = nullptr;
  CHECK(session.restore_text_surface());
  CHECK(session.view == fake_window(1));
  CHECK(session.map == fake_window(2));
  CHECK_FALSE(session.has_preserved_text_surface());
}

TEST_CASE("document session refuses to overwrite a preserved text surface") {
  DocumentSession session;
  session.view = fake_window(1);
  REQUIRE(session.preserve_text_surface());

  session.view = fake_window(3);
  CHECK_FALSE(session.preserve_text_surface());
  CHECK(session.preserved_text_view() == fake_window(1));
}

TEST_CASE("document session owns transient editor state") {
  DocumentSession session;
  session.snippet_fields.push_back({.start = 4, .length = 2});
  session.snippet_index = 1;

  session.clear_transient_editor_state();
  CHECK(session.snippet_fields.empty());
  CHECK(session.snippet_index == 0);
}
