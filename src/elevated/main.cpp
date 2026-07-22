#include "listopad/file_io.h"
#include "listopad/ipc_protocol.h"
#include "listopad/signature.h"
#include "listopad/strings.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace {

bool read_exact(HANDLE handle, std::span<std::byte> data) {
  while (!data.empty()) {
    DWORD received = 0;
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(data.size(), 1024 * 1024));
    if (!ReadFile(handle, data.data(), chunk, &received, nullptr) || received == 0) return false;
    data = data.subspan(received);
  }
  return true;
}

bool parse_arguments(std::wstring& pipe_name, DWORD& parent_pid) {
  int count = 0;
  wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
  if (!arguments) return false;
  for (int i = 1; i < count; ++i) {
    if (std::wstring_view(arguments[i]) == L"--pipe" && i + 1 < count) pipe_name = arguments[++i];
    else if (std::wstring_view(arguments[i]) == L"--parent" && i + 1 < count) {
      const std::wstring_view value(arguments[++i]);
      parent_pid = static_cast<DWORD>(_wcstoui64(value.data(), nullptr, 10));
    }
  }
  LocalFree(arguments);
  const std::wstring prefix = L"\\\\.\\pipe\\ListopadPP.Elevated." + std::to_wstring(parent_pid) + L".";
  return parent_pid != 0 && pipe_name.starts_with(prefix) && pipe_name.size() <= 240;
}

std::filesystem::path process_image(HANDLE process) {
  std::wstring path(32768, L'\0');
  DWORD size = static_cast<DWORD>(path.size());
  if (!QueryFullProcessImageNameW(process, 0, path.data(), &size)) return {};
  path.resize(size);
  return listopad::canonical_path(path);
}

std::filesystem::path current_image() {
  std::wstring path(32768, L'\0');
  const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (!size || size == path.size()) return {};
  path.resize(size);
  return listopad::canonical_path(path);
}

bool trusted_parent(HANDLE process) {
  const auto parent = process_image(process);
  const auto helper = current_image();
  if (parent.empty() || helper.empty() ||
      listopad::lowercase(parent.filename().wstring()) != L"listopadpp.exe" ||
      listopad::lowercase(parent.parent_path().wstring()) !=
          listopad::lowercase(helper.parent_path().wstring())) return false;
#ifdef _DEBUG
  return true;
#else
  return listopad::has_matching_authenticode_signer(parent, helper);
#endif
}

bool has_reparse_component(const std::filesystem::path& path) {
  std::filesystem::path current = path.root_path();
  for (const auto& component : path.relative_path()) {
    current /= component;
    const DWORD attributes = GetFileAttributesW(current.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      if (current == path && GetLastError() == ERROR_FILE_NOT_FOUND) return false;
      return true;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return true;
  }
  return false;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, wchar_t*, int) {
  std::wstring pipe_name; DWORD parent_pid = 0;
  if (!parse_arguments(pipe_name, parent_pid)) return ERROR_INVALID_PARAMETER;
  const HANDLE parent = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parent_pid);
  if (!parent) return ERROR_ACCESS_DENIED;
  if (!trusted_parent(parent)) { CloseHandle(parent); return ERROR_INVALID_IMAGE_HASH; }
  const HANDLE pipe = CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE,
                                  0, nullptr, OPEN_EXISTING, 0, nullptr);
  if (pipe == INVALID_HANDLE_VALUE) { CloseHandle(parent); return GetLastError(); }
  ULONG server_pid = 0;
  if (!GetNamedPipeServerProcessId(pipe, &server_pid) || server_pid != parent_pid) {
    CloseHandle(pipe); CloseHandle(parent); return ERROR_ACCESS_DENIED;
  }

  for (;;) {
    if (WaitForSingleObject(parent, 0) == WAIT_OBJECT_0) break;
    listopad::ipc::FrameHeader header{}; std::vector<std::byte> payload;
    if (!listopad::ipc::read_frame(pipe, header, payload) ||
        header.type != listopad::ipc::MessageType::SaveRequest) break;
    listopad::ipc::SaveRequest request;
    if (!listopad::ipc::decode(payload, request) || request.content_length > (512ull << 20)) break;
    std::vector<std::byte> content(static_cast<std::size_t>(request.content_length));
    if (!read_exact(pipe, content) || listopad::sha256(content) != request.content_hash) break;
    const auto target = listopad::canonical_path(request.path);
    listopad::SaveFileResult saved;
    if (!target.is_absolute() || target.filename().empty() ||
        listopad::lowercase(target.wstring()) != listopad::lowercase(request.path.wstring()) ||
        has_reparse_component(target)) {
      saved.status = listopad::SaveStatus::Failed;
      saved.error = ERROR_REPARSE_TAG_INVALID;
    } else {
      saved = listopad::atomic_save(target, content, request.expected, false);
    }
    listopad::ipc::SaveResult response;
    response.request_id = request.request_id;
    response.status = static_cast<std::uint32_t>(saved.status);
    response.win32_error = saved.error;
    response.fingerprint = saved.fingerprint;
    const auto response_payload = listopad::ipc::encode(response);
    if (!listopad::ipc::write_frame(pipe, listopad::ipc::MessageType::SaveResult, response_payload)) break;
  }
  CloseHandle(pipe); CloseHandle(parent); return 0;
}
