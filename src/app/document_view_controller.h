#pragma once

#include "document_session.h"

namespace listopad::app {

class EditorWindow;

class DocumentViewController final {
 public:
  void destroy(EditorWindow& owner, DocumentSession& session) const noexcept;
  [[nodiscard]] bool create(EditorWindow& owner,
                            DocumentSession& session) const;
  [[nodiscard]] bool switch_to(EditorWindow& owner, DocumentSession& session,
                               DocumentViewKind requested) const;
};

}  // namespace listopad::app
