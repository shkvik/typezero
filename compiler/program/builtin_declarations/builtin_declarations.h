#pragma once

#include "compiler/program/ast/stmt/stmt.h"
#include "llvm/ADT/ArrayRef.h"
#include <map>
#include <vector>

namespace tz {

enum class BuiltinText {
  PRELUDE,
  ITERATE,
  FILE_SYSTEM_ERRORS,
  CRYPTO_ERRORS,
  CHILD_PROCESS_ERRORS,
};

class BuiltinDeclarations {
public:
  void setItems(BuiltinText text, std::vector<Stmt *> parsed);
  llvm::ArrayRef<Stmt *> getItems(BuiltinText text) const;

private:
  std::map<BuiltinText, std::vector<Stmt *>> items;
};

}
