#pragma once

#include "listopad/ui_layout.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace listopad {

enum class LinuxCoreArchitecture {
  Unknown,
  X86,
  X86_64,
  Arm,
  AArch64,
  RiscV,
};

enum class LinuxCoreError {
  None,
  OpenFailed,
  NotCoreDump,
  UnsupportedFormat,
  InvalidData,
  MetadataTooLarge,
  DecompressionFailed,
  Cancelled,
};

struct LinuxCoreSourceMetadata {
  std::string process;
  std::string boot_id;
  std::uint32_t uid{0};
  std::uint32_t pid{0};
  std::uint64_t timestamp_microseconds{0};
  bool has_uid{false};
  bool has_pid{false};
  bool has_timestamp{false};
};

struct LinuxCoreMapping {
  std::uint64_t start{0};
  std::uint64_t end{0};
  std::uint64_t file_offset{0};
  std::string path;
};

struct LinuxCoreDocument {
  LinuxCoreArchitecture architecture{LinuxCoreArchitecture::Unknown};
  LinuxCoreSourceMetadata source;
  std::vector<LinuxCoreMapping> mappings;
  std::uint32_t signal{0};
  std::size_t thread_count{0};
  std::size_t program_header_count{0};
  std::size_t load_segment_count{0};
  std::uint8_t elf_class{0};
  bool little_endian{true};
  bool compressed{false};
  bool mappings_truncated{false};
};

struct LinuxCoreUiState {
  std::size_t selected_mapping{0};
};

struct LinuxCoreReadResult {
  LinuxCoreError error{LinuxCoreError::None};
  unsigned long system_error{0};
  LinuxCoreDocument document;

  [[nodiscard]] bool ok() const noexcept {
    return error == LinuxCoreError::None;
  }
};

[[nodiscard]] bool looks_like_linux_core(
    std::span<const std::byte> bytes) noexcept;
[[nodiscard]] bool looks_like_linux_core_file(
    const std::filesystem::path& path) noexcept;
[[nodiscard]] LinuxCoreReadResult parse_linux_core(
    std::span<const std::byte> bytes, std::wstring_view source_name = {},
    std::stop_token stop = {});
[[nodiscard]] LinuxCoreReadResult read_linux_core_file(
    const std::filesystem::path& path, std::stop_token stop = {});

[[nodiscard]] std::string_view linux_core_architecture_name(
    LinuxCoreArchitecture architecture) noexcept;
[[nodiscard]] std::string_view linux_signal_name(
    std::uint32_t signal) noexcept;

struct LinuxCoreLayout {
  LayoutRect summary;
  LayoutRect mappings;
};

[[nodiscard]] LinuxCoreLayout calculate_linux_core_layout(
    int width, int height, int dpi) noexcept;

}  // namespace listopad
