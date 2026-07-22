#pragma once

#include "listopad/file_io.h"
#include "listopad/version.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace listopad::ipc {

constexpr std::uint32_t kMagic = 0x5050494c;  // LIPP
constexpr std::uint32_t kMaxMetadata = 1024 * 1024;

enum class MessageType : std::uint32_t {
  OpenFiles = 1,
  SaveRequest = 2,
  SaveResult = 3,
  Ping = 4,
};

#pragma pack(push, 1)
struct FrameHeader {
  std::uint32_t magic{kMagic};
  std::uint32_t version{LISTOPAD_PROTOCOL_VERSION};
  MessageType type{MessageType::Ping};
  std::uint32_t payload_size{0};
};
#pragma pack(pop)

struct OpenFilesRequest {
  std::vector<std::filesystem::path> files;
  std::uint64_t line{0};
  std::uint64_t column{0};
  std::string encoding;
};

struct SaveRequest {
  std::uint64_t request_id{0};
  std::filesystem::path path;
  FileFingerprint expected;
  std::uint64_t content_length{0};
  std::array<std::byte, 32> content_hash{};
};

struct SaveResult {
  std::uint64_t request_id{0};
  std::uint32_t status{0};
  std::uint32_t win32_error{0};
  FileFingerprint fingerprint{};
};

std::vector<std::byte> encode(const OpenFilesRequest& request);
std::vector<std::byte> encode(const SaveRequest& request);
std::vector<std::byte> encode(const SaveResult& result);
bool decode(std::span<const std::byte> payload, OpenFilesRequest& request);
bool decode(std::span<const std::byte> payload, SaveRequest& request);
bool decode(std::span<const std::byte> payload, SaveResult& result);
bool write_frame(void* handle, MessageType type, std::span<const std::byte> payload);
bool read_frame(void* handle, FrameHeader& header, std::vector<std::byte>& payload);
std::wstring instance_pipe_name();

}  // namespace listopad::ipc
