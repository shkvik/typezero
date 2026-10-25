#include "compiler/lower/errors/throw_scanner/throw_scanner.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include <functional>
#include <map>
#include <optional>
#include "compiler/lower/errors/throw_calls/throw_calls.h"

namespace tz {

using llvm::cast;

using llvm::dyn_cast;

using llvm::isa;

using llvm::BasicBlock;

using llvm::Value;

void forEachImplementation(const FunctionDecl *fn,
                           llvm::function_ref<void(const FunctionDecl *)> visit) {
  if (!fn->isAbstract) {
    visit(fn);
  }
  if (!fn->cls || fn->slot < 0) {
    return;
  }
  std::function<void(const ClassDecl *)> down = [&](const ClassDecl *cls) {
    for (const ClassDecl *child : cls->children) {
      if (size_t(fn->slot) < child->vtable.size()) {
        if (const FunctionDecl *impl = child->vtable[size_t(fn->slot)]; impl && impl != fn) {
          visit(impl);
        }
      }
      down(child);
    }
  };
  down(fn->cls);
}

std::optional<int64_t> constantInt(const Expr *expr) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit()) {
      return std::nullopt;
    }
    expr = conv->operand;
  }
  if (auto *lit = dyn_cast<IntLitExpr>(expr)) {
    return int64_t(lit->value);
  }
  if (auto *unary = dyn_cast<UnaryExpr>(expr); unary && unary->op == UnaryOp::Neg) {
    if (auto *lit = dyn_cast<IntLitExpr>(unary->operand)) {
      return -int64_t(lit->value);
    }
  }

  return std::nullopt;
}

const FunctionDecl *namedFunction(const Expr *expr) {
  auto *name = dyn_cast<NameExpr>(expr);
  if (!name) {
    return nullptr;
  }
  if (name->func) {
    return name->func->decl;
  }
  if (const VarSymbol *var = name->var) {
    return var->function ? var->function : var->isConst ? var->lambda : nullptr;
  }

  return nullptr;
}

ThrowScanner::ThrowScanner(ThrowSummary &out) : out_(out) {
  throwCallsPart_ = std::make_unique<ThrowCalls>(*this);}

void ThrowScanner::stmt(const Stmt *stmt, bool caught) {
  if (!stmt) {
    return;
  }
  switch (stmt->kind()) {
  case Stmt::Kind::Block:
    for (const Stmt *s : llvm::cast<BlockStmt>(stmt)->body) {
      this->stmt(s, caught);
    }
    break;
  case Stmt::Kind::VarDecl:
    for (const VarDeclarator &d : llvm::cast<VarDeclStmt>(stmt)->decls) {
      expr(d.init, caught);
    }
    break;
  case Stmt::Kind::Function:
    out_.nested.push_back(llvm::cast<FunctionDecl>(stmt));
    break;
  case Stmt::Kind::Expr:
    expr(llvm::cast<ExprStmt>(stmt)->expr, caught);
    break;
  case Stmt::Kind::If: {
    auto *s = llvm::cast<IfStmt>(stmt);
    expr(s->cond, caught);
    this->stmt(s->thenStmt, caught);
    this->stmt(s->elseStmt, caught);
    break;
  }
  case Stmt::Kind::While:
    expr(llvm::cast<WhileStmt>(stmt)->cond, caught);
    this->stmt(llvm::cast<WhileStmt>(stmt)->body, caught);
    break;
  case Stmt::Kind::DoWhile:
    this->stmt(llvm::cast<DoWhileStmt>(stmt)->body, caught);
    expr(llvm::cast<DoWhileStmt>(stmt)->cond, caught);
    break;
  case Stmt::Kind::For: {
    auto *s = llvm::cast<ForStmt>(stmt);
    this->stmt(s->init, caught);
    expr(s->cond, caught);
    expr(s->step, caught);
    this->stmt(s->body, caught);
    break;
  }
  case Stmt::Kind::ForOf: {
    auto *s = llvm::cast<ForOfStmt>(stmt);
    expr(s->iterable, caught);
    if (s->iterable->type && s->iterable->type->isIterator()) {
      throwCalls().resumes(caught);
    }
    this->stmt(s->body, caught);
    break;
  }
  case Stmt::Kind::Switch: {
    auto *s = llvm::cast<SwitchStmt>(stmt);
    expr(s->discriminant, caught);
    for (const SwitchCase &c : s->cases) {
      expr(c.test, caught);
      for (const Stmt *body : c.body) {
        this->stmt(body, caught);
      }
    }
    break;
  }
  case Stmt::Kind::Labeled:
    this->stmt(llvm::cast<LabeledStmt>(stmt)->body, caught);
    break;
  case Stmt::Kind::Return:
    expr(llvm::cast<ReturnStmt>(stmt)->value, caught);
    break;
  case Stmt::Kind::Throw:
    expr(llvm::cast<ThrowStmt>(stmt)->value, caught);
    throwCalls().intrinsic(caught);
    break;
  case Stmt::Kind::Try: {
    auto *s = llvm::cast<TryStmt>(stmt);
    this->stmt(s->block, caught || s->handler);
    this->stmt(s->handler, caught);
    this->stmt(s->finalizer, caught);
    break;
  }
  case Stmt::Kind::Class: {
    // Its static fields get their values where it is declared (those of
    // an instance of a generic class are the generic class's), and its
    // decorators run there.
    auto *cls = llvm::cast<ClassDecl>(stmt);
    expr(cls->baseExpr, caught);
    for (const ClassField &f : cls->fields) {
      if (f.isStatic && f.var) {
        expr(f.init, caught);
      }
    }
    for (const Stmt *s : cls->decorations) {
      this->stmt(s, caught);
    }
    break;
  }
  default:
    break;
  }
}

void ThrowScanner::expr(const Expr *expr, bool caught) {
  if (!expr) {
    return;
  }
  // An iterator object runs code when it is resumed; whatever consumes one
  // may resume it. That of a dictionary only walks its entries.
  auto *dictIterator = llvm::dyn_cast<CallExpr>(expr);
  if (expr->type && expr->type->isIterator() &&
      !(dictIterator && isDictIterator(dictIterator->builtin))) {
    throwCalls().resumes(caught);
  }
  switch (expr->kind()) {
  case Expr::Kind::Name: {
    auto *name = llvm::cast<NameExpr>(expr);
    if (const FunctionDecl *fn = namedFunction(name);
        fn && name->type && name->type->isFunction()) {
      throwCalls().value(name->type, fn);
    }
    break;
  }
  case Expr::Kind::Template:
    for (const Expr *e : llvm::cast<TemplateExpr>(expr)->exprs) {
      this->expr(e, caught);
    }
    break;
  case Expr::Kind::ArrayLit:
    for (const ArrayLitExpr::Element &e : llvm::cast<ArrayLitExpr>(expr)->elements) {
      this->expr(e.expr, caught);
    }
    break;
  case Expr::Kind::ObjectLit:
    for (const ObjectLitExpr::Property &p : llvm::cast<ObjectLitExpr>(expr)->props) {
      this->expr(p.key, caught);
      this->expr(p.value, caught);
    }
    break;
  case Expr::Kind::Member: {
    auto *member = llvm::cast<MemberExpr>(expr);
    this->expr(member->object, caught);
    if (jsonReadMayThrow(member->object)) {
      throwCalls().intrinsic(caught); // a key of null or undefined
    }
    // A method as a value: through the method table, any that overrides it.
    if (member->method && member->type) {
      if (member->virtualCall) {
        forEachImplementation(member->method->decl,
                              [&](const FunctionDecl *fn) { throwCalls().value(member->type, fn); });
      } else {
        throwCalls().value(member->type, member->method->decl);
      }
    }
    break;
  }
  case Expr::Kind::Index:
    this->expr(llvm::cast<IndexExpr>(expr)->object, caught);
    this->expr(llvm::cast<IndexExpr>(expr)->index, caught);
    if (jsonReadMayThrow(llvm::cast<IndexExpr>(expr)->object)) {
      throwCalls().intrinsic(caught);
    }
    break;
  case Expr::Kind::Unary: {
    auto *unary = llvm::cast<UnaryExpr>(expr);
    this->expr(unary->operand, caught);
    if (unary->op == UnaryOp::NonNull && unary->operand->type &&
        unary->operand->type->isOptional()) {
      throwCalls().intrinsic(caught);
    }
    break;
  }
  case Expr::Kind::Binary: {
    auto *bin = llvm::cast<BinaryExpr>(expr);
    this->expr(bin->lhs, caught);
    if (bin->op != BinaryOp::InstanceOf) {
      this->expr(bin->rhs, caught);
    }
    if (ThrowCalls::operatorThrows(bin->op, bin->lhs->type, bin->rhs)) {
      throwCalls().intrinsic(caught);
    }
    break;
  }
  case Expr::Kind::Assign: {
    auto *assign = llvm::cast<AssignExpr>(expr);
    throwCalls().target(assign->target, caught);
    this->expr(assign->value, caught);
    if (assign->op && ThrowCalls::operatorThrows(*assign->op, assign->type, assign->value)) {
      throwCalls().intrinsic(caught);
    }
    // `a.length = n`: a negative length, or one that grows an array whose
    // elements have no default.
    if (auto *member = llvm::dyn_cast<MemberExpr>(assign->target);
        member && member->object->type && member->object->type->isArray()) {
      throwCalls().intrinsic(caught);
    }
    // A key of a Json that is no object, or an element past the end.
    if (auto *member = llvm::dyn_cast<MemberExpr>(assign->target);
        member && member->object->type && member->object->type->nonOptional()->isJson()) {
      throwCalls().intrinsic(caught);
    }
    if (auto *index = llvm::dyn_cast<IndexExpr>(assign->target);
        index && index->object->type && index->object->type->nonOptional()->isJson()) {
      throwCalls().intrinsic(caught);
    }
    break;
  }
  case Expr::Kind::Update: {
    auto *update = llvm::cast<UpdateExpr>(expr);
    throwCalls().target(update->target, caught);
    if (update->type && update->type->isDecimal()) {
      throwCalls().intrinsic(caught);
    }
    break;
  }
  case Expr::Kind::Call:
    throwCalls().call(llvm::cast<CallExpr>(expr), caught);
    break;
  case Expr::Kind::New: {
    auto *create = llvm::cast<NewExpr>(expr);
    this->expr(create->callee, caught);
    for (const Expr *arg : create->ctor ? create->ctor->args : create->args) {
      this->expr(arg, caught);
    }
    // Through the description of the class: its arguments are checked
    // then, and its code may throw.
    if (create->reflect) {
      throwCalls().intrinsic(caught);
      break;
    }
    if (const ClassDecl *cls = create->cls) {
      // The initializers of the classes up the chain run too.
      for (const ClassDecl *c = cls; c; c = c->parent) {
        if (c->init) {
          throwCalls().callee(c->init, caught, nullptr);
        }
      }
      if (create->ctor && create->ctor->func) {
        throwCalls().callee(create->ctor->func->decl, caught, nullptr);
      }
    } else if (create->args.size() == 1) {
      std::optional<int64_t> length = constantInt(create->args[0]);
      if (!length || *length < 0) {
        throwCalls().intrinsic(caught);
      }
    }
    break;
  }
  case Expr::Kind::Conditional: {
    auto *cond = llvm::cast<ConditionalExpr>(expr);
    this->expr(cond->cond, caught);
    this->expr(cond->thenExpr, caught);
    this->expr(cond->elseExpr, caught);
    break;
  }
  case Expr::Kind::Cast: {
    auto *conv = llvm::cast<CastExpr>(expr);
    this->expr(conv->operand, caught);
    // An adapter calls the function it wraps.
    if (conv->adaptsResult) {
      out_.adapters.push_back({conv->type, conv->operand->type});
    } else if (conv->type && conv->type->isFunction() && conv->operand->type &&
               conv->operand->type->isFunction()) {
      if (const FunctionDecl *fn = namedFunction(conv->operand)) {
        throwCalls().value(conv->type, fn);
      }
    }
    break;
  }
  case Expr::Kind::Sequence:
    for (const Expr *e : llvm::cast<SequenceExpr>(expr)->exprs) {
      this->expr(e, caught);
    }
    break;
  case Expr::Kind::OptionalChain:
    this->expr(llvm::cast<OptionalChainExpr>(expr)->chain, caught);
    break;
  case Expr::Kind::Function: {
    auto *fe = llvm::cast<FunctionExpr>(expr);
    out_.nested.push_back(fe->fn);
    if (fe->type) {
      throwCalls().value(fe->type, fe->fn);
    }
    break;
  }
  case Expr::Kind::Yield: {
    auto *yield = llvm::cast<YieldExpr>(expr);
    this->expr(yield->value, caught);
    // next() without a value may resume a `yield` that needs one.
    if (!yield->discarded && yield->type && !yield->type->isOptional() &&
        !yield->type->isUndefined() && !yield->type->isVoid()) {
      throwCalls().intrinsic(caught);
    }
    break;
  }
  case Expr::Kind::Await: {
    // A Promise that is rejected throws its error here.
    const Expr *value = llvm::cast<AwaitExpr>(expr)->value;
    this->expr(value, caught);
    if (value->type && value->type->isPromise()) {
      throwCalls().intrinsic(caught);
    }
    break;
  }
  case Expr::Kind::Class: // as the declaration of its class, where it is
    stmt(llvm::cast<ClassExpr>(expr)->decl, caught);
    break;
  default:
    break;
  }
}

ThrowScanner::~ThrowScanner() = default;
} // namespace tz
