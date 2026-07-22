#include "listopad/ipc_protocol.h"

#include "listopad/strings.h"

#include <windows.h>
#include <sddl.h>

#include <cstring>
#include <algorithm>
#include <limits>
#include <type_traits>

namespace listopad::ipc {
namespace {

class Writer {
 public:
  template <typename T> void pod(const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    const auto* first = reinterpret_cast<const std::byte*>(&value);
    data_.insert(data_.end(), first, first + sizeof(T));
  }
  void bytes(std::span<const std::byte> value) { data_.insert(data_.end(), value.begin(), value.end()); }
  void text(std::string_view value) {
    const auto size = static_cast<std::uint32_t>(value.size());
    pod(size); bytes(std::as_bytes(std::span(value.data(), value.size())));
  }
  std::vector<std::byte> finish() { return std::move(data_); }
 private:
  std::vector<std::byte> data_;
};

class Reader {
 public:
  explicit Reader(std::span<const std::byte> data) : data_(data) {}
  template <typename T> bool pod(T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    if (remaining() < sizeof(T)) return false;
    std::memcpy(&value, data_.data() + offset_, sizeof(T)); offset_ += sizeof(T); return true;
  }
  bool bytes(std::span<std::byte> value) {
    if (remaining() < value.size()) return false;
    std::memcpy(value.data(), data_.data() + offset_, value.size()); offset_ += value.size(); return true;
  }
  bool text(std::string& value) {
    std::uint32_t size = 0;
    if (!pod(size) || size > kMaxMetadata || remaining() < size) return false;
    value.assign(reinterpret_cast<const char*>(data_.data() + offset_), size); offset_ += size; return true;
  }
  [[nodiscard]] bool done() const { return offset_ == data_.size(); }
 private:
  [[nodiscard]] std::size_t remaining() const { return data_.size() - offset_; }
  std::span<const std::byte> data_; std::size_t offset_{0};
};

void write_fingerprint(Writer& writer, const FileFingerprint& value) {
  const std::uint8_t exists = value.exists ? 1 : 0;
  writer.pod(exists); writer.pod(value.volume_serial); writer.bytes(value.file_id);
  writer.pod(value.size); writer.pod(value.last_write);
}
bool read_fingerprint(Reader& reader, FileFingerprint& value) {
  std::uint8_t exists = 0;
  if (!reader.pod(exists) || !reader.pod(value.volume_serial) || !reader.bytes(value.file_id) ||
      !reader.pod(value.size) || !reader.pod(value.last_write)) return false;
  value.exists = exists != 0; return true;
}

bool write_exact(HANDLE handle, const void* data, std::size_t size) {
  const auto* bytes = static_cast<const std::byte*>(data);
  while (size > 0) {
    DWORD written = 0;
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size, 1024 * 1024));
    if (!WriteFile(handle, bytes, chunk, &written, nullptr) || written == 0) return false;
    bytes += written; size -= written;
  }
  return true;
}
bool read_exact(HANDLE handle, void* data, std::size_t size) {
  auto* bytes = static_cast<std::byte*>(data);
  while (size > 0) {
    DWORD received = 0;
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size, 1024 * 1024));
    if (!ReadFile(handle, bytes, chunk, &received, nullptr) || received == 0) return false;
    bytes += received; size -= received;
  }
  return true;
}

}  // namespace

std::vector<std::byte> encode(const OpenFilesRequest& request) {
  Writer writer;
  writer.pod(request.line); writer.pod(request.column); writer.text(request.encoding);
  writer.pod(static_cast<std::uint32_t>(request.files.size()));
  for (const auto& path : request.files) writer.text(wide_to_utf8(path.wstring()));
  return writer.finish();
}
std::vector<std::byte> encode(const SaveRequest& request) {
  Writer writer;
  writer.pod(request.request_id); writer.text(wide_to_utf8(request.path.wstring()));
  write_fingerprint(writer, request.expected); writer.pod(request.content_length);
  writer.bytes(request.content_hash); return writer.finish();
}
std::vector<std::byte> encode(const SaveResult& result) {
  Writer writer;
  writer.pod(result.request_id); writer.pod(result.status); writer.pod(result.win32_error);
  write_fingerprint(writer, result.fingerprint); return writer.finish();
}
bool decode(const std::span<const std::byte> payload, OpenFilesRequest& request) {
  Reader reader(payload); std::uint32_t count = 0;
  if (!reader.pod(request.line) || !reader.pod(request.column) || !reader.text(request.encoding) ||
      !reader.pod(count) || count > 4096) return false;
  request.files.clear(); request.files.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    std::string path; if (!reader.text(path)) return false; request.files.emplace_back(utf8_to_wide(path));
  }
  return reader.done();
}
bool decode(const std::span<const std::byte> payload, SaveRequest& request) {
  Reader reader(payload); std::string path;
  if (!reader.pod(request.request_id) || !reader.text(path) ||
      !read_fingerprint(reader, request.expected) || !reader.pod(request.content_length) ||
      !reader.bytes(request.content_hash)) return false;
  request.path = utf8_to_wide(path); return reader.done();
}
bool decode(const std::span<const std::byte> payload, SaveResult& result) {
  Reader reader(payload);
  return reader.pod(result.request_id) && reader.pod(result.status) &&
         reader.pod(result.win32_error) && read_fingerprint(reader, result.fingerprint) && reader.done();
}

bool write_frame(void* raw, const MessageType type, const std::span<const std::byte> payload) {
  if (payload.size() > std::numeric_limits<std::uint32_t>::max()) return false;
  FrameHeader header; header.type = type; header.payload_size = static_cast<std::uint32_t>(payload.size());
  const HANDLE handle = static_cast<HANDLE>(raw);
  return write_exact(handle, &header, sizeof(header)) &&
         (payload.empty() || write_exact(handle, payload.data(), payload.size()));
}
bool read_frame(void* raw, FrameHeader& header, std::vector<std::byte>& payload) {
  const HANDLE handle = static_cast<HANDLE>(raw);
  if (!read_exact(handle, &header, sizeof(header)) || header.magic != kMagic ||
      header.version != LISTOPAD_PROTOCOL_VERSION || header.payload_size > kMaxMetadata) return false;
  payload.resize(header.payload_size);
  return payload.empty() || read_exact(handle, payload.data(), payload.size());
}

std::wstring instance_pipe_name() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return L"\\\\.\\pipe\\ListopadPP.default";
  DWORD size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<std::byte> buffer(size);
  std::wstring sid_text = L"default";
  if (GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    wchar_t* sid = nullptr;
    if (ConvertSidToStringSidW(user->User.Sid, &sid)) { sid_text = sid; LocalFree(sid); }
  }
  CloseHandle(token);
  return L"\\\\.\\pipe\\ListopadPP." + sid_text;
}

}  // namespace listopad::ipc
