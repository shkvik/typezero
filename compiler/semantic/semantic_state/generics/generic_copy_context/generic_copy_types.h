#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "llvm/Support/SMLoc.h"
#include <string>
#include <vector>

namespace tz {

struct InstanceRecord {
  FunctionDecl *decl = nullptr;
  FuncSymbol *generic = nullptr;
  ClassDecl *cls = nullptr;
  std::vector<const Type *> args;
  std::vector<const Type *> concrete;
  std::vector<const Type *> exact;
  llvm::SMLoc callLoc;
  const InstanceRecord *requester = nullptr;
  bool checking = false, checked = false, failed = false;
  bool isReported = false;
  std::string message;
  llvm::SMLoc errorLoc;
};

}
