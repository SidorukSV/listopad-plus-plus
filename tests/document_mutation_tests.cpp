#include "listopad/document_mutation.h"

#include <catch2/catch_test_macros.hpp>

using namespace listopad;

TEST_CASE("baseline mutations never become undoable edits") {
  for (const auto origin :
       {DocumentMutationOrigin::Load, DocumentMutationOrigin::Reload}) {
    const auto policy = mutation_policy(origin);
    CHECK(policy.undo == UndoPolicy::ResetBaseline);
    CHECK(policy.set_save_point);
    CHECK(policy.reset_snippet);
    CHECK(policy.recolor);
    CHECK(policy.refresh_document_map);
  }
}

TEST_CASE("programmatic edits are one undo action with explicit side effects") {
  const auto format = mutation_policy(DocumentMutationOrigin::Format);
  CHECK(format.undo == UndoPolicy::RecordSingleAction);
  CHECK(format.reset_snippet);
  CHECK(format.invalidate_search);
  CHECK(format.recolor);

  const auto emmet = mutation_policy(DocumentMutationOrigin::Emmet);
  CHECK(emmet.undo == UndoPolicy::RecordSingleAction);
  CHECK_FALSE(emmet.reset_snippet);
  CHECK(emmet.refresh_document_map);
}

TEST_CASE("view switching is not a document mutation") {
  const auto policy = mutation_policy(DocumentMutationOrigin::ViewSwitch);
  CHECK(policy.undo == UndoPolicy::Preserve);
  CHECK_FALSE(policy.set_save_point);
  CHECK_FALSE(policy.invalidate_search);
  CHECK_FALSE(policy.refresh_document_map);
}
