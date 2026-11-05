#include "compiler/library/held_values/held_value_checker/held_value_checker.h"
#include "compiler/parallel/parallel_helpers/parallel_helpers.h"
#include "compiler/program/module/module.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

void children(const Expr *e, const std::function<void(const Expr *)> &visit) {
  switch (e->kind()) {
  case Expr::Kind::Template:
    for (const Expr *x : cast<TemplateExpr>(e)->exprs) {
      visit(x);
    }
    break;
  case Expr::Kind::Member:
    visit(cast<MemberExpr>(e)->object);
    break;
  case Expr::Kind::Index:
    visit(cast<IndexExpr>(e)->object);
    visit(cast<IndexExpr>(e)->index);
    break;
  case Expr::Kind::Unary:
    visit(cast<UnaryExpr>(e)->operand);
    break;
  case Expr::Kind::Binary:
    visit(cast<BinaryExpr>(e)->lhs);
    visit(cast<BinaryExpr>(e)->rhs);
    break;
  case Expr::Kind::Update:
    visit(cast<UpdateExpr>(e)->target);
    break;
  case Expr::Kind::Conditional:
    visit(cast<ConditionalExpr>(e)->cond);
    visit(cast<ConditionalExpr>(e)->thenExpr);
    visit(cast<ConditionalExpr>(e)->elseExpr);
    break;
  case Expr::Kind::Cast:
    visit(cast<CastExpr>(e)->operand);
    break;
  case Expr::Kind::Sequence:
    for (const Expr *x : cast<SequenceExpr>(e)->exprs) {
      visit(x);
    }
    break;
  case Expr::Kind::OptionalChain:
    visit(cast<OptionalChainExpr>(e)->chain);
    break;
  case Expr::Kind::Yield:
    visit(cast<YieldExpr>(e)->value);
    break;
  case Expr::Kind::Await:
    visit(cast<AwaitExpr>(e)->value);
    break;
  default:
    break;
  }
}

std::string calleeName(const CallExpr *c) {
  const Expr *callee = c->callee;
  if (auto *member = dyn_cast_or_null<MemberExpr>(callee)) {
    return member->name.str();
  }
  if (auto *name = dyn_cast_or_null<NameExpr>(callee)) {
    return name->name.str();
  }

  return "";
}

}

HeldValueChecker::HeldValueChecker(Program &program) : program_(program) {}

bool HeldValueChecker::produces(const CallExpr *c) const {
  (void)c;

  return false;
}

bool HeldValueChecker::takesOver(const CallExpr *c) {
  (void)c;

  return false;
}

void HeldValueChecker::sawNew(const NewExpr *n) { (void)n; }

void HeldValueChecker::sawVoid(const CallExpr *c) { (void)c; }

void HeldValueChecker::sawCall(const CallExpr *c) { (void)c; }

bool HeldValueChecker::heldMember(const MemberExpr *m) const {
  (void)m;

  return false;
}

void HeldValueChecker::spreadKept(const Expr *value) { (void)value; }

size_t HeldValueChecker::extraKnown() const { return 0; }

bool HeldValueChecker::closuresCarry() const { return false; }

void HeldValueChecker::finish() {}

void HeldValueChecker::startWalk() {}

bool HeldValueChecker::usesModule(llvm::StringRef name) const {
  return llvm::any_of(program_.file.modules,
                      [&](const Module *m) { return m->builtin && m->name == name; });
}

bool HeldValueChecker::inLibrary(llvm::SMLoc loc) const {
  const char *at = loc.getPointer();

  return llvm::any_of(libraryRanges_, [&](const std::pair<const char *, const char *> &range) {
    return at >= range.first && at < range.second;
  });
}

void HeldValueChecker::run() {
  if (!used()) {
    return;
  }
  for (const Module *module : program_.file.modules) {
    if (!module->builtin) {
      continue;
    }
    for (unsigned id : module->buffers) {
      const llvm::MemoryBuffer *buffer = program_.sourceMgr.getMemoryBuffer(id);
      libraryRanges_.push_back({buffer->getBufferStart(), buffer->getBufferEnd()});
    }
  }
  auto known = [&] {
    return heldVars_.size() + heldParams_.size() + returnsHeld_.size() + heldThis_.size() +
           extraKnown();
  };
  for (int round = 0; round < 32; ++round) {
    size_t before = known();
    walkProgram();
    if (known() == before) {
      break;
    }
  }
  report_ = true;
  walkProgram();
  finish();
}

void HeldValueChecker::walkProgram() {
  walked_.clear();
  callees_.clear();
  allowedNews_.clear();
  awaited_.clear();
  allowedStores_.clear();
  callSites_.clear();
  startWalk();
  SourceFile &file = program_.file;
  for (Stmt *item : file.items) {
    if (auto *fn = dyn_cast<FunctionDecl>(item)) {
      if (fn->typeParams.empty() && !file.unusedFunctions.contains(fn)) {
        function(fn);
      }
      continue;
    }
    if (auto *cls = dyn_cast<ClassDecl>(item)) {
      for (const FunctionDecl *fn : cls->methods) {
        if (fn->typeParams.empty() && (fn->isStatic() ? !cls->generic : cls->isEmitted())) {
          function(fn);
        }
      }
      if (cls->isEmitted()) {
        for (const FunctionDecl *fn : cls->methodInstances) {
          function(fn);
        }
        for (const FunctionDecl *fn : cls->constructors) {
          function(fn);
        }
        function(cls->init);
        for (const ClassField &f : cls->fields) {
          function(f.method);
        }
      }
    }
    topLevel(item);
  }
  for (const FunctionDecl *fn : file.instances) {
    function(fn);
  }
}

void HeldValueChecker::function(const FunctionDecl *fn) {
  if (!fn || !fn->body || inLibrary(fn->loc()) || !walked_.insert(fn).second) {
    return;
  }
  const FunctionDecl *saved = current_;
  current_ = fn;
  if (fn->thisVar() && heldThis_.contains(fn)) {
    heldVars_.insert(fn->thisVar());
  }
  for (unsigned i = 0; i < fn->params.size(); ++i) {
    const Param &param = fn->params[i];
    if (param.sym && heldParams_.count({fn, i})) {
      heldVars_.insert(param.sym);
    }
    expr(param.defaultValue);
  }
  stmt(fn->body);
  current_ = saved;
}

void HeldValueChecker::topLevel(const Stmt *s) {
  if (!s || inLibrary(s->loc())) {
    return;
  }
  current_ = nullptr;
  if (auto *decl = dyn_cast<VarDeclStmt>(s); decl && decl->isConst) {
    for (const VarDeclarator &d : decl->decls) {
      if (!d.destructured) {
        allowedNews_.insert(bare(d.init));
      }
    }
  }
  stmt(s);
}

void HeldValueChecker::stmt(const Stmt *s) {
  if (!s) {
    return;
  }
  switch (s->kind()) {
  case Stmt::Kind::Block:
    if (NestedGroup *group = cast<BlockStmt>(s)->group) {
      for (const FunctionDecl *member : group->members) {
        function(member);
      }
    }
    for (const Stmt *x : cast<BlockStmt>(s)->body) {
      stmt(x);
    }
    break;
  case Stmt::Kind::VarDecl:
    for (const VarDeclarator &d : cast<VarDeclStmt>(s)->decls) {
      expr(d.init);
      if (!d.sym || d.destructured || !isHeld(d.init)) {
        continue;
      }
      if (d.sym->isGlobal) {
        keep(d.init, "in a module variable");
      } else {
        heldVars_.insert(d.sym);
      }
    }
    break;
  case Stmt::Kind::Function:
    function(cast<FunctionDecl>(s));
    break;
  case Stmt::Kind::Expr:
    expr(cast<ExprStmt>(s)->expr);
    break;
  case Stmt::Kind::If:
    expr(cast<IfStmt>(s)->cond);
    stmt(cast<IfStmt>(s)->thenStmt);
    stmt(cast<IfStmt>(s)->elseStmt);
    break;
  case Stmt::Kind::While:
    expr(cast<WhileStmt>(s)->cond);
    stmt(cast<WhileStmt>(s)->body);
    break;
  case Stmt::Kind::DoWhile:
    stmt(cast<DoWhileStmt>(s)->body);
    expr(cast<DoWhileStmt>(s)->cond);
    break;
  case Stmt::Kind::For:
    stmt(cast<ForStmt>(s)->init);
    expr(cast<ForStmt>(s)->cond);
    expr(cast<ForStmt>(s)->step);
    stmt(cast<ForStmt>(s)->body);
    break;
  case Stmt::Kind::ForOf:
    expr(cast<ForOfStmt>(s)->iterable);
    stmt(cast<ForOfStmt>(s)->body);
    break;
  case Stmt::Kind::Switch:
    expr(cast<SwitchStmt>(s)->discriminant);
    for (const SwitchCase &c : cast<SwitchStmt>(s)->cases) {
      expr(c.test);
      for (const Stmt *x : c.body) {
        stmt(x);
      }
    }
    break;
  case Stmt::Kind::Labeled:
    stmt(cast<LabeledStmt>(s)->body);
    break;
  case Stmt::Kind::Return: {
    const Expr *value = cast<ReturnStmt>(s)->value;
    awaited_.insert(bare(value));
    expr(value);
    if (current_ && isHeld(value)) {
      returnsHeld_.insert(current_);
    }
    break;
  }
  case Stmt::Kind::Class: {
    auto *cls = cast<ClassDecl>(s);
    for (const ClassField &f : cls->fields) {
      if (f.isStatic) {
        expr(f.init);
      }
    }
    for (const Stmt *x : cls->decorations) {
      stmt(x);
    }
    break;
  }
  default:
    break;
  }
}

void HeldValueChecker::expr(const Expr *e) {
  if (!e) {
    return;
  }
  switch (e->kind()) {
  case Expr::Kind::Function:
    function(cast<FunctionExpr>(e)->fn);
    return;
  case Expr::Kind::Await:
    awaited_.insert(bare(cast<AwaitExpr>(e)->value));
    expr(cast<AwaitExpr>(e)->value);
    return;
  case Expr::Kind::Call:
    call(cast<CallExpr>(e));
    return;
  case Expr::Kind::New:
    create(cast<NewExpr>(e));
    return;
  case Expr::Kind::Assign:
    assign(cast<AssignExpr>(e));
    return;
  case Expr::Kind::Unary: {
    auto *unary = cast<UnaryExpr>(e);
    expr(unary->operand);
    if (auto *c = dyn_cast_or_null<CallExpr>(bare(unary->operand));
        c && unary->op == UnaryOp::Void) {
      sawVoid(c);
    }
    return;
  }
  case Expr::Kind::ObjectLit:
    for (const ObjectLitExpr::Property &p : cast<ObjectLitExpr>(e)->props) {
      expr(p.key);
      expr(p.value);
      if (p.spread && isHeld(p.value)) {
        spreadKept(p.value);
      } else if (!p.spread && isHeld(p.value) && !allowedStores_.contains(e)) {
        keep(p.value, "in a new object");
      }
    }
    return;
  case Expr::Kind::ArrayLit:
    for (const ArrayLitExpr::Element &x : cast<ArrayLitExpr>(e)->elements) {
      expr(x.expr);
      if (!x.spread && isHeld(x.expr)) {
        keep(x.expr, "in an array");
      }
    }
    return;
  default:
    children(e, [&](const Expr *x) { expr(x); });
    return;
  }
}

void HeldValueChecker::call(const CallExpr *c) {
  sawCall(c);
  expr(c->callee);
  for (const Expr *arg : c->args) {
    expr(arg);
  }
  const FunctionDecl *target = programTarget(c);
  if (current_ && target) {
    callees_[current_].push_back(target);
  }
  if (target) {
    callSites_[target].push_back(c);
    if (target->generic) {
      callSites_[target->generic].push_back(c);
    }
  }
  bool over = takesOver(c);
  if (current_ && !over) {
    for (const Expr *arg : c->args) {
      if (auto *fe = dyn_cast<FunctionExpr>(bare(arg))) {
        callees_[current_].push_back(fe->fn);
      }
    }
  }
  if (auto *member = dyn_cast_or_null<MemberExpr>(c->callee);
      member && target && c->method && isHeld(member->object)) {
    markThis(target);
  }
  if (over) {
    return;
  }
  passed(c, target, calleeName(c));
}

void HeldValueChecker::passed(const CallExpr *c, const FunctionDecl *target, llvm::StringRef name) {
  for (unsigned i = 0; i < c->args.size(); ++i) {
    const Expr *arg = c->args[i];
    if (!isHeld(arg)) {
      continue;
    }
    if (target) {
      markParam(target, i);
      continue;
    }
    if (parallel::onlyReadsArgs(c->builtin) || c->builtin == Builtin::ResultOk ||
        c->builtin == Builtin::ResultErr) {
      continue;
    }
    if (auto *fe = dyn_cast<FunctionExpr>(bare(arg)); fe && fe->fn->nonEscaping) {
      continue;
    }
    if (name.empty()) {
      keep(arg, "by a function the compiler can't follow");
    } else {
      keep(arg, "by '" + name + "', which may store it");
    }
  }
}

void HeldValueChecker::create(const NewExpr *n) {
  const std::vector<Expr *> &args = n->ctor ? n->ctor->args : n->args;
  for (const Expr *arg : args) {
    expr(arg);
  }
  sawNew(n);
  const FunctionDecl *target = n->ctor ? programTarget(n->ctor) : nullptr;
  if (current_ && target) {
    callees_[current_].push_back(target);
  }
  if (allowedStores_.contains(n)) {
    return;
  }
  for (unsigned i = 0; i < args.size(); ++i) {
    if (!isHeld(args[i])) {
      continue;
    }
    if (target) {
      markParam(target, i);
    } else {
      keep(args[i], "in a new object");
    }
  }
}

void HeldValueChecker::assign(const AssignExpr *a) {
  expr(a->target);
  expr(a->value);
  if (a->op || !isHeld(a->value)) {
    return;
  }
  const Expr *target = bare(a->target);
  if (auto *name = dyn_cast<NameExpr>(target); name && name->var) {
    const VarSymbol *var = name->var;
    if (var->isGlobal) {
      keep(a->value, "in a module variable");
    } else if (var->owner && var->owner != current_) {
      keep(a->value, "in a variable of an outer function");
    } else {
      heldVars_.insert(var);
    }
    return;
  }
  if (isa<IndexExpr>(target)) {
    keep(a->value, "in an array or a map");
    return;
  }
  keep(a->value, "in a field");
}

bool HeldValueChecker::isHeld(const Expr *e) const {
  e = bare(e);
  if (!e) {
    return false;
  }
  switch (e->kind()) {
  case Expr::Kind::Call: {
    auto *c = cast<CallExpr>(e);
    if (produces(c)) {
      return true;
    }
    if ((c->builtin == Builtin::ResultOk || c->builtin == Builtin::ResultErr) && !c->args.empty()) {
      return isHeld(c->args[0]);
    }
    const FunctionDecl *target = programTarget(c);
    return target && returnsHeld_.contains(target);
  }
  case Expr::Kind::Name: {
    const VarSymbol *var = cast<NameExpr>(e)->var;
    return var && heldVars_.contains(var);
  }
  case Expr::Kind::Member: {
    auto *member = cast<MemberExpr>(e);
    const Type *object =
        member->object && member->object->type ? member->object->type->nonOptional() : nullptr;
    if (heldMember(member)) {
      return true;
    }
    return object && object->isResult() && (member->name == "value" || member->name == "error") &&
           isHeld(member->object);
  }
  case Expr::Kind::OptionalChain:
    return isHeld(cast<OptionalChainExpr>(e)->chain);
  case Expr::Kind::Function:
    return closuresCarry() && carries(cast<FunctionExpr>(e)->fn);
  case Expr::Kind::Conditional:
    return isHeld(cast<ConditionalExpr>(e)->thenExpr) || isHeld(cast<ConditionalExpr>(e)->elseExpr);
  case Expr::Kind::Binary: {
    auto *binary = cast<BinaryExpr>(e);
    bool picks = binary->op == BinaryOp::LogOr || binary->op == BinaryOp::LogAnd ||
                 binary->op == BinaryOp::Nullish;
    return picks && (isHeld(binary->lhs) || isHeld(binary->rhs));
  }
  case Expr::Kind::Assign: {
    auto *a = cast<AssignExpr>(e);
    return !a->op && isHeld(a->value);
  }
  default:
    return false;
  }
}

bool HeldValueChecker::carries(const FunctionDecl *fn) const {
  if (!fn) {
    return false;
  }
  const std::vector<VarSymbol *> &captures = fn->group ? fn->group->captures : fn->captures;

  return llvm::any_of(captures, [&](const VarSymbol *v) { return heldVars_.contains(v); });
}

void HeldValueChecker::keep(const Expr *value, const llvm::Twine &where) {
  if (!report_ || !value || !reported_.insert(value->loc().getPointer()).second) {
    return;
  }
  const Type *type = value->type ? value->type->nonOptional() : nullptr;
  kept(value, type && type->isFunction(), where);
}

bool HeldValueChecker::once(const Expr *value) {
  return report_ && value && reported_.insert(value->loc().getPointer()).second;
}

void HeldValueChecker::markParam(const FunctionDecl *fn, unsigned index) {
  if (fn && index < fn->params.size()) {
    heldParams_.insert({fn, index});
  }
}

void HeldValueChecker::markThis(const FunctionDecl *fn) {
  if (!fn || !fn->thisVar() || fn->isStatic()) {
    return;
  }
  heldThis_.insert(fn);
  if (!fn->cls()) {
    return;
  }
  std::vector<const ClassDecl *> pending(fn->cls()->children.begin(), fn->cls()->children.end());
  while (!pending.empty()) {
    const ClassDecl *cls = pending.back();
    pending.pop_back();
    for (const FunctionDecl *method : cls->methods) {
      for (const FunctionDecl *up = method->overrides(); up; up = up->overrides()) {
        if (up == fn) {
          heldThis_.insert(method);
          break;
        }
      }
    }
    pending.insert(pending.end(), cls->children.begin(), cls->children.end());
  }
}

const Expr *HeldValueChecker::bare(const Expr *e) {
  while (auto *conv = dyn_cast_or_null<CastExpr>(e)) {
    e = conv->operand;
  }

  return e;
}

const FunctionDecl *HeldValueChecker::callbackOf(const Expr *e) {
  e = bare(e);
  if (auto *fe = dyn_cast_or_null<FunctionExpr>(e)) {
    return fe->fn;
  }
  if (auto *name = dyn_cast_or_null<NameExpr>(e)) {
    if (name->func) {
      return name->func->decl;
    }
    if (name->var) {
      return name->var->lambda ? name->var->lambda : name->var->function;
    }
    return nullptr;
  }
  if (auto *member = dyn_cast_or_null<MemberExpr>(e); member && member->method) {
    return member->method->decl;
  }

  return nullptr;
}

const ClassDecl *HeldValueChecker::storageClass(const ClassDecl *cls) {
  for (; cls; cls = cls->parent) {
    const ClassDecl *base = cls->generic ? cls->generic : cls;
    if ((base->library || cls->library) && base->name == "AsyncLocalStorage") {
      return base;
    }
  }

  return nullptr;
}

bool HeldValueChecker::storageMethod(const CallExpr *c, llvm::StringRef name) {
  const FunctionDecl *decl = c->method ? c->method->decl : nullptr;

  return decl && decl->name == name && storageClass(decl->cls());
}

const VarSymbol *HeldValueChecker::storageOf(const CallExpr *c) {
  auto *member = dyn_cast_or_null<MemberExpr>(c->callee);
  auto *name = member ? dyn_cast_or_null<NameExpr>(bare(member->object)) : nullptr;

  return name ? name->var : nullptr;
}

const FunctionDecl *HeldValueChecker::programTarget(const CallExpr *c) const {
  if (c->builtin != Builtin::None) {
    return nullptr;
  }
  const FunctionDecl *decl = c->func ? c->func->decl : c->method ? c->method->decl : nullptr;
  if (!decl) {
    if (auto *name = dyn_cast_or_null<NameExpr>(c->callee);
        name && name->var && !name->var->reassigned) {
      decl = name->var->lambda ? name->var->lambda : name->var->function;
    }
  }
  if (!decl || !decl->body || inLibrary(decl->loc())) {
    return nullptr;
  }

  return decl;
}

}
