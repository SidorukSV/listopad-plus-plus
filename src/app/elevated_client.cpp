#include "elevated_client.h"

#include "listopad/ipc_protocol.h"
#include "listopad/signature.h"
#include "listopad/strings.h"

#include <bcrypt.h>
#include <sddl.h>
#include <shellapi.h>
#include <array>

namespace listopad::app {
namespace {

bool write_exact(HANDLE handle, std::span<const std::byte> data) {
  while (!data.empty()) {
    DWORD written = 0;
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(data.size(), 1024 * 1024));
    if (!WriteFile(handle, data.data(), chunk, &written, nullptr) || written == 0) return false;
    data = data.subspan(written);
  }
  return true;
}

std::filesystem::path module_directory() {
  std::wstring path(32768, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  path.resize(length);
  return std::filesystem::path(path).parent_path();
}

bool trusted_pair(const std::filesystem::path& editor,
                  const std::filesystem::path& helper) {
#ifdef _DEBUG
  return std::filesystem::exists(editor) && std::filesystem::exists(helper);
#else
  return has_matching_authenticode_signer(editor, helper);
#endif
}

std::wstring random_nonce() {
  std::array<std::byte, 16> bytes{};
  if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(bytes.data()),
                      static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return {};
  return utf8_to_wide(hex_encode(bytes));
}

}  // namespace

ElevatedClient::~ElevatedClient() { close(); }

void ElevatedClient::close() {
  if (pipe_ != INVALID_HANDLE_VALUE) { FlushFileBuffers(pipe_); DisconnectNamedPipe(pipe_); CloseHandle(pipe_); }
  if (process_) CloseHandle(process_);
  pipe_ = INVALID_HANDLE_VALUE; process_ = nullptr;
}

bool ElevatedClient::connect() {
  if (pipe_ != INVALID_HANDLE_VALUE) return true;
  const auto directory = module_directory();
  const auto editor = directory / L"ListopadPP.exe";
  const auto helper = directory / L"ListopadElevated.exe";
  if (!trusted_pair(editor, helper)) { SetLastError(ERROR_INVALID_IMAGE_HASH); return false; }
  const std::wstring pipe_name = L"\\\\.\\pipe\\ListopadPP.Elevated." +
                                 std::to_wstring(GetCurrentProcessId()) + L"." + random_nonce();

  PSECURITY_DESCRIPTOR descriptor = nullptr;
  ConvertStringSecurityDescriptorToSecurityDescriptorW(
      L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;OW)", SDDL_REVISION_1, &descriptor, nullptr);
  SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
  pipe_ = CreateNamedPipeW(pipe_name.c_str(), PIPE_ACCESS_DUPLEX,
                           PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT |
                               PIPE_REJECT_REMOTE_CLIENTS,
                           1, ipc::kMaxMetadata, ipc::kMaxMetadata, 0, &security);
  if (descriptor) LocalFree(descriptor);
  if (pipe_ == INVALID_HANDLE_VALUE) return false;

  const std::wstring parameters = L"--pipe " + quote_command_line_argument(pipe_name) +
                                  L" --parent " + std::to_wstring(GetCurrentProcessId());
  SHELLEXECUTEINFOW execute{sizeof(execute)};
  execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
  execute.lpVerb = L"runas"; execute.lpFile = helper.c_str();
  execute.lpParameters = parameters.c_str(); execute.nShow = SW_HIDE;
  if (!ShellExecuteExW(&execute)) { close(); return false; }
  process_ = execute.hProcess;
  bool connected = false;
  const ULONGLONG deadline = GetTickCount64() + 15'000;
  while (GetTickCount64() < deadline) {
    if (ConnectNamedPipe(pipe_, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
      connected = true;
      break;
    }
    const DWORD error = GetLastError();
    if (error != ERROR_PIPE_LISTENING && error != ERROR_NO_DATA) break;
    if (WaitForSingleObject(process_, 0) == WAIT_OBJECT_0) break;
    Sleep(10);
  }
  DWORD blocking_mode = PIPE_READMODE_BYTE | PIPE_WAIT;
  if (connected) connected = SetNamedPipeHandleState(pipe_, &blocking_mode, nullptr, nullptr) != FALSE;
  ULONG client_pid = 0;
  if (!connected || !GetNamedPipeClientProcessId(pipe_, &client_pid) ||
      client_pid != GetProcessId(process_)) { close(); return false; }
  return true;
}

SaveFileResult ElevatedClient::save(const std::filesystem::path& path,
                                    const std::span<const std::byte> bytes,
                                    const FileFingerprint& expected) {
  SaveFileResult result;
  if (!connect()) { result.error = GetLastError(); result.status = SaveStatus::AccessDenied; return result; }
  ipc::SaveRequest request;
  request.request_id = ++request_id_; request.path = canonical_path(path);
  request.expected = expected; request.content_length = bytes.size();
  request.content_hash = sha256(bytes);
  const auto metadata = ipc::encode(request);
  if (!ipc::write_frame(pipe_, ipc::MessageType::SaveRequest, metadata) || !write_exact(pipe_, bytes)) {
    result.error = GetLastError(); close(); return result;
  }
  ipc::FrameHeader header{}; std::vector<std::byte> payload; ipc::SaveResult response;
  if (!ipc::read_frame(pipe_, header, payload) || header.type != ipc::MessageType::SaveResult ||
      !ipc::decode(payload, response) || response.request_id != request.request_id) {
    result.error = ERROR_INVALID_DATA; close(); return result;
  }
  result.status = static_cast<SaveStatus>(response.status);
  result.error = response.win32_error; result.fingerprint = response.fingerprint;
  return result;
}

}  // namespace listopad::app
