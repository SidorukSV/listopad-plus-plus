#pragma once

#include "command_controller.h"
#include "document_session.h"
#include "document_view_controller.h"
#include "elevated_client.h"
#include "file_watcher.h"
#include "recovery_controller.h"
#include "search_controller.h"
#include "settings_dialog.h"
#include "tab_controller.h"
#include "window_layout_controller.h"
#include "listopad/command_line.h"
#include "listopad/document.h"
#include "listopad/document_mutation.h"
#include "listopad/emmet_engine.h"
#include "listopad/ipc_protocol.h"
#include "listopad/settings.h"
#include "listopad/session.h"

#include <windows.h>
#include <commctrl.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace listopad::app {

class EditorWindow final {
 public:
  EditorWindow(HINSTANCE instance, Settings settings,
               bool restoring_elevated_restart = false);
  ~EditorWindow();
  EditorWindow(const EditorWindow&) = delete;
  EditorWindow& operator=(const EditorWindow&) = delete;

  bool create(int show_command);
  HWND handle() const noexcept { return window_; }
  void open_request(const ipc::OpenFilesRequest& request);
  void retry_elevated_save(const ElevatedRestartRequest& request);

  static constexpr UINT kOpenRequestMessage = WM_APP + 1;
  static constexpr UINT kExternalChangeMessage = WM_APP + 2;
  static constexpr UINT kSearchResultMessage = WM_APP + 3;
  static constexpr UINT kDocumentMapRefreshMessage = WM_APP + 4;
  static constexpr UINT kGuiSmokeCommandMessage = WM_APP + 63;

 private:
  friend class CommandController;
  friend class DocumentViewController;

  using Tab = DocumentSession;
  using ViewKind = DocumentViewKind;

  class DocumentMutationScope final {
   public:
    DocumentMutationScope(EditorWindow& owner, Tab& tab,
                          DocumentMutationOrigin origin);
    ~DocumentMutationScope();
    DocumentMutationScope(const DocumentMutationScope&) = delete;
    DocumentMutationScope& operator=(const DocumentMutationScope&) = delete;

   private:
    EditorWindow& owner_;
    Tab& tab_;
    DocumentMutationOrigin origin_;
    DocumentMutationPolicy policy_;
    Tab* previous_tab_{nullptr};
    DocumentMutationOrigin previous_origin_{DocumentMutationOrigin::User};
  };

  struct SearchHit {
    int tab_index{-1};
    std::size_t start{0};
    std::size_t length{0};
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
  void update_position_status(const Tab& tab);
  void update_ui();
  void rebuild_menu();
  void prepare_menu_bar(HMENU menu);
  void refresh_view_menu_state();
  void apply_window_theme();
  void recreate_theme_brushes();
  void persist_window_bounds();
  void create_toolbar();
  void load_icon_font();
  int toolbar_height() const;
  LRESULT draw_toolbar(NMTBCUSTOMDRAW& custom);
  void draw_toolbar_glyph(HDC dc, int command, RECT button, bool disabled);
  void toolbar_tooltip(NMTTDISPINFOW& info) const;
  void paint_menu_underline();
  std::wstring fit_tab_title(std::wstring title) const;

  Tab* active_tab();
  const Tab* active_tab() const;
  int active_index() const;
  void activate_tab(int index);
  void add_empty_tab();
  void initialize_tab_identity(Tab& tab);
  bool initialize_session();
  void restore_manifest(const SessionManifest& manifest);
  void restore_recovery(const std::vector<RecoverySnapshot>& snapshots);
  void restore_elevated_restart(
      const SessionManifest& manifest,
      const std::vector<RecoverySnapshot>& snapshots);
  bool restore_session_tab(const SessionTab& stored);
  bool restore_recovery_tab(const RecoverySnapshot& snapshot);
  void apply_stored_view_state(Tab& tab, const SessionTab& stored);
  [[nodiscard]] SessionManifest capture_manifest(bool clean_shutdown) const;
  [[nodiscard]] SessionTab capture_session_tab(const Tab& tab) const;
  void queue_manifest(bool clean_shutdown);
  void mark_tab_edited(Tab& tab);
  void on_recovery_timer();
  void capture_recovery_now();
  void add_recent_file(const std::filesystem::path& path);
  void add_closed_file(const std::filesystem::path& path);
  void reopen_closed_file();
  void clear_recent_files();
  void prepare_clean_shutdown();
  bool persist_elevated_restart_state();
  bool offer_elevated_restart(
      const Tab& tab, const std::filesystem::path& target,
      std::string_view content_sha256);
  void show_settings();
  bool apply_settings(const Settings& settings,
                      const SettingsActions& actions);
  void update_localized_controls();
  [[nodiscard]] static bool editable(const Tab& tab) {
    return tab.editable();
  }
  void destroy_tab_views(Tab& tab);
  bool create_tab_views(Tab& tab);
  bool switch_tab_view(Tab& tab, ViewKind requested);
  bool open_file(const std::filesystem::path& path, const Encoding* forced = nullptr);
  bool close_tab(int index);
  bool confirm_close(Tab& tab);
  bool save_tab(Tab& tab, bool save_as = false);
  void reopen_active(const Encoding& encoding);
  void reload_active();
  void keep_external_active();
  void handle_external_change(const std::filesystem::path& path);

  HWND create_editor();
  void configure_editor(Tab& tab);
  void apply_language(Tab& tab, std::string_view language);
  std::string editor_text(HWND editor) const;
  void set_editor_text(Tab& tab, std::string_view text,
                       DocumentMutationOrigin origin);
  void replace_editor_range(Tab& tab, std::size_t start, std::size_t end,
                            std::string_view text,
                            DocumentMutationOrigin origin);
  void finish_document_mutation(Tab& tab, DocumentMutationOrigin origin);
  bool try_emmet(HWND editor, bool backwards);
  void format_active();
  void show_search(bool replace);
  void apply_search_visibility();
  void find_next();
  void find_all();
  void replace_one();
  void replace_all_open_tabs();
  void handle_search_result(void* raw_result);
  void clear_find_all_results();
  void navigate_search_result(int result_index);
  std::filesystem::path choose_open_file();
  std::filesystem::path choose_save_file(const Tab& tab);

  bool russian() const { return settings_.ui_language != "en"; }
  const wchar_t* tr(const wchar_t* ru, const wchar_t* en) const { return russian() ? ru : en; }

  HINSTANCE instance_{};
  Settings settings_;
  SessionStore session_store_;
  RecoveryController recovery_controller_;
  std::vector<std::filesystem::path> recent_files_;
  std::vector<std::filesystem::path> closed_files_;
  bool session_initialized_{false};
  bool manifest_dirty_{true};
  bool shutdown_prepared_{false};
  bool restoring_elevated_restart_{false};
  bool restarting_elevated_{false};
  bool recovery_write_error_{false};
  bool ending_windows_session_{false};
  HWND window_{nullptr};
  HWND toolbar_{nullptr};
  HWND tabs_{nullptr};
  HWND status_{nullptr};
  HWND banner_{nullptr};
  HWND reload_button_{nullptr};
  HWND keep_button_{nullptr};
  HWND search_panel_{nullptr};
  HWND find_text_{nullptr};
  HWND replace_text_{nullptr};
  HWND find_button_{nullptr};
  HWND find_all_button_{nullptr};
  HWND replace_button_{nullptr};
  HWND replace_all_button_{nullptr};
  HWND search_results_{nullptr};
  HWND regex_check_{nullptr};
  HWND case_check_{nullptr};
  HWND all_tabs_check_{nullptr};
  HWND whole_word_check_{nullptr};
  HWND wrap_check_{nullptr};
  HWND selection_only_check_{nullptr};
  HFONT editor_font_{nullptr};
  HFONT tab_font_{nullptr};
  HFONT icon_font_{nullptr};
  HICON window_icon_large_{nullptr};
  HICON window_icon_small_{nullptr};
  HANDLE icon_font_resource_{nullptr};
  HIMAGELIST toolbar_images_{nullptr};
  int dpi_{96};
  HBRUSH window_brush_{nullptr};
  HBRUSH panel_brush_{nullptr};
  HBRUSH field_brush_{nullptr};
  HBRUSH banner_brush_{nullptr};
  TabController tab_controller_;
  std::vector<std::string> language_menu_ids_;
  std::vector<std::unique_ptr<MenuVisual>> menu_visuals_;
  FileWatcher watcher_;
  ElevatedClient elevated_;
  EmmetEngine emmet_;
  SearchController search_;
  std::vector<SearchHit> search_hits_;
  int pressed_close_tab_{-1};
  bool dark_{false};
  CommandController command_controller_;
  DocumentViewController view_controller_;
  WindowLayoutController layout_controller_;
  Tab* active_mutation_tab_{nullptr};
  DocumentMutationOrigin active_mutation_origin_{
      DocumentMutationOrigin::User};
};

}  // namespace listopad::app
