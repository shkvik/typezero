#include "compiler/overflow/overflow_helpers/overflow_helpers.h"
#include "compiler/check/facts/facts.h"
#include "compiler/check/sema/sema.h"
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
#include "compiler/overflow/value_range/value_range.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

const Expr *stripCasts(const Expr *expr) {
  while (auto *conv = dyn_cast_or_null<CastExpr>(expr)) {
    if (!conv->isImplicit()) {
      break;
    }
    expr = conv->operand;
  }

  return expr;
}

std::optional<size_t> callbackArg(Builtin builtin) {
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

Val widenVal(const Val &old, const Val &grown, const Type *type) {
  Val r = grown;
  auto widen = [](const Num &o, Num n, Limits l) {
    if (o.empty() || n.empty()) {
      return n;
    }
    // An end that grows across rounds comes of recursion, or of data that
    // is updated from itself.
    if (n.lo < o.lo) {
      n.lo = l.lo;
      if (!dangerous(n.loOrigin)) {
        n.loOrigin = Origin::Trusted, n.loWhy = nullptr;
      }
    }
    if (n.hi > o.hi) {
      n.hi = l.hi;
      if (!dangerous(n.hiOrigin)) {
        n.hiOrigin = Origin::Trusted, n.hiWhy = nullptr;
      }
    }

    return n;
  };
  r.num = widen(old.num, grown.num, limitsOf(type));
  r.len = widen(old.len, grown.len, {0, kLengthMax});
  if (old.elem && grown.elem) {
    const Type *element =
        type && type->nonOptional()->isArray() ? type->nonOptional()->inner() : nullptr;
    r.elem = share(widenVal(*old.elem, *grown.elem, element));
  }

  return r;
}

bool containsLoop(const Stmt *stmt) {
  if (!stmt) {
    return false;
  }
  switch (stmt->kind()) {
  case Stmt::Kind::While:
  case Stmt::Kind::DoWhile:
  case Stmt::Kind::For:
  case Stmt::Kind::ForOf:
    return true;
  case Stmt::Kind::Block:
    return llvm::any_of(cast<BlockStmt>(stmt)->body, containsLoop);
  case Stmt::Kind::If:
    if (containsLoop(cast<IfStmt>(stmt)->thenStmt) || containsLoop(cast<IfStmt>(stmt)->elseStmt)) {
      return true;
    }
    break;
  case Stmt::Kind::Labeled:
    return containsLoop(cast<LabeledStmt>(stmt)->body);
  case Stmt::Kind::Switch:
    for (const SwitchCase &c : cast<SwitchStmt>(stmt)->cases) {
      if (llvm::any_of(c.body, containsLoop)) {
        return true;
      }
    }
    break;
  case Stmt::Kind::Try: {
    auto *s = cast<TryStmt>(stmt);
    if (containsLoop(s->block) || containsLoop(s->handler) || containsLoop(s->finalizer)) {
      return true;
    }
    break;
  }
  default:
    break;
  }
  bool found = false;
  visitExprs(
      stmt,
      [&](const Expr *e) {
        if (auto *call = dyn_cast<CallExpr>(e)) {
          if (std::optional<size_t> at = callbackArg(call->builtin);
              at && *at < call->args.size()) {
            found |= isa<FunctionExpr>(stripCasts(call->args[*at]));
          }
        }
      },
      /*intoFunctions=*/false);

  return found;
}

} // namespace tz
