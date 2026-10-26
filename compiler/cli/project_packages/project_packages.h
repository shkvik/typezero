#pragma once

#include "compiler/packages/graph/graph.h"
#include "llvm/ADT/StringRef.h"

namespace tz {

class ProjectPackages {
public:
  bool prepare(llvm::StringRef input);
  const pkg::Graph *graph() const { return graph_; }

private:
  pkg::Graph storage_;
  const pkg::Graph *graph_ = nullptr;
};

}
