#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace listopad {

struct FileFingerprint {
  bool exists{false};
  std::uint64_t volume_serial{0};
  std::array<std::byte, 16> file_id{};
  std::uint64_t size{0};
  std::uint64_t last_write{0};

  friend bool operator==(const FileFingerprint&, const FileFingerprint&) = default;
};

struct ReadFileResult {
  bool ok{false};
  std::vector<std::byte> bytes;
  FileFingerprint fingerprint;
  unsigned long error{0};
};

enum class SaveStatus { Saved, Conflict, AccessDenied, Failed };

struct SaveFileResult {
  SaveStatus status{SaveStatus::Failed};
  FileFingerprint fingerprint;
  unsigned long error{0};
};

std::filesystem::path canonical_path(const std::filesystem::path& path);
FileFingerprint fingerprint_file(const std::filesystem::path& path);
ReadFileResult read_file(const std::filesystem::path& path);
SaveFileResult atomic_save(const std::filesystem::path& path,
                           std::span<const std::byte> bytes,
                           const std::optional<FileFingerprint>& expected,
                           bool overwrite_conflict);
std::array<std::byte, 32> sha256(std::span<const std::byte> bytes);
std::string hex_encode(std::span<const std::byte> bytes);

class MappedFile final {
 public:
  MappedFile() = default;
  ~MappedFile();
  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;
  MappedFile(MappedFile&& other) noexcept;
  MappedFile& operator=(MappedFile&& other) noexcept;

  bool open(const std::filesystem::path& path);
  void close();
  [[nodiscard]] const std::byte* data() const noexcept;
  [[nodiscard]] std::uint64_t size() const noexcept;
  [[nodiscard]] unsigned long error() const noexcept;

 private:
  void* file_{reinterpret_cast<void*>(-1)};
  void* mapping_{nullptr};
  const std::byte* view_{nullptr};
  std::uint64_t size_{0};
  unsigned long error_{0};
};

}  // namespace listopad

