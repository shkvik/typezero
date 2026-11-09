#include "compiler/semantic/flow/placed_nodes/placed_nodes.h"

using llvm::dyn_cast;

namespace tz {

PlacedNodes PlacedNodes::collect(llvm::ArrayRef<const Stmt *> stmts,
                                 llvm::ArrayRef<const Expr *> exprs) {
  PlacedNodes nodes;
  for (const Stmt *stmt : stmts) {
    nodes.addStmt(stmt, TreePlace());
  }
  for (const Expr *expr : exprs) {
    nodes.addExpr(expr, TreePlace());
  }
  size_t nextStmt = 0;
  size_t nextExpr = 0;
  while (nextStmt < nodes.stmts.size() || nextExpr < nodes.exprs.size()) {
    if (nextStmt < nodes.stmts.size()) {
      nodes.addStmtChildren(nextStmt);
    } else {
      nodes.addExprChildren(nextExpr);
    }
  }

  return nodes;
}

llvm::ArrayRef<PlacedStmt> PlacedNodes::getStmts() const {
  return this->stmts;
}

llvm::ArrayRef<PlacedExpr> PlacedNodes::getExprs() const {
  return this->exprs;
}

void PlacedNodes::addStmtChildren(size_t &nextStmt) {
  auto placed = this->stmts[nextStmt];
  nextStmt += 1;
  this->addChildren(placed.stmt, placed.place);
}

void PlacedNodes::addExprChildren(size_t &nextExpr) {
  auto placed = this->exprs[nextExpr];
  nextExpr += 1;
  this->addChildren(placed.expr, placed.place);
}

void PlacedNodes::addStmt(const Stmt *stmt, const TreePlace &place) {
  if (stmt != nullptr) {
    this->stmts.push_back({stmt, place});
  }
}

void PlacedNodes::addExpr(const Expr *expr, const TreePlace &place) {
  if (expr != nullptr) {
    this->exprs.push_back({expr, place});
  }
}

void PlacedNodes::addChildren(const Stmt *stmt, const TreePlace &place) {
  this->addPlainParts(stmt, place);
  this->addLoopParts(stmt, place);
  this->addBranchParts(stmt, place);
  this->addDeclarationParts(stmt, place);
}

void PlacedNodes::addPlainParts(const Stmt *stmt, const TreePlace &place) {
  if (auto *block = dyn_cast<BlockStmt>(stmt)) {
    for (const Stmt *inner : block->body) {
      this->addStmt(inner, place);
    }
  }
  if (auto *statement = dyn_cast<ExprStmt>(stmt)) {
    this->addExpr(statement->expr, place);
  }
  if (auto *result = dyn_cast<ReturnStmt>(stmt)) {
    this->addExpr(result->value, place);
  }
}

void PlacedNodes::addLoopParts(const Stmt *stmt, const TreePlace &place) {
  TreePlace inside = place.enterLoop();
  if (auto *loop = dyn_cast<WhileStmt>(stmt)) {
    this->addExpr(loop->cond, inside);
    this->addStmt(loop->body, inside);
  }
  if (auto *loop = dyn_cast<DoWhileStmt>(stmt)) {
    this->addStmt(loop->body, inside);
    this->addExpr(loop->cond, inside);
  }
  if (auto *loop = dyn_cast<ForStmt>(stmt)) {
    this->addStmt(loop->init, inside);
    this->addExpr(loop->cond, inside);
    this->addExpr(loop->step, inside);
    this->addStmt(loop->body, inside);
  }
  if (auto *loop = dyn_cast<ForOfStmt>(stmt)) {
    this->addExpr(loop->iterable, place);
    this->addStmt(loop->body, inside);
  }
}

void PlacedNodes::addBranchParts(const Stmt *stmt, const TreePlace &place) {
  if (auto *choice = dyn_cast<IfStmt>(stmt)) {
    this->addExpr(choice->cond, place);
    this->addStmt(choice->thenStmt, place);
    this->addStmt(choice->elseStmt, place);
  }
  if (auto *choice = dyn_cast<SwitchStmt>(stmt)) {
    this->addExpr(choice->discriminant, place);
    for (const SwitchCase &branch : choice->cases) {
      this->addExpr(branch.test, place);
      for (const Stmt *inner : branch.body) {
        this->addStmt(inner, place.enterSwitch());
      }
    }
  }
  if (auto *labeled = dyn_cast<LabeledStmt>(stmt)) {
    this->addStmt(labeled->body, place.enterLabel(labeled->label));
  }
}

void PlacedNodes::addDeclarationParts(const Stmt *stmt, const TreePlace &place) {
  if (auto *declaration = dyn_cast<VarDeclStmt>(stmt)) {
    for (const VarDeclarator &declarator : declaration->decls) {
      this->addExpr(declarator.init, place);
    }
  }
  if (auto *fn = dyn_cast<FunctionDecl>(stmt)) {
    this->addFunctionChildren(fn, place);
  }
  if (auto *cls = dyn_cast<ClassDecl>(stmt)) {
    this->addClassChildren(cls, place);
  }
}

void PlacedNodes::addChildren(const Expr *expr, const TreePlace &place) {
  this->addCallParts(expr, place);
  this->addAccessParts(expr, place);
  this->addOperatorParts(expr, place);
  this->addListParts(expr, place);
  this->addNestedCodeParts(expr, place);
}

void PlacedNodes::addCallParts(const Expr *expr, const TreePlace &place) {
  if (auto *call = dyn_cast<CallExpr>(expr)) {
    this->addExpr(call->callee, place);
    this->addExprs(call->args, place);
  }
  if (auto *made = dyn_cast<NewExpr>(expr)) {
    const CallExpr *constructor = made->ctor();
    this->addExpr(made->callee, place);
    this->addExprs(constructor != nullptr ? constructor->args : made->args, place);
  }
}

void PlacedNodes::addAccessParts(const Expr *expr, const TreePlace &place) {
  if (auto *member = dyn_cast<MemberExpr>(expr)) {
    this->addExpr(member->object, place);
  }
  if (auto *index = dyn_cast<IndexExpr>(expr)) {
    this->addExpr(index->object, place);
    this->addExpr(index->index, place);
  }
  if (auto *assign = dyn_cast<AssignExpr>(expr)) {
    this->addExpr(assign->target, place);
    this->addExpr(assign->value, place);
  }
  if (auto *update = dyn_cast<UpdateExpr>(expr)) {
    this->addExpr(update->target, place);
  }
}

void PlacedNodes::addOperatorParts(const Expr *expr, const TreePlace &place) {
  if (auto *unary = dyn_cast<UnaryExpr>(expr)) {
    this->addExpr(unary->operand, place);
  }
  if (auto *binary = dyn_cast<BinaryExpr>(expr)) {
    this->addExpr(binary->lhs, place);
    this->addExpr(binary->rhs, place);
  }
  if (auto *choice = dyn_cast<ConditionalExpr>(expr)) {
    this->addExpr(choice->cond, place);
    this->addExpr(choice->thenExpr, place);
    this->addExpr(choice->elseExpr, place);
  }
  if (auto *conversion = dyn_cast<CastExpr>(expr)) {
    this->addExpr(conversion->operand, place);
  }
  if (auto *chain = dyn_cast<OptionalChainExpr>(expr)) {
    this->addExpr(chain->chain, place);
  }
  if (auto *wait = dyn_cast<AwaitExpr>(expr)) {
    this->addExpr(wait->value, place);
  }
  if (auto *yield = dyn_cast<YieldExpr>(expr)) {
    this->addExpr(yield->value, place);
  }
}

void PlacedNodes::addListParts(const Expr *expr, const TreePlace &place) {
  if (auto *sequence = dyn_cast<SequenceExpr>(expr)) {
    this->addExprs(sequence->exprs, place);
  }
  if (auto *text = dyn_cast<TemplateExpr>(expr)) {
    this->addExprs(text->exprs, place);
  }
  if (auto *list = dyn_cast<ArrayLitExpr>(expr)) {
    for (const ArrayLitExpr::Element &element : list->elements) {
      this->addExpr(element.expr, place);
    }
  }
  if (auto *object = dyn_cast<ObjectLitExpr>(expr)) {
    for (const ObjectLitExpr::Property &property : object->props) {
      this->addExpr(property.key(), place);
      this->addExpr(property.value, place);
    }
  }
}

void PlacedNodes::addNestedCodeParts(const Expr *expr, const TreePlace &place) {
  if (auto *function = dyn_cast<FunctionExpr>(expr)) {
    this->addStmt(function->fn, place);
  }
  if (auto *cls = dyn_cast<ClassExpr>(expr)) {
    this->addStmt(cls->decl, place);
  }
}

void PlacedNodes::addExprs(llvm::ArrayRef<Expr *> list, const TreePlace &place) {
  for (const Expr *expr : list) {
    this->addExpr(expr, place);
  }
}

void PlacedNodes::addClassChildren(const ClassDecl *cls, const TreePlace &place) {
  this->addExpr(cls->baseExpr, place);
  for (const Decorator &decorator : cls->decorators) {
    this->addExpr(decorator.expr, place);
  }
  for (const ClassField &field : cls->fields) {
    this->addExpr(field.init, place);
    for (const Decorator &decorator : field.decorators) {
      this->addExpr(decorator.expr, place);
    }
  }
  for (const Stmt *decoration : cls->decorations) {
    this->addStmt(decoration, place);
  }
  for (const FunctionDecl *method : cls->methods) {
    this->addStmt(method, place);
  }
  for (const FunctionDecl *constructor : cls->constructors) {
    this->addStmt(constructor, place);
  }
}

void PlacedNodes::addFunctionChildren(const FunctionDecl *fn, const TreePlace &place) {
  TreePlace inside = place.enterFunction();
  for (const Decorator &decorator : fn->decorators()) {
    this->addExpr(decorator.expr, place);
  }
  for (const Param &param : fn->params) {
    this->addExpr(param.defaultValue(), inside);
    for (const Decorator &decorator : param.decorators) {
      this->addExpr(decorator.expr, place);
    }
  }
  this->addStmt(fn->body, inside);
}

}
