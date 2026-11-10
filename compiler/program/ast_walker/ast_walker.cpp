#include "compiler/program/ast_walker/ast_walker.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;

namespace tz {

void AstWalker::collectNames(const Stmt *stmt, llvm::StringSet<> &names, bool inFunction) {
  AstWalker::namesFrom(NamePart{stmt, nullptr, inFunction}, names);
}

void AstWalker::collectNames(const Expr *expr, llvm::StringSet<> &names, bool inFunction) {
  AstWalker::namesFrom(NamePart{nullptr, expr, inFunction}, names);
}

void AstWalker::namesFrom(NamePart root, llvm::StringSet<> &names) {
  std::vector<NamePart> stack = {root};
  while (!stack.empty()) {
    NamePart part = stack.back();
    stack.pop_back();
    if (part.expr != nullptr) {
      AstWalker::addReadName(part, names);
      AstWalker::addNameChildren(part.expr, part.inFunction, stack);
    } else if (part.stmt != nullptr) {
      AstWalker::addNameChildren(part.stmt, part.inFunction, stack);
    }
  }
}

void AstWalker::addReadName(const NamePart &part, llvm::StringSet<> &names) {
  if (!part.inFunction) {
    return;
  }
  auto *name = dyn_cast<NameExpr>(part.expr);
  if (name != nullptr) {
    names.insert(name->name);
  }
  auto *create = dyn_cast<NewExpr>(part.expr);
  if (create != nullptr && !create->name.empty()) {
    names.insert(create->name);
  }
}

void AstWalker::addNameChildren(const Stmt *stmt, bool inFunction, std::vector<NamePart> &stack) {
  auto addExpr = [&](const Expr *e) { stack.push_back(NamePart{nullptr, e, inFunction}); };
  auto addStmt = [&](const Stmt *s) { stack.push_back(NamePart{s, nullptr, inFunction}); };
  switch (stmt->kind()) {
    case Stmt::Kind::Block:
      for (const Stmt *s : cast<BlockStmt>(stmt)->body) {
        addStmt(s);
      }
      break;
    case Stmt::Kind::VarDecl:
      for (const VarDeclarator &d : cast<VarDeclStmt>(stmt)->decls) {
        addExpr(d.init);
      }
      break;
    case Stmt::Kind::Function: {
      auto *fn = cast<FunctionDecl>(stmt);
      for (const Param &param : fn->params) {
        stack.push_back(NamePart{nullptr, param.defaultValue(), true});
      }
      stack.push_back(NamePart{fn->body, nullptr, true});
      break;
    }
    case Stmt::Kind::Expr:
      addExpr(cast<ExprStmt>(stmt)->expr);
      break;
    case Stmt::Kind::If: {
      auto *s = cast<IfStmt>(stmt);
      addExpr(s->cond);
      addStmt(s->thenStmt);
      addStmt(s->elseStmt);
      break;
    }
    case Stmt::Kind::While:
      addExpr(cast<WhileStmt>(stmt)->cond);
      addStmt(cast<WhileStmt>(stmt)->body);
      break;
    case Stmt::Kind::DoWhile:
      addExpr(cast<DoWhileStmt>(stmt)->cond);
      addStmt(cast<DoWhileStmt>(stmt)->body);
      break;
    case Stmt::Kind::For: {
      auto *s = cast<ForStmt>(stmt);
      addStmt(s->init);
      addExpr(s->cond);
      addExpr(s->step);
      addStmt(s->body);
      break;
    }
    case Stmt::Kind::ForOf:
      addExpr(cast<ForOfStmt>(stmt)->iterable);
      addStmt(cast<ForOfStmt>(stmt)->body);
      break;
    case Stmt::Kind::Switch: {
      auto *s = cast<SwitchStmt>(stmt);
      addExpr(s->discriminant);
      for (const SwitchCase &c : s->cases) {
        addExpr(c.test);
        for (const Stmt *body : c.body) {
          addStmt(body);
        }
      }
      break;
    }
    case Stmt::Kind::Labeled:
      addStmt(cast<LabeledStmt>(stmt)->body);
      break;
    case Stmt::Kind::Return:
      addExpr(cast<ReturnStmt>(stmt)->value);
      break;
    case Stmt::Kind::Class: {
      auto *cls = cast<ClassDecl>(stmt);
      addExpr(cls->baseExpr);
      for (const FunctionDecl *fn : cls->methods) {
        stack.push_back(NamePart{fn, nullptr, true});
      }
      for (const FunctionDecl *fn : cls->constructors) {
        stack.push_back(NamePart{fn, nullptr, true});
      }
      for (const ClassField &f : cls->fields) {
        bool fieldInFunction = f.isStatic() ? inFunction : true;
        stack.push_back(NamePart{nullptr, f.init, fieldInFunction});
      }
      break;
    }
    case Stmt::Kind::Break:
    case Stmt::Kind::Continue:
    case Stmt::Kind::Empty:
    case Stmt::Kind::TypeAlias:
    case Stmt::Kind::Enum:
    case Stmt::Kind::Debugger:
      break;
  }
}

void AstWalker::addNameChildren(const Expr *expr, bool inFunction, std::vector<NamePart> &stack) {
  auto addExpr = [&](const Expr *e) { stack.push_back(NamePart{nullptr, e, inFunction}); };
  switch (expr->kind()) {
    case Expr::Kind::Template:
      for (const Expr *e : cast<TemplateExpr>(expr)->exprs) {
        addExpr(e);
      }
      break;
    case Expr::Kind::ArrayLit:
      for (const ArrayLitExpr::Element &e : cast<ArrayLitExpr>(expr)->elements) {
        addExpr(e.expr);
      }
      break;
    case Expr::Kind::ObjectLit:
      for (const ObjectLitExpr::Property &p : cast<ObjectLitExpr>(expr)->props) {
        addExpr(p.key());
        addExpr(p.value);
      }
      break;
    case Expr::Kind::Member:
      addExpr(cast<MemberExpr>(expr)->object);
      break;
    case Expr::Kind::Index:
      addExpr(cast<IndexExpr>(expr)->object);
      addExpr(cast<IndexExpr>(expr)->index);
      break;
    case Expr::Kind::Unary:
      addExpr(cast<UnaryExpr>(expr)->operand);
      break;
    case Expr::Kind::Binary:
      addExpr(cast<BinaryExpr>(expr)->lhs);
      addExpr(cast<BinaryExpr>(expr)->rhs);
      break;
    case Expr::Kind::Assign:
      addExpr(cast<AssignExpr>(expr)->target);
      addExpr(cast<AssignExpr>(expr)->value);
      break;
    case Expr::Kind::Update:
      addExpr(cast<UpdateExpr>(expr)->target);
      break;
    case Expr::Kind::Call:
      addExpr(cast<CallExpr>(expr)->callee);
      for (const Expr *arg : cast<CallExpr>(expr)->args) {
        addExpr(arg);
      }
      break;
    case Expr::Kind::New: {
      auto *create = cast<NewExpr>(expr);
      addExpr(create->callee);
      for (const Expr *arg : create->ctor() != nullptr ? create->ctor()->args : create->args) {
        addExpr(arg);
      }
      break;
    }
    case Expr::Kind::Conditional:
      addExpr(cast<ConditionalExpr>(expr)->cond);
      addExpr(cast<ConditionalExpr>(expr)->thenExpr);
      addExpr(cast<ConditionalExpr>(expr)->elseExpr);
      break;
    case Expr::Kind::Cast:
      addExpr(cast<CastExpr>(expr)->operand);
      break;
    case Expr::Kind::Sequence:
      for (const Expr *e : cast<SequenceExpr>(expr)->exprs) {
        addExpr(e);
      }
      break;
    case Expr::Kind::OptionalChain:
      addExpr(cast<OptionalChainExpr>(expr)->chain);
      break;
    case Expr::Kind::Function:
      stack.push_back(NamePart{cast<FunctionExpr>(expr)->fn, nullptr, true});
      break;
    case Expr::Kind::Yield:
      addExpr(cast<YieldExpr>(expr)->value);
      break;
    case Expr::Kind::Await:
      addExpr(cast<AwaitExpr>(expr)->value);
      break;
    case Expr::Kind::Class:
      stack.push_back(NamePart{cast<ClassExpr>(expr)->decl, nullptr, inFunction});
      break;
    default:
      break;
  }
}

std::vector<const Expr *> AstWalker::exprsIn(const Stmt *stmt, bool intoFunctions) {
  return AstWalker::exprsFrom(AstPart{stmt, nullptr}, intoFunctions);
}

std::vector<const Expr *> AstWalker::exprsIn(const Expr *expr, bool intoFunctions) {
  return AstWalker::exprsFrom(AstPart{nullptr, expr}, intoFunctions);
}

std::vector<const Expr *> AstWalker::exprsFrom(AstPart root, bool intoFunctions) {
  std::vector<const Expr *> exprs;
  std::vector<AstPart> stack = {root};
  std::vector<AstPart> children;
  while (!stack.empty()) {
    AstPart part = stack.back();
    stack.pop_back();
    children.clear();
    if (part.expr != nullptr) {
      exprs.push_back(part.expr);
      AstWalker::addExprChildren(part.expr, intoFunctions, children);
    } else if (part.stmt != nullptr) {
      AstWalker::addStmtChildren(part.stmt, intoFunctions, children);
    }
    stack.insert(stack.end(), children.rbegin(), children.rend());
  }

  return exprs;
}

void AstWalker::addExprChildren(const Expr *expr, bool intoFunctions,
                                std::vector<AstPart> &children) {
  auto addExpr = [&](const Expr *e) { children.push_back(AstPart{nullptr, e}); };
  switch (expr->kind()) {
    case Expr::Kind::Template:
      for (const Expr *e : cast<TemplateExpr>(expr)->exprs) {
        addExpr(e);
      }
      break;
    case Expr::Kind::ArrayLit:
      for (const ArrayLitExpr::Element &e : cast<ArrayLitExpr>(expr)->elements) {
        addExpr(e.expr);
      }
      break;
    case Expr::Kind::ObjectLit:
      for (const ObjectLitExpr::Property &p : cast<ObjectLitExpr>(expr)->props) {
        addExpr(p.key());
        addExpr(p.value);
      }
      break;
    case Expr::Kind::Member:
      addExpr(cast<MemberExpr>(expr)->object);
      break;
    case Expr::Kind::Index:
      addExpr(cast<IndexExpr>(expr)->object);
      addExpr(cast<IndexExpr>(expr)->index);
      break;
    case Expr::Kind::Unary:
      addExpr(cast<UnaryExpr>(expr)->operand);
      break;
    case Expr::Kind::Binary:
      addExpr(cast<BinaryExpr>(expr)->lhs);
      addExpr(cast<BinaryExpr>(expr)->rhs);
      break;
    case Expr::Kind::Assign:
      addExpr(cast<AssignExpr>(expr)->target);
      addExpr(cast<AssignExpr>(expr)->value);
      break;
    case Expr::Kind::Update:
      addExpr(cast<UpdateExpr>(expr)->target);
      break;
    case Expr::Kind::Call:
      addExpr(cast<CallExpr>(expr)->callee);
      for (const Expr *arg : cast<CallExpr>(expr)->args) {
        addExpr(arg);
      }
      break;
    case Expr::Kind::New: {
      auto *create = cast<NewExpr>(expr);
      addExpr(create->callee);
      for (const Expr *arg : create->ctor() != nullptr ? create->ctor()->args : create->args) {
        addExpr(arg);
      }
      break;
    }
    case Expr::Kind::Conditional:
      addExpr(cast<ConditionalExpr>(expr)->cond);
      addExpr(cast<ConditionalExpr>(expr)->thenExpr);
      addExpr(cast<ConditionalExpr>(expr)->elseExpr);
      break;
    case Expr::Kind::Cast:
      addExpr(cast<CastExpr>(expr)->operand);
      break;
    case Expr::Kind::Sequence:
      for (const Expr *e : cast<SequenceExpr>(expr)->exprs) {
        addExpr(e);
      }
      break;
    case Expr::Kind::OptionalChain:
      addExpr(cast<OptionalChainExpr>(expr)->chain);
      break;
    case Expr::Kind::Function:
      if (intoFunctions) {
        children.push_back(AstPart{cast<FunctionExpr>(expr)->fn, nullptr});
      }
      break;
    case Expr::Kind::Yield:
      addExpr(cast<YieldExpr>(expr)->value);
      break;
    case Expr::Kind::Await:
      addExpr(cast<AwaitExpr>(expr)->value);
      break;
    case Expr::Kind::Class:
      children.push_back(AstPart{cast<ClassExpr>(expr)->decl, nullptr});
      break;
    default:
      break;
  }
}

void AstWalker::addStmtChildren(const Stmt *stmt, bool intoFunctions,
                                std::vector<AstPart> &children) {
  auto addExpr = [&](const Expr *e) { children.push_back(AstPart{nullptr, e}); };
  auto addStmt = [&](const Stmt *s) { children.push_back(AstPart{s, nullptr}); };
  switch (stmt->kind()) {
    case Stmt::Kind::Block:
      for (const Stmt *s : cast<BlockStmt>(stmt)->body) {
        addStmt(s);
      }
      break;
    case Stmt::Kind::VarDecl:
      for (const VarDeclarator &d : cast<VarDeclStmt>(stmt)->decls) {
        addExpr(d.init);
      }
      break;
    case Stmt::Kind::Function: {
      if (!intoFunctions) {
        break;
      }
      auto *fn = cast<FunctionDecl>(stmt);
      for (const Param &param : fn->params) {
        addExpr(param.defaultValue());
      }
      addStmt(fn->body);
      break;
    }
    case Stmt::Kind::Expr:
      addExpr(cast<ExprStmt>(stmt)->expr);
      break;
    case Stmt::Kind::If: {
      auto *s = cast<IfStmt>(stmt);
      addExpr(s->cond);
      addStmt(s->thenStmt);
      addStmt(s->elseStmt);
      break;
    }
    case Stmt::Kind::While:
      addExpr(cast<WhileStmt>(stmt)->cond);
      addStmt(cast<WhileStmt>(stmt)->body);
      break;
    case Stmt::Kind::DoWhile:
      addStmt(cast<DoWhileStmt>(stmt)->body);
      addExpr(cast<DoWhileStmt>(stmt)->cond);
      break;
    case Stmt::Kind::For: {
      auto *s = cast<ForStmt>(stmt);
      addStmt(s->init);
      addExpr(s->cond);
      addExpr(s->step);
      addStmt(s->body);
      break;
    }
    case Stmt::Kind::ForOf:
      addExpr(cast<ForOfStmt>(stmt)->iterable);
      addStmt(cast<ForOfStmt>(stmt)->body);
      break;
    case Stmt::Kind::Switch: {
      auto *s = cast<SwitchStmt>(stmt);
      addExpr(s->discriminant);
      for (const SwitchCase &c : s->cases) {
        addExpr(c.test);
        for (const Stmt *body : c.body) {
          addStmt(body);
        }
      }
      break;
    }
    case Stmt::Kind::Labeled:
      addStmt(cast<LabeledStmt>(stmt)->body);
      break;
    case Stmt::Kind::Return:
      addExpr(cast<ReturnStmt>(stmt)->value);
      break;
    case Stmt::Kind::Class:
      AstWalker::addClassChildren(cast<ClassDecl>(stmt), intoFunctions, children);
      break;
    default:
      break;
  }
}

void AstWalker::addClassChildren(const ClassDecl *cls, bool intoFunctions,
                                 std::vector<AstPart> &children) {
  auto addExpr = [&](const Expr *e) { children.push_back(AstPart{nullptr, e}); };
  auto addStmt = [&](const Stmt *s) { children.push_back(AstPart{s, nullptr}); };
  addExpr(cls->baseExpr);
  for (const ClassField &f : cls->fields) {
    if (f.isStatic() || intoFunctions) {
      addExpr(f.init);
    }
  }
  for (const Decorator &d : cls->decorators) {
    addExpr(d.expr);
  }
  for (const ClassField &f : cls->fields) {
    for (const Decorator &d : f.decorators) {
      addExpr(d.expr);
    }
  }
  for (const FunctionDecl *fn : cls->methods) {
    AstWalker::addDecoratorChildren(fn, children);
  }
  for (const FunctionDecl *fn : cls->constructors) {
    AstWalker::addDecoratorChildren(fn, children);
  }
  for (const Stmt *s : cls->decorations) {
    addStmt(s);
  }
  if (!intoFunctions) {
    return;
  }
  for (const FunctionDecl *fn : cls->methods) {
    addStmt(fn);
  }
  for (const FunctionDecl *fn : cls->constructors) {
    addStmt(fn);
  }
}

void AstWalker::addDecoratorChildren(const FunctionDecl *fn, std::vector<AstPart> &children) {
  for (const Decorator &d : fn->decorators()) {
    children.push_back(AstPart{nullptr, d.expr});
  }
  for (const Param &param : fn->params) {
    for (const Decorator &d : param.decorators) {
      children.push_back(AstPart{nullptr, d.expr});
    }
  }
}

std::vector<Expr *> AstWalker::returnValuesIn(Stmt *body) {
  std::vector<Expr *> values;
  std::vector<Stmt *> stack = {body};
  while (!stack.empty()) {
    Stmt *stmt = stack.back();
    stack.pop_back();
    if (stmt == nullptr) {
      continue;
    }
    auto *ret = dyn_cast<ReturnStmt>(stmt);
    if (ret != nullptr && ret->value != nullptr) {
      values.push_back(ret->value);
    }
    std::vector<Stmt *> children = AstWalker::returnScopeChildren(stmt);
    stack.insert(stack.end(), children.rbegin(), children.rend());
  }

  return values;
}

std::vector<Stmt *> AstWalker::returnScopeChildren(Stmt *stmt) {
  switch (stmt->kind()) {
    case Stmt::Kind::Block:
      return cast<BlockStmt>(stmt)->body;
    case Stmt::Kind::If:
      return {cast<IfStmt>(stmt)->thenStmt, cast<IfStmt>(stmt)->elseStmt};
    case Stmt::Kind::While:
      return {cast<WhileStmt>(stmt)->body};
    case Stmt::Kind::DoWhile:
      return {cast<DoWhileStmt>(stmt)->body};
    case Stmt::Kind::For:
      return {cast<ForStmt>(stmt)->body};
    case Stmt::Kind::ForOf:
      return {cast<ForOfStmt>(stmt)->body};
    case Stmt::Kind::Switch: {
      std::vector<Stmt *> children;
      for (SwitchCase &c : cast<SwitchStmt>(stmt)->cases) {
        children.insert(children.end(), c.body.begin(), c.body.end());
      }

      return children;
    }
    case Stmt::Kind::Labeled:
      return {cast<LabeledStmt>(stmt)->body};
    default:
      return {};
  }
}

std::vector<CallExpr *> AstWalker::tailCallsOf(Expr *value) {
  std::vector<CallExpr *> calls;
  std::vector<Expr *> stack = {value};
  while (!stack.empty()) {
    Expr *expr = stack.back();
    stack.pop_back();
    auto *call = dyn_cast<CallExpr>(expr);
    auto *cond = dyn_cast<ConditionalExpr>(expr);
    auto *bin = dyn_cast<BinaryExpr>(expr);
    auto *seq = dyn_cast<SequenceExpr>(expr);
    if (call != nullptr) {
      if (call->func() != nullptr && call->builtin() == Builtin::None) {
        calls.push_back(call);
      }
    } else if (cond != nullptr) {
      stack.push_back(cond->elseExpr);
      stack.push_back(cond->thenExpr);
    } else if (bin != nullptr) {
      bool shortCircuit = bin->op == BinaryOp::LogAnd || bin->op == BinaryOp::LogOr ||
                          bin->op == BinaryOp::Nullish;
      if (shortCircuit) {
        stack.push_back(bin->rhs);
      }
    } else if (seq != nullptr) {
      stack.push_back(seq->exprs.back());
    }
  }

  return calls;
}

std::vector<Stmt *> AstWalker::tailStatementsOf(Stmt *body) {
  std::vector<Stmt *> stmts;
  std::vector<TailPart> stack = {TailPart{body, true}};
  while (!stack.empty()) {
    TailPart part = stack.back();
    stack.pop_back();
    auto *block = dyn_cast<BlockStmt>(part.stmt);
    auto *branch = dyn_cast<IfStmt>(part.stmt);
    if (block != nullptr) {
      for (size_t i = block->body.size(); i > 0; --i) {
        bool tail = AstWalker::isTailAt(block, i - 1, part.tail);
        stack.push_back(TailPart{block->body[i - 1], tail});
      }
    } else if (branch != nullptr) {
      if (branch->elseStmt != nullptr) {
        stack.push_back(TailPart{branch->elseStmt, part.tail});
      }
      stack.push_back(TailPart{branch->thenStmt, part.tail});
    } else if (part.tail && AstWalker::isPlainCallStatement(part.stmt)) {
      stmts.push_back(part.stmt);
    }
  }

  return stmts;
}

bool AstWalker::isTailAt(const BlockStmt *block, size_t i, bool tail) {
  bool last = i + 1 == block->body.size();
  auto *next = last ? nullptr : dyn_cast<ReturnStmt>(block->body[i + 1]);
  bool beforeEmptyReturn = next != nullptr && next->value == nullptr;

  return beforeEmptyReturn || (tail && last);
}

bool AstWalker::isPlainCallStatement(const Stmt *stmt) {
  auto *statement = dyn_cast<ExprStmt>(stmt);
  auto *call = statement != nullptr ? dyn_cast<CallExpr>(statement->expr) : nullptr;

  return call != nullptr && call->func() != nullptr && call->builtin() == Builtin::None;
}

std::vector<CallExpr *> AstWalker::tailCallsIn(FunctionDecl *fn) {
  std::vector<CallExpr *> calls;
  for (Expr *value : AstWalker::returnValuesIn(fn->body)) {
    std::vector<CallExpr *> tails = AstWalker::tailCallsOf(value);
    calls.insert(calls.end(), tails.begin(), tails.end());
  }
  if (fn->sym->getKnownReturnType()->isVoid()) {
    for (Stmt *stmt : AstWalker::tailStatementsOf(fn->body)) {
      calls.push_back(cast<CallExpr>(cast<ExprStmt>(stmt)->expr));
    }
  }

  return calls;
}

}
