#pragma once

#include "elevated_client.h"
#include "file_watcher.h"
#include "listopad/command_line.h"
#include "listopad/document.h"
#include "listopad/emmet_engine.h"
#include "listopad/ipc_protocol.h"
#include "listopad/settings.h"

#include <windows.h>
#include <commctrl.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

namespace listopad::app {

class EditorWindow final {
 public:
  EditorWindow(HINSTANCE instance, Settings settings);
  ~EditorWindow();
  EditorWindow(const EditorWindow&) = delete;
  EditorWindow& operator=(const EditorWindow&) = delete;

  bool create(int show_command);
  HWND handle() const noexcept { return window_; }
  void open_request(const ipc::OpenFilesRequest& request);

  static constexpr UINT kOpenRequestMessage = WM_APP + 1;
  static constexpr UINT kExternalChangeMessage = WM_APP + 2;
  static constexpr UINT kSearchResultMessage = WM_APP + 3;

 private:
  struct Tab {
    Document document;
    HWND view{nullptr};
    bool external_notice_pending{false};
    std::vector<EmmetField> snippet_fields;
    std::size_t snippet_index{0};
  };

  struct MenuVisual {
    std::wstring text;
  };

  static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
  static LRESULT CALLBACK editor_subclass(HWND window, UINT message, WPARAM wparam,
                                           LPARAM lparam, UINT_PTR id, DWORD_PTR data);
  static LRESULT CALLBACK tabs_subclass(HWND window, UINT message, WPARAM wparam,
                                        LPARAM lparam, UINT_PTR id, DWORD_PTR data);
  static LRESULT CALLBACK status_subclass(HWND window, UINT message, WPARAM wparam,
                                          LPARAM lparam, UINT_PTR id, DWORD_PTR data);
  LRESULT dispatch(UINT message, WPARAM wparam, LPARAM lparam);
  bool on_create();
  void on_size();
  void on_command(int command, int notification, HWND control);
  void on_notify(const NMHDR& notification);
  void update_layout();
  void update_ui();
  void rebuild_menu();
  void prepare_menu_bar(HMENU menu);
  void apply_window_theme();
  void recreate_theme_brushes();

  Tab* active_tab();
  const Tab* active_tab() const;
  int active_index() const;
  void activate_tab(int index);
  void add_empty_tab();
  bool open_file(const std::filesystem::path& path, const Encoding* forced = nullptr);
  bool close_tab(int index);
  bool confirm_close(Tab& tab);
  bool save_tab(Tab& tab, bool save_as = false);
  void reopen_active(const Encoding& encoding);
  void reload_active();
  void keep_external_active();
  void handle_external_change(const std::filesystem::path& path);

  HWND create_editor();
  void configure_editor(HWND editor, const Document& document);
  void apply_language(Tab& tab, std::string_view language);
  std::string editor_text(HWND editor) const;
  void set_editor_text(HWND editor, std::string_view text, bool save_point);
  bool try_emmet(HWND editor, bool backwards);
  void format_active();
  void show_search(bool replace);
  void find_next();
  void replace_one();
  void replace_all_open_tabs();
  void handle_search_result(void* raw_result);
  std::filesystem::path choose_open_file();
  std::filesystem::path choose_save_file(const Tab& tab);

  bool russian() const { return settings_.ui_language != "en"; }
  const wchar_t* tr(const wchar_t* ru, const wchar_t* en) const { return russian() ? ru : en; }

  HINSTANCE instance_{};
  Settings settings_;
  HWND window_{nullptr};
  HWND tabs_{nullptr};
  HWND status_{nullptr};
  HWND banner_{nullptr};
  HWND reload_button_{nullptr};
  HWND keep_button_{nullptr};
  HWND search_panel_{nullptr};
  HWND find_text_{nullptr};
  HWND replace_text_{nullptr};
  HWND find_button_{nullptr};
  HWND replace_button_{nullptr};
  HWND replace_all_button_{nullptr};
  HWND regex_check_{nullptr};
  HWND case_check_{nullptr};
  HWND all_tabs_check_{nullptr};
  HWND whole_word_check_{nullptr};
  HWND wrap_check_{nullptr};
  HWND selection_only_check_{nullptr};
  HFONT editor_font_{nullptr};
  HBRUSH window_brush_{nullptr};
  HBRUSH panel_brush_{nullptr};
  HBRUSH field_brush_{nullptr};
  HBRUSH banner_brush_{nullptr};
  std::vector<Tab> documents_;
  std::vector<std::string> language_menu_ids_;
  std::vector<std::unique_ptr<MenuVisual>> menu_visuals_;
  FileWatcher watcher_;
  ElevatedClient elevated_;
  EmmetEngine emmet_;
  std::jthread search_thread_;
  std::uint64_t search_generation_{0};
  int pressed_close_tab_{-1};
  bool dark_{false};
};

}  // namespace listopad::app
