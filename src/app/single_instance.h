#pragma once

#include "listopad/ipc_protocol.h"

#include <windows.h>

#include <functional>
#include <string>
#include <thread>

namespace listopad::app {

class SingleInstance final {
 public:
  using Callback = std::function<void(ipc::OpenFilesRequest)>;

  SingleInstance();
  ~SingleInstance();
  SingleInstance(const SingleInstance&) = delete;
  SingleInstance& operator=(const SingleInstance&) = delete;

  [[nodiscard]] bool primary() const noexcept;
  bool send(const ipc::OpenFilesRequest& request) const;
  void start(Callback callback);

 private:
  std::wstring pipe_name_;
  HANDLE mutex_{nullptr};
  bool primary_{false};
  std::jthread server_;
};

}  // namespace listopad::app
