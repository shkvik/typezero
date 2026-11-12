#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;
using llvm::dyn_cast_or_null;

namespace tz {

std::vector<const Expr *> ExprParts::andParts(const Expr *cond) {
  return ExprParts::sameOperatorParts(cond, BinaryOp::LogAnd);
}

std::vector<const Expr *> ExprParts::sameOperatorParts(const Expr *expr, BinaryOp op) {
  auto parts = std::vector<const Expr *>();
  auto stack = std::vector<const Expr *>{expr};
  while (!stack.empty()) {
    const Expr *next = stack.back();
    stack.pop_back();
    auto *same = dyn_cast_or_null<BinaryExpr>(next);
    if (same != nullptr && same->op == op) {
      stack.push_back(same->rhs);
      stack.push_back(same->lhs);
    } else {
      parts.push_back(next);
    }
  }

  return parts;
}

std::vector<const Expr *> ExprParts::logicParts(const Expr *expr) {
  auto parts = std::vector<const Expr *>();
  auto stack = std::vector<const Expr *>{expr};
  while (!stack.empty()) {
    const Expr *next = stack.back();
    stack.pop_back();
    auto *logic = dyn_cast_or_null<BinaryExpr>(next);
    bool isSplit =
        logic != nullptr && (logic->op == BinaryOp::LogAnd || logic->op == BinaryOp::LogOr);
    if (isSplit) {
      stack.push_back(logic->rhs);
      stack.push_back(logic->lhs);
    } else {
      parts.push_back(next);
    }
  }

  return parts;
}

std::vector<const Expr *> ExprParts::choiceBranches(const Expr *expr) {
  auto branches = std::vector<const Expr *>();
  auto stack = std::vector<const Expr *>{expr};
  while (!stack.empty()) {
    const auto *next = ExprParts::withoutImplicitCasts(stack.back());
    stack.pop_back();
    auto *choice = dyn_cast<ConditionalExpr>(next);
    if (choice != nullptr) {
      stack.push_back(choice->elseExpr);
      stack.push_back(choice->thenExpr);
    } else {
      branches.push_back(next);
    }
  }

  return branches;
}

std::vector<const Expr *> ExprParts::operatorTree(const Expr *expr) {
  auto nodes = std::vector<const Expr *>();
  auto stack = std::vector<const Expr *>{expr};
  while (!stack.empty()) {
    const Expr *next = stack.back();
    stack.pop_back();
    nodes.push_back(next);
    auto *unary = dyn_cast<UnaryExpr>(next);
    auto *binary = dyn_cast<BinaryExpr>(next);
    if (unary != nullptr) {
      stack.push_back(unary->operand);
    }
    if (binary != nullptr) {
      stack.push_back(binary->rhs);
      stack.push_back(binary->lhs);
    }
  }

  return nodes;
}

std::vector<const Expr *> ExprParts::literalLeaves(const Expr *expr) {
  auto leaves = std::vector<const Expr *>();
  auto stack = std::vector<const Expr *>{expr};
  while (!stack.empty()) {
    const Expr *next = stack.back();
    stack.pop_back();
    auto *list = dyn_cast<ArrayLitExpr>(next);
    auto *object = dyn_cast<ObjectLitExpr>(next);
    if (list != nullptr) {
      ExprParts::pushElements(list, stack);
    } else if (object != nullptr) {
      ExprParts::pushValues(object, stack);
    } else {
      leaves.push_back(next);
    }
  }

  return leaves;
}

void ExprParts::pushElements(const ArrayLitExpr *list, std::vector<const Expr *> &stack) {
  for (const ArrayLitExpr::Element &element : llvm::reverse(list->elements)) {
    stack.push_back(element.expr);
  }
}

void ExprParts::pushValues(const ObjectLitExpr *object, std::vector<const Expr *> &stack) {
  for (const ObjectLitExpr::Property &property : llvm::reverse(object->props)) {
    stack.push_back(property.value);
  }
}

const Expr *ExprParts::withoutImplicitCasts(const Expr *expr) {
  const Expr *inner = expr;
  auto *conversion = dyn_cast<CastExpr>(inner);
  while (conversion != nullptr && conversion->isImplicit()) {
    inner = conversion->operand;
    conversion = dyn_cast<CastExpr>(inner);
  }

  return inner;
}

Expr *ExprParts::withoutImplicitCasts(Expr *expr) {
  Expr *inner = expr;
  auto *conversion = dyn_cast<CastExpr>(inner);
  while (conversion != nullptr && conversion->isImplicit()) {
    inner = conversion->operand;
    conversion = dyn_cast<CastExpr>(inner);
  }

  return inner;
}

const Expr *ExprParts::withoutCasts(const Expr *expr) {
  const Expr *inner = expr;
  auto *conversion = dyn_cast<CastExpr>(inner);
  while (conversion != nullptr) {
    inner = conversion->operand;
    conversion = dyn_cast<CastExpr>(inner);
  }

  return inner;
}

Expr *ExprParts::withoutCasts(Expr *expr) {
  Expr *inner = expr;
  auto *conversion = dyn_cast<CastExpr>(inner);
  while (conversion != nullptr) {
    inner = conversion->operand;
    conversion = dyn_cast<CastExpr>(inner);
  }

  return inner;
}

}
