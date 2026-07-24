#include "tab_controller.h"

#include <catch2/catch_test_macros.hpp>

using listopad::app::DocumentSession;
using listopad::app::TabController;

TEST_CASE("tab controller owns the active document session") {
  TabController tabs;
  CHECK(tabs.active() == nullptr);
  CHECK(tabs.active_index() == -1);

  tabs.push_back(DocumentSession{});
  tabs.push_back(DocumentSession{});
  REQUIRE(tabs.size() == 2);
  CHECK(tabs.active_index() == 0);

  REQUIRE(tabs.activate(1));
  CHECK(tabs.active() == &tabs[1]);
  CHECK_FALSE(tabs.activate(2));
  CHECK(tabs.active_index() == 1);
}

TEST_CASE("tab controller keeps selection valid when sessions are removed") {
  TabController tabs;
  tabs.push_back(DocumentSession{});
  tabs.push_back(DocumentSession{});
  tabs.push_back(DocumentSession{});
  REQUIRE(tabs.activate(2));

  tabs.erase(tabs.begin());
  CHECK(tabs.size() == 2);
  CHECK(tabs.active_index() == 1);

  tabs.erase(tabs.begin() + 1);
  CHECK(tabs.size() == 1);
  CHECK(tabs.active_index() == 0);

  tabs.pop_back();
  CHECK(tabs.empty());
  CHECK(tabs.active() == nullptr);
  CHECK(tabs.active_index() == -1);
}
