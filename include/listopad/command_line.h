#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace listopad {

enum class ShellRegistration { None, Register, Unregister };

struct ElevatedRestartRequest {
  std::uint32_t parent_process_id{0};
  std::wstring ready_event;
  std::string tab_id;
  std::filesystem::path target_path;
  std::string content_sha256;
};

struct CommandLine {
  std::vector<std::filesystem::path> files;
  std::optional<std::size_t> line;
  std::optional<std::size_t> column;
  std::optional<std::string> encoding;
  std::optional<ElevatedRestartRequest> elevated_restart;
  ShellRegistration shell_registration{ShellRegistration::None};
};

CommandLine parse_command_line(const std::vector<std::wstring>& arguments);
CommandLine current_command_line();

}  // namespace listopad
