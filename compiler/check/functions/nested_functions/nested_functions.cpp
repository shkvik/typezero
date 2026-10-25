#include "compiler/check/functions/nested_functions/nested_functions.h"
#include "compiler/infer/call_cycles/call_cycles.h"
#include "compiler/check/functions/function_checker/function_checker.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/declare/function_names/function_names.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

void visitExprs(const Expr *expr, llvm::function_ref<void(const Expr *)> visit,
                bool intoFunctions) {
  visit(expr);
  auto sub = [&](const Expr *e) { visitExprs(e, visit, intoFunctions); };
  switch (expr->kind()) {
  case Expr::Kind::Template:
    for (const Expr *e : cast<TemplateExpr>(expr)->exprs) {
      sub(e);
    }
    break;
  case Expr::Kind::ArrayLit:
    for (const ArrayLitExpr::Element &e : cast<ArrayLitExpr>(expr)->elements) {
      sub(e.expr);
    }
    break;
  case Expr::Kind::ObjectLit:
    for (const ObjectLitExpr::Property &p : cast<ObjectLitExpr>(expr)->props) {
      if (p.key) {
        sub(p.key);
      }
      sub(p.value);
    }
    break;
  case Expr::Kind::Member:
    sub(cast<MemberExpr>(expr)->object);
    break;
  case Expr::Kind::Index:
    sub(cast<IndexExpr>(expr)->object);
    sub(cast<IndexExpr>(expr)->index);
    break;
  case Expr::Kind::Unary:
    sub(cast<UnaryExpr>(expr)->operand);
    break;
  case Expr::Kind::Binary:
    sub(cast<BinaryExpr>(expr)->lhs);
    sub(cast<BinaryExpr>(expr)->rhs);
    break;
  case Expr::Kind::Assign:
    sub(cast<AssignExpr>(expr)->target);
    sub(cast<AssignExpr>(expr)->value);
    break;
  case Expr::Kind::Update:
    sub(cast<UpdateExpr>(expr)->target);
    break;
  case Expr::Kind::Call:
    sub(cast<CallExpr>(expr)->callee);
    for (const Expr *arg : cast<CallExpr>(expr)->args) {
      sub(arg);
    }
    break;
  case Expr::Kind::New: {
    // Once checked, the arguments of a class's constructor are in its call.
    auto *create = cast<NewExpr>(expr);
    if (create->callee) {
      sub(create->callee);
    }
    for (const Expr *arg : create->ctor ? create->ctor->args : create->args) {
      sub(arg);
    }
    break;
  }
  case Expr::Kind::Conditional:
    sub(cast<ConditionalExpr>(expr)->cond);
    sub(cast<ConditionalExpr>(expr)->thenExpr);
    sub(cast<ConditionalExpr>(expr)->elseExpr);
    break;
  case Expr::Kind::Cast:
    sub(cast<CastExpr>(expr)->operand);
    break;
  case Expr::Kind::Sequence:
    for (const Expr *e : cast<SequenceExpr>(expr)->exprs) {
      sub(e);
    }
    break;
  case Expr::Kind::OptionalChain:
    sub(cast<OptionalChainExpr>(expr)->chain);
    break;
  case Expr::Kind::Function:
    if (intoFunctions) {
      visitExprs(cast<FunctionExpr>(expr)->fn, visit);
    }
    break;
  case Expr::Kind::Yield:
    if (const Expr *value = cast<YieldExpr>(expr)->value) {
      sub(value);
    }
    break;
  case Expr::Kind::Await:
    sub(cast<AwaitExpr>(expr)->value);
    break;
  case Expr::Kind::Class: // as a declaration of the class
    visitExprs(cast<ClassExpr>(expr)->decl, visit, intoFunctions);
    break;
  default:
    break;
  }
}

void visitExprs(const Stmt *stmt, llvm::function_ref<void(const Expr *)> visit,
                bool intoFunctions) {
  if (!stmt) {
    return;
  }
  auto expr = [&](const Expr *e) {
    if (e) {
      visitExprs(e, visit, intoFunctions);
    }
  };
  auto sub = [&](const Stmt *s) { visitExprs(s, visit, intoFunctions); };
  switch (stmt->kind()) {
  case Stmt::Kind::Block:
    for (const Stmt *s : cast<BlockStmt>(stmt)->body) {
      sub(s);
    }
    break;
  case Stmt::Kind::VarDecl:
    for (const VarDeclarator &d : cast<VarDeclStmt>(stmt)->decls) {
      expr(d.init);
    }
    break;
  case Stmt::Kind::Function: {
    if (!intoFunctions) {
      break;
    }
    auto *fn = cast<FunctionDecl>(stmt);
    for (const Param &param : fn->params) {
      expr(param.defaultValue);
    }
    sub(fn->body);
    break;
  }
  case Stmt::Kind::Expr:
    expr(cast<ExprStmt>(stmt)->expr);
    break;
  case Stmt::Kind::If: {
    auto *s = cast<IfStmt>(stmt);
    expr(s->cond);
    sub(s->thenStmt);
    sub(s->elseStmt);
    break;
  }
  case Stmt::Kind::While:
    expr(cast<WhileStmt>(stmt)->cond);
    sub(cast<WhileStmt>(stmt)->body);
    break;
  case Stmt::Kind::DoWhile:
    sub(cast<DoWhileStmt>(stmt)->body);
    expr(cast<DoWhileStmt>(stmt)->cond);
    break;
  case Stmt::Kind::For: {
    auto *s = cast<ForStmt>(stmt);
    sub(s->init);
    expr(s->cond);
    expr(s->step);
    sub(s->body);
    break;
  }
  case Stmt::Kind::ForOf:
    expr(cast<ForOfStmt>(stmt)->iterable);
    sub(cast<ForOfStmt>(stmt)->body);
    break;
  case Stmt::Kind::Switch: {
    auto *s = cast<SwitchStmt>(stmt);
    expr(s->discriminant);
    for (const SwitchCase &c : s->cases) {
      expr(c.test);
      for (const Stmt *body : c.body) {
        sub(body);
      }
    }
    break;
  }
  case Stmt::Kind::Labeled:
    sub(cast<LabeledStmt>(stmt)->body);
    break;
  case Stmt::Kind::Return:
    expr(cast<ReturnStmt>(stmt)->value);
    break;
  case Stmt::Kind::Throw:
    expr(cast<ThrowStmt>(stmt)->value);
    break;
  case Stmt::Kind::Try: {
    auto *s = cast<TryStmt>(stmt);
    sub(s->block);
    sub(s->handler);
    sub(s->finalizer);
    break;
  }
  case Stmt::Kind::Class: {
    // The static initializers run where the class is declared, and so does
    // a base that is a value; the rest is the code of functions.
    auto *cls = cast<ClassDecl>(stmt);
    expr(cls->baseExpr);
    for (const ClassField &f : cls->fields) {
      if (f.init && (f.isStatic || intoFunctions)) {
        expr(f.init);
      }
    }
    // So do the decorators, and what applies them (Sema::decorateClass).
    for (const Decorator &d : cls->decorators) {
      expr(d.expr);
    }
    for (const ClassField &f : cls->fields) {
      for (const Decorator &d : f.decorators) {
        expr(d.expr);
      }
    }
    for (const auto *fns : {&cls->methods, &cls->constructors}) {
      for (const FunctionDecl *fn : *fns) {
        for (const Decorator &d : fn->decorators) {
          expr(d.expr);
        }
        for (const Param &param : fn->params) {
          for (const Decorator &d : param.decorators) {
            expr(d.expr);
          }
        }
      }
    }
    for (const Stmt *s : cls->decorations) {
      sub(s);
    }
    if (!intoFunctions) {
      break;
    }
    for (const FunctionDecl *fn : cls->methods) {
      sub(fn);
    }
    for (const FunctionDecl *fn : cls->constructors) {
      sub(fn);
    }
    break;
  }
  default:
    break;
  }
}

namespace {

// The names `stmt` assigns to: `x = v`, `x += v`, `x++`...
void collectAssigned(const Stmt *stmt, std::vector<llvm::StringRef> &names) {
  visitExprs(stmt, [&](const Expr *e) {
    const Expr *target = nullptr;
    if (auto *assign = dyn_cast<AssignExpr>(e)) {
      target = assign->target;
    } else if (auto *update = dyn_cast<UpdateExpr>(e)) {
      target = update->target;
    }
    if (auto *name = llvm::dyn_cast_or_null<NameExpr>(target)) {
      if (!llvm::is_contained(names, name->name)) {
        names.push_back(name->name);
      }
    }
  });
}

} // namespace

NestedFunctions::NestedFunctions(Sema &sema) : sema_(sema) {}

NestedGroup *NestedFunctions::declareNested(BlockStmt *block) {
  std::vector<FunctionDecl *> members;
  for (Stmt *stmt : block->body) {
    if (auto *fn = dyn_cast<FunctionDecl>(stmt)) {
      members.push_back(fn);
    }
  }
  if (members.empty()) {
    return nullptr;
  }
  NestedGroup *group = sema_.ctx_.newGroup();
  block->group = group;
  group->owner = sema_.currentDecl();
  group->scopeDepth = sema_.scopes_.size();
  group->functionDepth = sema_.functions_.size();
  llvm::StringSet<> names;
  for (FunctionDecl *fn : members) {
    fn->group = group;
    fn->parent = sema_.currentDecl();
    group->members.push_back(fn);
    names.insert(fn->name);
    VarSymbol *var = sema_.ctx_.newVar();
    var->name = fn->name;
    var->loc = fn->loc();
    var->isConst = true;
    var->owner = sema_.currentDecl();
    var->function = fn;
    fn->var = var;
    sema_.functionChecker().declareFunction(fn, var);
    collectAssigned(fn->body, group->assignedNames);
  }
  for (Stmt *stmt : block->body) {
    if (auto *decl = dyn_cast<VarDeclStmt>(stmt)) {
      for (const VarDeclarator &d : decl->decls) {
        group->declaredNames.push_back(d.name);
      }
    }
  }
  // The functions are created before the first statement that mentions
  // them, so that they can copy the variables declared before it.
  for (size_t i = 0; i < block->body.size() && group->firstUse == ~size_t(0); ++i) {
    if (isa<FunctionDecl>(block->body[i])) {
      continue;
    }
    llvm::StringSet<> used;
    collectNames(block->body[i], used);
    if (llvm::any_of(used, [&](const auto &entry) { return names.contains(entry.getKey()); })) {
      group->firstUse = i;
    }
  }
  sema_.callCycles().findCycles(group->members);

  return group;
}

void NestedFunctions::finishGroup(NestedGroup *group) {
  // Every variable of the block is declared now.
  group->finished = true;
  for (FunctionDecl *fn : group->members) {
    if (!fn->sym->ret) {
      sema_.returnInferrer().inferReturnType(fn->sym);
    }
    if (fn->checked) {
      continue;
    }
    Sema::BodyState saved = sema_.returnInferrer().enterBody(fn);
    sema_.functionChecker().checkFunctionBody(fn);
    sema_.returnInferrer().leaveBody(saved);
  }

  // A function the block calls (or passes on) before a variable it uses is
  // declared: JavaScript would throw a ReferenceError when it reads it.
  auto declaredAfter = [&](const NestedGroup::Use &use) -> VarSymbol * {
    std::vector<FunctionDecl *> reached{use.fn};
    for (size_t i = 0; i < reached.size(); ++i) {
      for (FunctionDecl *sibling : reached[i]->siblings) {
        if (!llvm::is_contained(reached, sibling)) {
          reached.push_back(sibling);
        }
      }
    }
    for (FunctionDecl *fn : reached) {
      for (VarSymbol *var : fn->captures) {
        if (var->owner == group->owner && var->declSeq > use.seq) {
          return var;
        }
      }
    }

    return nullptr;
  };
  for (const NestedGroup::Use &use : group->uses) {
    if (VarSymbol *var = declaredAfter(use)) {
      sema_.diags_.error(use.loc,
                   "'" + use.fn->name + "' uses '" + var->name +
                       "' before it is declared: JavaScript would throw a ReferenceError when it "
                       "runs here; move the declaration of '" +
                       var->name + "' up");
      sema_.diags_.note(var->loc, "'" + var->name + "' is declared here");
    }
  }
}

void NestedFunctions::noteUse(VarSymbol *var, llvm::SMLoc loc) {
  if (var->isGlobal) {
    return;
  }
  // The block's own code uses one of its functions: it runs, or may run,
  // from here (see finishGroup).
  if (FunctionDecl *member = var->function; member && sema_.currentDecl() == member->group->owner) {
    member->group->uses.push_back({member, sema_.seq_, loc});
  }
  for (size_t i = sema_.functions_.size(); i-- > 0;) {
    FunctionDecl *fn = sema_.functions_[i];
    // Its own variables, and the function itself through its name.
    if (fn == var->owner || fn == var->lambda) {
      break;
    }
    // The functions of a group share their environment: they reach each
    // other through it.
    if (var->function && fn->group && fn->group == var->function->group) {
      if (fn != var->function && !llvm::is_contained(fn->siblings, var->function)) {
        fn->siblings.push_back(var->function);
      }
      break;
    }
    if (!llvm::is_contained(fn->captures, var)) {
      fn->captures.push_back(var);
    }
    if (fn->group && !llvm::is_contained(fn->group->captures, var)) {
      fn->group->captures.push_back(var);
    }
    var->captured = true;
    unsigned created = fn->group ? fn->group->creationSeq : fn->creationSeq;
    if (created <= var->declSeq) {
      var->capturedEarly = true;
    }
  }
}

FunctionDecl *NestedFunctions::namedFunction(const Expr *expr) const {
  auto *name = dyn_cast<NameExpr>(expr);
  if (!name) {
    return nullptr;
  }
  if (name->func) {
    return name->func->decl;
  }
  if (VarSymbol *var = name->var) {
    return var->function ? var->function : var->isConst ? var->lambda : nullptr;
  }

  return nullptr;
}

} // namespace tz
