#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace listopad {

enum class ShellRegistration { None, Register, Unregister };

struct CommandLine {
  std::vector<std::filesystem::path> files;
  std::optional<std::size_t> line;
  std::optional<std::size_t> column;
  std::optional<std::string> encoding;
  ShellRegistration shell_registration{ShellRegistration::None};
};

CommandLine parse_command_line(const std::vector<std::wstring>& arguments);
CommandLine current_command_line();

}  // namespace listopad

