#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;
using llvm::dyn_cast_or_null;

namespace tz {

std::vector<const Expr *> ExprParts::andParts(const Expr *cond) {
  return ExprParts::sameOperatorParts(cond, BinaryOp::LogAnd);
}

std::vector<const Expr *> ExprParts::sameOperatorParts(const Expr *expr, BinaryOp op) {
  std::vector<const Expr *> parts;
  std::vector<const Expr *> stack = {expr};
  while (!stack.empty()) {
    const Expr *next = stack.back();
    stack.pop_back();
    auto *same = dyn_cast_or_null<BinaryExpr>(next);
    if (same != nullptr && same->op == op) {
      stack.push_back(same->rhs);
      stack.push_back(same->lhs);
      continue;
    }
    parts.push_back(next);
  }

  return parts;
}

std::vector<const Expr *> ExprParts::logicParts(const Expr *expr) {
  std::vector<const Expr *> parts;
  std::vector<const Expr *> stack = {expr};
  while (!stack.empty()) {
    const Expr *next = stack.back();
    stack.pop_back();
    auto *logic = dyn_cast_or_null<BinaryExpr>(next);
    bool isSplit =
        logic != nullptr && (logic->op == BinaryOp::LogAnd || logic->op == BinaryOp::LogOr);
    if (isSplit) {
      stack.push_back(logic->rhs);
      stack.push_back(logic->lhs);
      continue;
    }
    parts.push_back(next);
  }

  return parts;
}

std::vector<const Expr *> ExprParts::choiceBranches(const Expr *expr) {
  std::vector<const Expr *> branches;
  std::vector<const Expr *> stack = {expr};
  while (!stack.empty()) {
    const Expr *next = ExprParts::withoutImplicitCasts(stack.back());
    stack.pop_back();
    auto *choice = dyn_cast<ConditionalExpr>(next);
    if (choice != nullptr) {
      stack.push_back(choice->elseExpr);
      stack.push_back(choice->thenExpr);
      continue;
    }
    branches.push_back(next);
  }

  return branches;
}

std::vector<const Expr *> ExprParts::operatorTree(const Expr *expr) {
  std::vector<const Expr *> nodes;
  std::vector<const Expr *> stack = {expr};
  while (!stack.empty()) {
    const Expr *next = stack.back();
    stack.pop_back();
    nodes.push_back(next);
    if (auto *unary = dyn_cast<UnaryExpr>(next)) {
      stack.push_back(unary->operand);
    }
    if (auto *binary = dyn_cast<BinaryExpr>(next)) {
      stack.push_back(binary->rhs);
      stack.push_back(binary->lhs);
    }
  }

  return nodes;
}

std::vector<const Expr *> ExprParts::literalLeaves(const Expr *expr) {
  std::vector<const Expr *> leaves;
  std::vector<const Expr *> stack = {expr};
  while (!stack.empty()) {
    const Expr *next = stack.back();
    stack.pop_back();
    if (auto *list = dyn_cast<ArrayLitExpr>(next)) {
      for (const ArrayLitExpr::Element &element : llvm::reverse(list->elements)) {
        stack.push_back(element.expr);
      }
      continue;
    }
    if (auto *object = dyn_cast<ObjectLitExpr>(next)) {
      for (const ObjectLitExpr::Property &property : llvm::reverse(object->props)) {
        stack.push_back(property.value);
      }
      continue;
    }
    leaves.push_back(next);
  }

  return leaves;
}

const Expr *ExprParts::withoutImplicitCasts(const Expr *expr) {
  const Expr *inner = expr;
  while (auto *conv = dyn_cast<CastExpr>(inner)) {
    if (!conv->isImplicit()) {
      break;
    }
    inner = conv->operand;
  }

  return inner;
}

}
