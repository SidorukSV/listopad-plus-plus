#include "single_instance.h"

#include <utility>

namespace listopad::app {

SingleInstance::SingleInstance() {
  const std::wstring name = L"Local\\ListopadPP.Instance." + ipc::instance_pipe_name().substr(18);
  mutex_ = CreateMutexW(nullptr, FALSE, name.c_str());
  primary_ = mutex_ && GetLastError() != ERROR_ALREADY_EXISTS;
}

SingleInstance::~SingleInstance() {
  if (server_.joinable()) {
    server_.request_stop();
    const HANDLE pipe = CreateFileW(ipc::instance_pipe_name().c_str(), GENERIC_WRITE,
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
  if (!WaitNamedPipeW(ipc::instance_pipe_name().c_str(), 1500)) return false;
  const HANDLE pipe = CreateFileW(ipc::instance_pipe_name().c_str(), GENERIC_WRITE,
                                  0, nullptr, OPEN_EXISTING, 0, nullptr);
  if (pipe == INVALID_HANDLE_VALUE) return false;
  const auto payload = ipc::encode(request);
  const bool result = ipc::write_frame(pipe, ipc::MessageType::OpenFiles, payload);
  CloseHandle(pipe);
  return result;
}

void SingleInstance::start(Callback callback) {
  if (!primary_ || server_.joinable()) return;
  server_ = std::jthread([callback = std::move(callback)](const std::stop_token stop) {
    while (!stop.stop_requested()) {
      const HANDLE pipe = CreateNamedPipeW(
          ipc::instance_pipe_name().c_str(), PIPE_ACCESS_INBOUND,
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

