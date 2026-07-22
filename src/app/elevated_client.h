#pragma once

#include "listopad/file_io.h"

#include <windows.h>

#include <filesystem>
#include <span>

namespace listopad::app {

class ElevatedClient final {
 public:
  ElevatedClient() = default;
  ~ElevatedClient();
  ElevatedClient(const ElevatedClient&) = delete;
  ElevatedClient& operator=(const ElevatedClient&) = delete;

  SaveFileResult save(const std::filesystem::path& path,
                      std::span<const std::byte> bytes,
                      const FileFingerprint& expected);
  void close();

 private:
  bool connect();
  HANDLE pipe_{INVALID_HANDLE_VALUE};
  HANDLE process_{nullptr};
  std::uint64_t request_id_{0};
};

}  // namespace listopad::app

