#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

namespace listopad {
bool register_classic_context_menu(std::string_view ui_language);
bool unregister_classic_context_menu();

// Registry location under HKEY_CURRENT_USER holding the editor's path. Tests
// override it so they never disturb the value the shell extension relies on.
inline constexpr wchar_t kExecutableLocationKey[] = L"Software\\ListopadPP";

// Publishes this executable's location so the shell extension can find it when
// the two no longer share a directory. Best effort: failure only degrades the
// extension to its directory-relative fallback.
void record_executable_location(const wchar_t* subkey = kExecutableLocationKey);

// Reads back what record_executable_location stored. Returns nothing when the
// value is missing, empty, or names a file that no longer exists, so callers
// can fall back instead of launching a stale path.
std::optional<std::filesystem::path> recorded_executable_location(
    const wchar_t* subkey = kExecutableLocationKey);
}  // namespace listopad
