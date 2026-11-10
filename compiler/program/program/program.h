#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/diagnostics/diagnostics.h"
#include "llvm/Support/SourceMgr.h"
#include <memory>
#include <vector>

namespace tz {

struct Program {
  Program();
  ~Program();

  llvm::SourceMgr sourceMgr;
  AstContext ast;
  Diagnostics diags;
  SourceFile file;
};

}
