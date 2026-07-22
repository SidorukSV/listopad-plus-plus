#pragma once

#include <string_view>

namespace listopad {
bool register_classic_context_menu(std::string_view ui_language);
bool unregister_classic_context_menu();

// Publishes this executable's location so the shell extension can find it when
// the two no longer share a directory. Best effort: failure only degrades the
// extension to its directory-relative fallback.
void record_executable_location();
}  // namespace listopad
