#pragma once

#include <string_view>

namespace listopad {
bool register_classic_context_menu(std::string_view ui_language);
bool unregister_classic_context_menu();
}  // namespace listopad
