#include "listopad/file_io.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <system_error>

namespace listopad {
namespace {

HANDLE as_handle(void* value) { return static_cast<HANDLE>(value); }

FileFingerprint fingerprint_handle(const HANDLE handle) {
  FileFingerprint result;
  FILE_ID_INFO id{};
  FILE_STANDARD_INFO standard{};
  FILE_BASIC_INFO basic{};
  if (!GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id)) ||
      !GetFileInformationByHandleEx(handle, FileStandardInfo, &standard, sizeof(standard)) ||
      !GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic))) {
    return result;
  }
  result.exists = true;
  result.volume_serial = id.VolumeSerialNumber;
  std::memcpy(result.file_id.data(), id.FileId.Identifier, result.file_id.size());
  result.size = static_cast<std::uint64_t>(standard.EndOfFile.QuadPart);
  result.last_write = static_cast<std::uint64_t>(basic.LastWriteTime.QuadPart);
  return result;
}

bool write_all(const HANDLE handle, std::span<const std::byte> bytes) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 8 * 1024 * 1024));
    DWORD written = 0;
    if (!WriteFile(handle, bytes.data() + offset, chunk, &written, nullptr) || written == 0) return false;
    offset += written;
  }
  return true;
}

std::filesystem::path temporary_sibling(const std::filesystem::path& path) {
  static std::atomic_uint64_t counter{0};
  return path.parent_path() /
         (L"." + path.filename().wstring() + L".listopad." +
          std::to_wstring(GetCurrentProcessId()) + L"." +
          std::to_wstring(++counter) + L".tmp");
}

}  // namespace

std::filesystem::path canonical_path(const std::filesystem::path& path) {
  std::error_code error;
  auto absolute = std::filesystem::absolute(path, error);
  if (error) absolute = path;
  const auto canonical = std::filesystem::weakly_canonical(absolute, error);
  return error ? absolute.lexically_normal() : canonical;
}

FileFingerprint fingerprint_file(const std::filesystem::path& path) {
  const HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (file == INVALID_HANDLE_VALUE) return {};
  const FileFingerprint result = fingerprint_handle(file);
  CloseHandle(file);
  return result;
}

ReadFileResult read_file(const std::filesystem::path& path) {
  ReadFileResult result;
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    result.error = GetLastError();
    return result;
  }
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
      static_cast<unsigned long long>(size.QuadPart) > SIZE_MAX) {
    result.error = GetLastError();
    CloseHandle(file);
    return result;
  }
  result.bytes.resize(static_cast<std::size_t>(size.QuadPart));
  std::size_t offset = 0;
  while (offset < result.bytes.size()) {
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(result.bytes.size() - offset, 8 * 1024 * 1024));
    DWORD read = 0;
    if (!ReadFile(file, result.bytes.data() + offset, chunk, &read, nullptr)) {
      result.error = GetLastError();
      CloseHandle(file);
      result.bytes.clear();
      return result;
    }
    if (read == 0) break;
    offset += read;
  }
  result.bytes.resize(offset);
  result.fingerprint = fingerprint_handle(file);
  result.ok = true;
  CloseHandle(file);
  return result;
}

SaveFileResult atomic_save(const std::filesystem::path& path,
                           const std::span<const std::byte> bytes,
                           const std::optional<FileFingerprint>& expected,
                           const bool overwrite_conflict) {
  SaveFileResult result;
  // Explicit overwrite accepts the version present when saving starts, but it
  // must not also accept a second, later change while the temporary file is
  // being written.
  const FileFingerprint initial = fingerprint_file(path);
  const FileFingerprint guarded = overwrite_conflict ? initial : expected.value_or(initial);
  if (expected && !overwrite_conflict && initial != *expected) {
    result.status = SaveStatus::Conflict;
    return result;
  }

  std::filesystem::path temporary;
  HANDLE output = INVALID_HANDLE_VALUE;
  for (int attempt = 0; attempt < 16 && output == INVALID_HANDLE_VALUE; ++attempt) {
    temporary = temporary_sibling(path);
    output = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                         FILE_ATTRIBUTE_TEMPORARY, nullptr);
  }
  if (output == INVALID_HANDLE_VALUE) {
    result.error = GetLastError();
    result.status = result.error == ERROR_ACCESS_DENIED ? SaveStatus::AccessDenied : SaveStatus::Failed;
    return result;
  }

  if (!write_all(output, bytes) || !FlushFileBuffers(output)) {
    result.error = GetLastError();
    CloseHandle(output);
    DeleteFileW(temporary.c_str());
    result.status = result.error == ERROR_ACCESS_DENIED ? SaveStatus::AccessDenied : SaveStatus::Failed;
    return result;
  }
  CloseHandle(output);

  if (fingerprint_file(path) != guarded) {
    DeleteFileW(temporary.c_str());
    result.status = SaveStatus::Conflict;
    return result;
  }

  bool moved = false;
  if (fingerprint_file(path).exists) {
    moved = ReplaceFileW(path.c_str(), temporary.c_str(), nullptr,
                         REPLACEFILE_WRITE_THROUGH, nullptr, nullptr) != FALSE;
    if (!moved) {
      moved = MoveFileExW(temporary.c_str(), path.c_str(),
                          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    }
  } else {
    moved = MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
  }
  if (!moved) {
    result.error = GetLastError();
    DeleteFileW(temporary.c_str());
    result.status = result.error == ERROR_ACCESS_DENIED || result.error == ERROR_PRIVILEGE_NOT_HELD
                        ? SaveStatus::AccessDenied
                        : SaveStatus::Failed;
    return result;
  }
  result.status = SaveStatus::Saved;
  result.fingerprint = fingerprint_file(path);
  return result;
}

std::array<std::byte, 32> sha256(const std::span<const std::byte> bytes) {
  std::array<std::byte, 32> result{};
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  DWORD object_size = 0, received = 0;
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return result;
  if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                        reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &received, 0) < 0) {
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return result;
  }
  std::vector<unsigned char> object(object_size);
  if (BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) >= 0) {
    NTSTATUS status = 0;
    std::size_t offset = 0;
    while (status >= 0 && offset < bytes.size()) {
      const auto remaining = bytes.size() - offset;
      const auto chunk = static_cast<ULONG>((std::min)(
          remaining, static_cast<std::size_t>((std::numeric_limits<ULONG>::max)())));
      status = BCryptHashData(hash,
                              reinterpret_cast<PUCHAR>(
                                  const_cast<std::byte*>(bytes.data() + offset)),
                              chunk, 0);
      offset += chunk;
    }
    if (status >= 0) {
      BCryptFinishHash(hash, reinterpret_cast<PUCHAR>(result.data()),
                       static_cast<ULONG>(result.size()), 0);
    }
    BCryptDestroyHash(hash);
  }
  BCryptCloseAlgorithmProvider(algorithm, 0);
  return result;
}

std::string hex_encode(const std::span<const std::byte> bytes) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string result(bytes.size() * 2, '\0');
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    const unsigned value = std::to_integer<unsigned>(bytes[i]);
    result[i * 2] = digits[value >> 4];
    result[i * 2 + 1] = digits[value & 15];
  }
  return result;
}

MappedFile::~MappedFile() { close(); }
MappedFile::MappedFile(MappedFile&& other) noexcept { *this = std::move(other); }
MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
  if (this != &other) {
    close();
    file_ = other.file_; mapping_ = other.mapping_; view_ = other.view_;
    size_ = other.size_; error_ = other.error_;
    other.file_ = reinterpret_cast<void*>(-1); other.mapping_ = nullptr;
    other.view_ = nullptr; other.size_ = 0; other.error_ = 0;
  }
  return *this;
}

bool MappedFile::open(const std::filesystem::path& path) {
  close();
  file_ = CreateFileW(path.c_str(), GENERIC_READ,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (as_handle(file_) == INVALID_HANDLE_VALUE) { error_ = GetLastError(); return false; }
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(as_handle(file_), &size) || size.QuadPart < 0) {
    error_ = GetLastError(); close(); return false;
  }
  size_ = static_cast<std::uint64_t>(size.QuadPart);
  if (size_ == 0) return true;
  mapping_ = CreateFileMappingW(as_handle(file_), nullptr, PAGE_READONLY, 0, 0, nullptr);
  if (!mapping_) { error_ = GetLastError(); close(); return false; }
  view_ = static_cast<const std::byte*>(MapViewOfFile(as_handle(mapping_), FILE_MAP_READ, 0, 0, 0));
  if (!view_) { error_ = GetLastError(); close(); return false; }
  return true;
}

void MappedFile::close() {
  if (view_) UnmapViewOfFile(view_);
  if (mapping_) CloseHandle(as_handle(mapping_));
  if (as_handle(file_) != INVALID_HANDLE_VALUE) CloseHandle(as_handle(file_));
  file_ = reinterpret_cast<void*>(-1); mapping_ = nullptr; view_ = nullptr; size_ = 0;
}
const std::byte* MappedFile::data() const noexcept { return view_; }
std::uint64_t MappedFile::size() const noexcept { return size_; }
unsigned long MappedFile::error() const noexcept { return error_; }

}  // namespace listopad
