#pragma once

namespace listopad {

enum class DocumentMutationOrigin {
  User,
  Load,
  Reload,
  Format,
  Replace,
  Emmet,
  ViewSwitch,
};

enum class UndoPolicy {
  Preserve,
  RecordSingleAction,
  ResetBaseline,
};

struct DocumentMutationPolicy {
  UndoPolicy undo{UndoPolicy::Preserve};
  bool set_save_point{false};
  bool reset_snippet{false};
  bool invalidate_search{false};
  bool recolor{false};
  bool refresh_document_map{false};
};

[[nodiscard]] constexpr DocumentMutationPolicy mutation_policy(
    const DocumentMutationOrigin origin) noexcept {
  switch (origin) {
    case DocumentMutationOrigin::User:
      return {
          .undo = UndoPolicy::Preserve,
          .invalidate_search = true,
          .refresh_document_map = true,
      };
    case DocumentMutationOrigin::Load:
    case DocumentMutationOrigin::Reload:
      return {
          .undo = UndoPolicy::ResetBaseline,
          .set_save_point = true,
          .reset_snippet = true,
          .invalidate_search = true,
          .recolor = true,
          .refresh_document_map = true,
      };
    case DocumentMutationOrigin::Format:
      return {
          .undo = UndoPolicy::RecordSingleAction,
          .reset_snippet = true,
          .invalidate_search = true,
          .recolor = true,
          .refresh_document_map = true,
      };
    case DocumentMutationOrigin::Replace:
      return {
          .undo = UndoPolicy::RecordSingleAction,
          .reset_snippet = true,
          .invalidate_search = true,
          .refresh_document_map = true,
      };
    case DocumentMutationOrigin::Emmet:
      return {
          .undo = UndoPolicy::RecordSingleAction,
          .invalidate_search = true,
          .refresh_document_map = true,
      };
    case DocumentMutationOrigin::ViewSwitch:
      return {};
  }
  return {};
}

}  // namespace listopad
