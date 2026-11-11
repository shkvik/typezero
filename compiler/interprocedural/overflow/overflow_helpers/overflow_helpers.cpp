#include "compiler/interprocedural/overflow/overflow_helpers/overflow_helpers.h"
#include "compiler/interprocedural/overflow/value_range/value_range.h"
#include "compiler/program/ast_walker/ast_walker.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Path.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

const Expr *OverflowHelpers::stripCasts(const Expr *expr) {
  while (auto *conv = dyn_cast_or_null<CastExpr>(expr)) {
    if (!conv->isImplicit()) {
      break;
    }
    expr = conv->operand;
  }

  return expr;
}

std::optional<size_t> OverflowHelpers::callbackArg(Builtin builtin) {
  switch (builtin) {
  case Builtin::ArrForEach:
  case Builtin::ArrMap:
  case Builtin::ArrFilter:
  case Builtin::ArrFlatMap:
  case Builtin::ArrReduce:
  case Builtin::ArrReduceRight:
  case Builtin::ArrFind:
  case Builtin::ArrFindIndex:
  case Builtin::ArrFindLast:
  case Builtin::ArrFindLastIndex:
  case Builtin::ArrSome:
  case Builtin::ArrEvery:
  case Builtin::ArrSortWith:
  case Builtin::MapForEach:
    return 0;
  case Builtin::ArrayFrom:
    return 1;
  default:
    return std::nullopt;
  }
}

Val OverflowHelpers::widenVal(const Val &old, const Val &grown, const Type *type) {
  auto links = std::vector<WidenLink>{WidenLink{.old = &old, .grown = &grown, .type = type}};
  while (links.back().old->elem != nullptr && links.back().grown->elem != nullptr) {
    WidenLink last = links.back();
    bool isArray = last.type != nullptr && last.type->nonOptional()->isArray();
    const Type *element = isArray ? last.type->nonOptional()->inner() : nullptr;
    links.push_back(WidenLink{
        .old = last.old->elem.get(),
        .grown = last.grown->elem.get(),
        .type = element,
    });
  }
  Val widened = OverflowHelpers::widenLink(links.back());
  for (size_t i = links.size() - 1; i > 0; --i) {
    Val outer = OverflowHelpers::widenLink(links[i - 1]);
    outer.elem = ValueRange::share(widened);
    widened = outer;
  }

  return widened;
}

Val OverflowHelpers::widenLink(const WidenLink &link) {
  Val widened = *link.grown;
  Limits numLimits = ValueRange::limitsOf(link.type);
  widened.num = OverflowHelpers::widenNum(link.old->num, link.grown->num, numLimits);
  widened.len = OverflowHelpers::widenNum(link.old->len, link.grown->len, {0, kLengthMax});

  return widened;
}

Num OverflowHelpers::widenNum(const Num &old, Num grown, Limits limits) {
  if (old.empty() || grown.empty()) {
    return grown;
  }
  if (grown.lo < old.lo) {
    grown.lo = limits.lo;
    if (!ValueRange::dangerous(grown.loOrigin)) {
      grown.loOrigin = Origin::Trusted, grown.loWhy = nullptr;
    }
  }
  if (grown.hi > old.hi) {
    grown.hi = limits.hi;
    if (!ValueRange::dangerous(grown.hiOrigin)) {
      grown.hiOrigin = Origin::Trusted, grown.hiWhy = nullptr;
    }
  }

  return grown;
}

bool OverflowHelpers::containsLoop(const Stmt *stmt) {
  auto pending = std::vector<const Stmt *>{stmt};
  while (!pending.empty()) {
    const Stmt *next = pending.back();
    pending.pop_back();
    if (next == nullptr) {
      continue;
    }
    if (OverflowHelpers::isLoopHere(next)) {
      return true;
    }
    auto parts = OverflowHelpers::loopParts(next);
    pending.insert(pending.end(), parts.rbegin(), parts.rend());
  }

  return false;
}

bool OverflowHelpers::isLoopHere(const Stmt *stmt) {
  if (isa<WhileStmt, DoWhileStmt, ForStmt, ForOfStmt>(stmt)) {
    return true;
  }
  if (isa<BlockStmt, LabeledStmt>(stmt)) {
    return false;
  }

  return OverflowHelpers::passesCallback(stmt);
}

std::vector<const Stmt *> OverflowHelpers::loopParts(const Stmt *stmt) {
  const auto *block = dyn_cast<BlockStmt>(stmt);
  if (block != nullptr) {
    return std::vector<const Stmt *>(block->body.begin(), block->body.end());
  }
  const auto *branch = dyn_cast<IfStmt>(stmt);
  if (branch != nullptr) {
    return {branch->thenStmt, branch->elseStmt};
  }
  const auto *labeled = dyn_cast<LabeledStmt>(stmt);
  if (labeled != nullptr) {
    return {labeled->body};
  }
  const auto *choice = dyn_cast<SwitchStmt>(stmt);
  if (choice == nullptr) {
    return {};
  }
  auto parts = std::vector<const Stmt *>();
  for (const SwitchCase &c : choice->cases) {
    parts.insert(parts.end(), c.body.begin(), c.body.end());
  }

  return parts;
}

bool OverflowHelpers::passesCallback(const Stmt *stmt) {
  for (const Expr *e : AstWalker::exprsIn(stmt, false)) {
    auto *call = dyn_cast<CallExpr>(e);
    if (call == nullptr) {
      continue;
    }
    std::optional<size_t> at = OverflowHelpers::callbackArg(call->builtin());
    bool isPassingFunction = at.has_value() && *at < call->args.size() &&
                             isa<FunctionExpr>(OverflowHelpers::stripCasts(call->args[*at]));
    if (isPassingFunction) {
      return true;
    }
  }

  return false;
}

}
