#include "file_watcher.h"

#include "listopad/file_io.h"
#include "listopad/strings.h"

#include <windows.h>

#include <array>

namespace listopad::app {

struct FileWatcher::Entry {
  std::filesystem::path file;
  HANDLE directory{INVALID_HANDLE_VALUE};
  std::jthread thread;
};

FileWatcher::FileWatcher(Callback callback) : callback_(std::move(callback)) {}
FileWatcher::~FileWatcher() { clear(); }

void FileWatcher::clear() {
  std::vector<std::unique_ptr<Entry>> old;
  {
    std::scoped_lock lock(mutex_);
    old.swap(entries_);
  }
  for (auto& entry : old) {
    entry->thread.request_stop();
    if (entry->directory != INVALID_HANDLE_VALUE) CancelIoEx(entry->directory, nullptr);
  }
}

void FileWatcher::watch(const std::filesystem::path& input) {
  const auto file = canonical_path(input);
  std::scoped_lock lock(mutex_);
  for (const auto& entry : entries_) if (lowercase(entry->file.wstring()) == lowercase(file.wstring())) return;

  auto entry = std::make_unique<Entry>();
  entry->file = file;
  entry->directory = CreateFileW(file.parent_path().c_str(), FILE_LIST_DIRECTORY,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING,
                                 FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
  if (entry->directory == INVALID_HANDLE_VALUE) return;
  Entry* raw = entry.get();
  const Callback callback = callback_;
  entry->thread = std::jthread([raw, callback](const std::stop_token stop) {
    std::array<std::byte, 32 * 1024> buffer{};
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    while (!stop.stop_requested()) {
      OVERLAPPED overlapped{}; overlapped.hEvent = event;
      ResetEvent(event);
      if (!ReadDirectoryChangesW(raw->directory, buffer.data(), static_cast<DWORD>(buffer.size()), FALSE,
                                 FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE |
                                     FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION,
                                 nullptr, &overlapped, nullptr)) break;
      const DWORD wait = WaitForSingleObject(event, 1000);
      if (stop.stop_requested()) { CancelIoEx(raw->directory, &overlapped); break; }
      if (wait != WAIT_OBJECT_0) { CancelIoEx(raw->directory, &overlapped); continue; }
      DWORD transferred = 0;
      if (!GetOverlappedResult(raw->directory, &overlapped, &transferred, FALSE)) continue;
      std::size_t offset = 0;
      while (offset < transferred) {
        const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer.data() + offset);
        const std::wstring name(info->FileName, info->FileNameLength / sizeof(wchar_t));
        if (lowercase(name) == lowercase(raw->file.filename().wstring())) callback(raw->file);
        if (info->NextEntryOffset == 0) break;
        offset += info->NextEntryOffset;
      }
    }
    CloseHandle(event);
    CloseHandle(raw->directory);
    raw->directory = INVALID_HANDLE_VALUE;
  });
  entries_.push_back(std::move(entry));
}

}  // namespace listopad::app

