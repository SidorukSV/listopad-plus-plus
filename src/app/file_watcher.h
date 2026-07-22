#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace listopad::app {

class FileWatcher final {
 public:
  using Callback = std::function<void(const std::filesystem::path&)>;

  explicit FileWatcher(Callback callback);
  ~FileWatcher();
  FileWatcher(const FileWatcher&) = delete;
  FileWatcher& operator=(const FileWatcher&) = delete;

  void watch(const std::filesystem::path& file);
  void clear();

 private:
  struct Entry;
  Callback callback_;
  std::mutex mutex_;
  std::vector<std::unique_ptr<Entry>> entries_;
};

}  // namespace listopad::app

