#pragma once

#include "compiler/packages/graph/graph.h"
#include "llvm/ADT/StringRef.h"

namespace tz {

// The packages of the project the file belongs to: tz.json in its directory or above.
class ProjectPackages {
public:
  // False after reporting errors.
  bool prepare(llvm::StringRef input);
  // Null without a project.
  const pkg::Graph *graph() const { return graph_; }

private:
  pkg::Graph storage_;
  const pkg::Graph *graph_ = nullptr;
};

} // namespace tz
