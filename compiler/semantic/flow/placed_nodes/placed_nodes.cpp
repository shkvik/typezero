#include "compiler/semantic/flow/placed_nodes/placed_nodes.h"

using llvm::dyn_cast;

namespace tz {

PlacedNodes PlacedNodes::collect(llvm::ArrayRef<const Stmt *> stmts,
                                 llvm::ArrayRef<const Expr *> exprs) {
  auto nodes = PlacedNodes();
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
  auto *block = dyn_cast<BlockStmt>(stmt);
  if (block != nullptr) {
    for (const Stmt *inner : block->body) {
      this->addStmt(inner, place);
    }
  }
  auto *statement = dyn_cast<ExprStmt>(stmt);
  if (statement != nullptr) {
    this->addExpr(statement->expr, place);
  }
  auto *result = dyn_cast<ReturnStmt>(stmt);
  if (result != nullptr) {
    this->addExpr(result->value, place);
  }
}

void PlacedNodes::addLoopParts(const Stmt *stmt, const TreePlace &place) {
  auto inside = place.enterLoop();
  auto *whileLoop = dyn_cast<WhileStmt>(stmt);
  auto *doLoop = dyn_cast<DoWhileStmt>(stmt);
  auto *forLoop = dyn_cast<ForStmt>(stmt);
  auto *forOfLoop = dyn_cast<ForOfStmt>(stmt);
  if (whileLoop != nullptr) {
    this->addExpr(whileLoop->cond, inside);
    this->addStmt(whileLoop->body, inside);
  }
  if (doLoop != nullptr) {
    this->addStmt(doLoop->body, inside);
    this->addExpr(doLoop->cond, inside);
  }
  if (forLoop != nullptr) {
    this->addStmt(forLoop->init, inside);
    this->addExpr(forLoop->cond, inside);
    this->addExpr(forLoop->step, inside);
    this->addStmt(forLoop->body, inside);
  }
  if (forOfLoop != nullptr) {
    this->addExpr(forOfLoop->iterable, place);
    this->addStmt(forOfLoop->body, inside);
  }
}

void PlacedNodes::addBranchParts(const Stmt *stmt, const TreePlace &place) {
  auto *ifStmt = dyn_cast<IfStmt>(stmt);
  auto *switchStmt = dyn_cast<SwitchStmt>(stmt);
  auto *labeled = dyn_cast<LabeledStmt>(stmt);
  if (ifStmt != nullptr) {
    this->addExpr(ifStmt->cond, place);
    this->addStmt(ifStmt->thenStmt, place);
    this->addStmt(ifStmt->elseStmt, place);
  }
  if (switchStmt != nullptr) {
    this->addSwitchParts(switchStmt, place);
  }
  if (labeled != nullptr) {
    auto inside = place.enterLabel(labeled->label);
    this->addStmt(labeled->body, inside);
  }
}

void PlacedNodes::addSwitchParts(const SwitchStmt *switchStmt, const TreePlace &place) {
  this->addExpr(switchStmt->discriminant, place);
  for (const SwitchCase &branch : switchStmt->cases) {
    this->addCaseParts(branch, place);
  }
}

void PlacedNodes::addCaseParts(const SwitchCase &branch, const TreePlace &place) {
  auto inside = place.enterSwitch();
  this->addExpr(branch.test, place);
  for (const Stmt *inner : branch.body) {
    this->addStmt(inner, inside);
  }
}

void PlacedNodes::addDeclarationParts(const Stmt *stmt, const TreePlace &place) {
  auto *declaration = dyn_cast<VarDeclStmt>(stmt);
  if (declaration != nullptr) {
    for (const VarDeclarator &declarator : declaration->decls) {
      this->addExpr(declarator.init, place);
    }
  }
  auto *functionDecl = dyn_cast<FunctionDecl>(stmt);
  if (functionDecl != nullptr) {
    this->addFunctionChildren(functionDecl, place);
  }
  auto *classDecl = dyn_cast<ClassDecl>(stmt);
  if (classDecl != nullptr) {
    this->addClassChildren(classDecl, place);
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
  auto *call = dyn_cast<CallExpr>(expr);
  if (call != nullptr) {
    this->addExpr(call->callee, place);
    this->addExprs(call->args, place);
  }
  auto *made = dyn_cast<NewExpr>(expr);
  if (made != nullptr) {
    this->addNewParts(made, place);
  }
}

void PlacedNodes::addNewParts(const NewExpr *made, const TreePlace &place) {
  const CallExpr *constructor = made->ctor();
  llvm::ArrayRef<Expr *> args = constructor != nullptr ? constructor->args : made->args;
  this->addExpr(made->callee, place);
  this->addExprs(args, place);
}

void PlacedNodes::addAccessParts(const Expr *expr, const TreePlace &place) {
  auto *member = dyn_cast<MemberExpr>(expr);
  if (member != nullptr) {
    this->addExpr(member->object, place);
  }
  auto *index = dyn_cast<IndexExpr>(expr);
  if (index != nullptr) {
    this->addExpr(index->object, place);
    this->addExpr(index->index, place);
  }
  auto *assign = dyn_cast<AssignExpr>(expr);
  if (assign != nullptr) {
    this->addExpr(assign->target, place);
    this->addExpr(assign->value, place);
  }
  auto *update = dyn_cast<UpdateExpr>(expr);
  if (update != nullptr) {
    this->addExpr(update->target, place);
  }
}

void PlacedNodes::addOperatorParts(const Expr *expr, const TreePlace &place) {
  auto *unary = dyn_cast<UnaryExpr>(expr);
  if (unary != nullptr) {
    this->addExpr(unary->operand, place);
  }
  auto *binary = dyn_cast<BinaryExpr>(expr);
  if (binary != nullptr) {
    this->addExpr(binary->lhs, place);
    this->addExpr(binary->rhs, place);
  }
  auto *choice = dyn_cast<ConditionalExpr>(expr);
  if (choice != nullptr) {
    this->addExpr(choice->cond, place);
    this->addExpr(choice->thenExpr, place);
    this->addExpr(choice->elseExpr, place);
  }
  auto *conversion = dyn_cast<CastExpr>(expr);
  if (conversion != nullptr) {
    this->addExpr(conversion->operand, place);
  }
  auto *chain = dyn_cast<OptionalChainExpr>(expr);
  if (chain != nullptr) {
    this->addExpr(chain->chain, place);
  }
  auto *awaited = dyn_cast<AwaitExpr>(expr);
  if (awaited != nullptr) {
    this->addExpr(awaited->value, place);
  }
  auto *yield = dyn_cast<YieldExpr>(expr);
  if (yield != nullptr) {
    this->addExpr(yield->value, place);
  }
}

void PlacedNodes::addListParts(const Expr *expr, const TreePlace &place) {
  auto *sequence = dyn_cast<SequenceExpr>(expr);
  if (sequence != nullptr) {
    this->addExprs(sequence->exprs, place);
  }
  auto *text = dyn_cast<TemplateExpr>(expr);
  if (text != nullptr) {
    this->addExprs(text->exprs, place);
  }
  auto *list = dyn_cast<ArrayLitExpr>(expr);
  if (list != nullptr) {
    for (const ArrayLitExpr::Element &element : list->elements) {
      this->addExpr(element.expr, place);
    }
  }
  auto *object = dyn_cast<ObjectLitExpr>(expr);
  if (object != nullptr) {
    for (const ObjectLitExpr::Property &property : object->props) {
      this->addExpr(property.key(), place);
      this->addExpr(property.value, place);
    }
  }
}

void PlacedNodes::addNestedCodeParts(const Expr *expr, const TreePlace &place) {
  auto *functionExpr = dyn_cast<FunctionExpr>(expr);
  if (functionExpr != nullptr) {
    this->addStmt(functionExpr->fn, place);
  }
  auto *classExpr = dyn_cast<ClassExpr>(expr);
  if (classExpr != nullptr) {
    this->addStmt(classExpr->decl, place);
  }
}

void PlacedNodes::addExprs(llvm::ArrayRef<Expr *> list, const TreePlace &place) {
  for (const Expr *expr : list) {
    this->addExpr(expr, place);
  }
}

void PlacedNodes::addClassChildren(const ClassDecl *classDecl, const TreePlace &place) {
  this->addExpr(classDecl->baseExpr, place);
  for (const Decorator &decorator : classDecl->decorators) {
    this->addExpr(decorator.expr, place);
  }
  for (const ClassField &field : classDecl->fields) {
    this->addFieldParts(field, place);
  }
  for (const Stmt *decoration : classDecl->decorations) {
    this->addStmt(decoration, place);
  }
  for (const FunctionDecl *method : classDecl->methods) {
    this->addStmt(method, place);
  }
  for (const FunctionDecl *constructor : classDecl->constructors) {
    this->addStmt(constructor, place);
  }
}

void PlacedNodes::addFieldParts(const ClassField &field, const TreePlace &place) {
  this->addExpr(field.init, place);
  for (const Decorator &decorator : field.decorators) {
    this->addExpr(decorator.expr, place);
  }
}

void PlacedNodes::addFunctionChildren(const FunctionDecl *functionDecl, const TreePlace &place) {
  auto inside = place.enterFunction();
  for (const Decorator &decorator : functionDecl->decorators()) {
    this->addExpr(decorator.expr, place);
  }
  for (const Param &param : functionDecl->params) {
    this->addParamParts(param, place);
  }
  this->addStmt(functionDecl->body, inside);
}

void PlacedNodes::addParamParts(const Param &param, const TreePlace &place) {
  auto inside = place.enterFunction();
  this->addExpr(param.defaultValue(), inside);
  for (const Decorator &decorator : param.decorators) {
    this->addExpr(decorator.expr, place);
  }
}

}
