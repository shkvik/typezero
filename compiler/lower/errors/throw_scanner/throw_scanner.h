#pragma once

#include "compiler/lower/lowering/lowering.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include <functional>
#include <map>
#include <memory>
#include <optional>

namespace tz {

struct ThrowSummary {
  bool intrinsic = false;
  bool resumes = false;
  std::vector<const FunctionDecl *> calls;
  std::vector<const Type *> valueCalls;
  std::vector<std::pair<const FunctionDecl *, const CallExpr *>> allCalls;
  std::vector<const Type *> allValueCalls;
  std::vector<std::pair<const Type *, const FunctionDecl *>> values;
  std::vector<std::pair<const Type *, const Expr *>> fromThrowables;
  std::vector<std::pair<const Type *, const Type *>> adapters;
  std::vector<const FunctionDecl *> nested;
};

void forEachImplementation(const FunctionDecl *fn,
                           llvm::function_ref<void(const FunctionDecl *)> visit);

std::optional<int64_t> constantInt(const Expr *expr);

const FunctionDecl *namedFunction(const Expr *expr);

class ThrowCalls;

class ThrowScanner {
public:
  explicit ThrowScanner(ThrowSummary &out);
  void stmt(const Stmt *stmt, bool caught);
  void expr(const Expr *expr, bool caught);

private:
  ThrowSummary &out_;

public:
  ~ThrowScanner();

  ThrowCalls &throwCalls() const { return *throwCallsPart_; }

private:
  friend class ThrowCalls;
  std::unique_ptr<ThrowCalls> throwCallsPart_;
};

}
