#include "single_instance.h"

#include <array>
#include <string_view>
#include <utility>

namespace listopad::app {
namespace {

constexpr std::wstring_view kInstanceIdEnvironment = L"LISTOPAD_INSTANCE_ID";

[[nodiscard]] bool is_instance_id_character(const wchar_t value) noexcept {
  return (value >= L'a' && value <= L'z') ||
         (value >= L'A' && value <= L'Z') ||
         (value >= L'0' && value <= L'9') ||
         value == L'-' || value == L'_';
}

[[nodiscard]] std::wstring process_instance_pipe_name() {
  std::wstring name = ipc::instance_pipe_name();
  std::array<wchar_t, 65> buffer{};
  const DWORD length = GetEnvironmentVariableW(
      kInstanceIdEnvironment.data(), buffer.data(),
      static_cast<DWORD>(buffer.size()));
  if (length == 0 || length >= buffer.size()) return name;

  std::wstring instance_id;
  instance_id.reserve(length);
  for (DWORD index = 0; index < length; ++index) {
    if (is_instance_id_character(buffer[index])) {
      instance_id.push_back(buffer[index]);
    }
  }
  if (!instance_id.empty()) name += L"." + instance_id;
  return name;
}

}  // namespace

SingleInstance::SingleInstance() : pipe_name_(process_instance_pipe_name()) {
  const std::wstring name = L"Local\\ListopadPP.Instance." + pipe_name_.substr(18);
  mutex_ = CreateMutexW(nullptr, FALSE, name.c_str());
  primary_ = mutex_ && GetLastError() != ERROR_ALREADY_EXISTS;
}

SingleInstance::~SingleInstance() {
  if (server_.joinable()) {
    server_.request_stop();
    const HANDLE pipe = CreateFileW(pipe_name_.c_str(), GENERIC_WRITE,
                                    0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe != INVALID_HANDLE_VALUE) {
      ipc::write_frame(pipe, ipc::MessageType::Ping, {});
      CloseHandle(pipe);
    }
  }
  if (mutex_) CloseHandle(mutex_);
}

bool SingleInstance::primary() const noexcept { return primary_; }

bool SingleInstance::send(const ipc::OpenFilesRequest& request) const {
  if (!WaitNamedPipeW(pipe_name_.c_str(), 1500)) return false;
  const HANDLE pipe = CreateFileW(pipe_name_.c_str(), GENERIC_WRITE,
                                  0, nullptr, OPEN_EXISTING, 0, nullptr);
  if (pipe == INVALID_HANDLE_VALUE) return false;
  const auto payload = ipc::encode(request);
  const bool result = ipc::write_frame(pipe, ipc::MessageType::OpenFiles, payload);
  CloseHandle(pipe);
  return result;
}

void SingleInstance::start(Callback callback) {
  if (!primary_ || server_.joinable()) return;
  server_ = std::jthread([callback = std::move(callback),
                          pipe_name = pipe_name_](const std::stop_token stop) {
    while (!stop.stop_requested()) {
      const HANDLE pipe = CreateNamedPipeW(
          pipe_name.c_str(), PIPE_ACCESS_INBOUND,
          PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
          1, 4096, ipc::kMaxMetadata + sizeof(ipc::FrameHeader), 0, nullptr);
      if (pipe == INVALID_HANDLE_VALUE) return;
      const bool connected = ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
      if (connected) {
        ipc::FrameHeader header{};
        std::vector<std::byte> payload;
        if (ipc::read_frame(pipe, header, payload) && header.type == ipc::MessageType::OpenFiles) {
          ipc::OpenFilesRequest request;
          if (ipc::decode(payload, request)) callback(std::move(request));
        }
      }
      DisconnectNamedPipe(pipe);
      CloseHandle(pipe);
    }
  });
}

}  // namespace listopad::app
