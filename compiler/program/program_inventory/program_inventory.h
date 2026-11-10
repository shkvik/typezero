#pragma once

#include "compiler/program/widening_store/widening_store.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/SMLoc.h"
#include <utility>
#include <vector>

namespace tz {

class Expr;
class Stmt;
class Type;
struct ClassDecl;
struct FunctionDecl;

struct NamedUse {
  const Stmt *decl = nullptr;
  llvm::SMLoc name;
  llvm::StringRef member;
};

struct ProgramInventory {
  bool usesTls = false;
  bool usesAsync = false;
  bool topLevelAwait = false;
  bool comparesFunctions = false;
  const Type *nativeError = nullptr;
  std::vector<const Type *> subtypes;
  std::vector<ClassDecl *> lateClasses;
  std::vector<FunctionDecl *> instances;
  std::vector<const Type *> listenerReceivers;
  llvm::DenseMap<const Expr *, NamedUse> namedUses;
  std::vector<std::pair<const Type *, const Type *>> views;
  WideningStore widenings;
};

}
