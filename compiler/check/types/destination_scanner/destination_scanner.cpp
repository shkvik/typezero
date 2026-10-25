#include "compiler/check/types/destination_scanner/destination_scanner.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/flow/predicate_checker/predicate_checker.h"
#include "compiler/check/iterators/iterable_checker/iterable_checker.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/shared_helpers/shared_helpers.h"
#include "compiler/check/types/destination/destination.h"
#include "compiler/check/types/destination_check_helpers/destination_check_helpers.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

// The type of a parameter without an annotation, from a default value that
// is a literal, as `let x = 5` would give it (sema.cpp).
const Type *literalType(const Expr *value);

// `ok(v)` or `err(e)` as written, before it is checked.
static bool isResultLiteral(const CallExpr *call) {
  auto *name = dyn_cast<NameExpr>(call->callee);

  return name && (name->name == "ok" || name->name == "err") && call->args.size() <= 1 &&
         !call->hasSpread() && !call->optional;
}

DestinationScanner::DestinationScanner(Checker &checker) : checker_(checker) {}

bool DestinationScanner::scanDestinations(llvm::ArrayRef<Stmt *> stmts,
                                          Checker::DestinationScan &scan) {
  // A function declared in a block is visible in all of it.
  for (const Stmt *stmt : stmts) {
    if (auto *fn = dyn_cast<FunctionDecl>(stmt); fn && fn->name == scan.name) {
      return false;
    }
  }
  scan.locals.emplace_back();
  for (const Stmt *stmt : stmts) {
    if (auto *fn = dyn_cast<FunctionDecl>(stmt)) {
      scan.declare(fn->name, checker_.destination().declaredFunctionType(fn));
    }
  }
  bool visible = true;
  for (const Stmt *stmt : stmts) {
    scanDestinations(stmt, scan);
    if (auto *decl = dyn_cast<VarDeclStmt>(stmt);
        decl &&
        llvm::any_of(decl->decls, [&](const VarDeclarator &v) { return v.name == scan.name; })) {
      visible = false;
      break;
    }
  }
  scan.locals.pop_back();

  return visible;
}

void DestinationScanner::scanFunctionDestinations(const FunctionDecl *fn,
                                                  Checker::DestinationScan &scan) {
  if (!fn->body || llvm::any_of(fn->params, [&](const Param &p) { return p.name == scan.name; })) {
    return;
  }
  const Type *outer = scan.returns;
  scan.returns =
      fn->returnType
          ? checker_.predicateChecker().resolveReturnType(const_cast<FunctionDecl *>(fn), nullptr)
          : nullptr;
  scan.locals.emplace_back();
  for (const Param &param : fn->params) {
    const Type *type = param.type && !checker_.iterableChecker().isIterableRef(*param.type)
                           ? checker_.typeResolver().resolveType(*param.type, /*allowVoid=*/false)
                           : nullptr;
    if (param.defaultValue) {
      scanDestinations(param.defaultValue, type, scan);
    }
    scan.declare(param.name, type && param.optional ? Type::optional(type) : type);
  }
  scanDestinations(fn->body->body, scan);
  scan.locals.pop_back();
  scan.returns = outer;
}

void DestinationScanner::scanDestinations(const Stmt *stmt, Checker::DestinationScan &scan) {
  if (!stmt) {
    return;
  }
  auto expr = [&](const Expr *e) {
    if (e) {
      scanDestinations(e, nullptr, scan);
    }
  };
  switch (stmt->kind()) {
  case Stmt::Kind::Block:
    scanDestinations(cast<BlockStmt>(stmt)->body, scan);
    break;
  case Stmt::Kind::VarDecl:
    for (const VarDeclarator &d : cast<VarDeclStmt>(stmt)->decls) {
      const Type *type = d.typeAnn
                             ? checker_.typeResolver().resolveType(*d.typeAnn, /*allowVoid=*/false)
                             : nullptr;
      if (d.init) {
        scanDestinations(d.init, type, scan);
      }
      scan.declare(d.name, type);
    }
    break;
  case Stmt::Kind::Function:
    scanFunctionDestinations(cast<FunctionDecl>(stmt), scan);
    break;
  case Stmt::Kind::Expr:
    expr(cast<ExprStmt>(stmt)->expr);
    break;
  case Stmt::Kind::If: {
    auto *s = cast<IfStmt>(stmt);
    expr(s->cond);
    scanDestinations(s->thenStmt, scan);
    scanDestinations(s->elseStmt, scan);
    break;
  }
  case Stmt::Kind::While:
    expr(cast<WhileStmt>(stmt)->cond);
    scanDestinations(cast<WhileStmt>(stmt)->body, scan);
    break;
  case Stmt::Kind::DoWhile:
    scanDestinations(cast<DoWhileStmt>(stmt)->body, scan);
    expr(cast<DoWhileStmt>(stmt)->cond);
    break;
  case Stmt::Kind::For: {
    auto *s = cast<ForStmt>(stmt);
    scan.locals.emplace_back();
    scanDestinations(s->init, scan);
    if (!scan.locals.back().contains(scan.name)) {
      expr(s->cond);
      expr(s->step);
      scanDestinations(s->body, scan);
    }
    scan.locals.pop_back();
    break;
  }
  case Stmt::Kind::ForOf: {
    auto *s = cast<ForOfStmt>(stmt);
    expr(s->iterable);
    if (s->binding.name == scan.name) {
      break;
    }
    scan.locals.emplace_back();
    scan.declare(s->binding.name, nullptr);
    scanDestinations(s->body, scan);
    scan.locals.pop_back();
    break;
  }
  case Stmt::Kind::Switch: {
    auto *s = cast<SwitchStmt>(stmt);
    expr(s->discriminant);
    std::vector<Stmt *> all;
    for (const SwitchCase &c : s->cases) {
      expr(c.test);
      all.insert(all.end(), c.body.begin(), c.body.end());
    }
    scanDestinations(all, scan);
    break;
  }
  case Stmt::Kind::Labeled:
    scanDestinations(cast<LabeledStmt>(stmt)->body, scan);
    break;
  case Stmt::Kind::Return:
    if (const Expr *value = cast<ReturnStmt>(stmt)->value) {
      scanDestinations(value, scan.returns && !scan.returns->isVoid() ? scan.returns : nullptr,
                       scan);
    }
    break;
  case Stmt::Kind::Throw:
    expr(cast<ThrowStmt>(stmt)->value);
    break;
  case Stmt::Kind::Try: {
    auto *s = cast<TryStmt>(stmt);
    scanDestinations(s->block, scan);
    if (s->handler && s->param.name != scan.name) {
      scan.locals.emplace_back();
      scan.declare(s->param.name, nullptr);
      scanDestinations(s->handler, scan);
      scan.locals.pop_back();
    }
    scanDestinations(s->finalizer, scan);
    break;
  }
  default:
    break;
  }
}

void DestinationScanner::scanDestinations(const Expr *expr, const Type *sink,
                                          Checker::DestinationScan &scan) {
  auto sub = [&](const Expr *e, const Type *to = nullptr) {
    if (e) {
      scanDestinations(e, to, scan);
    }
  };
  if (sink && sink->isError()) {
    sink = nullptr;
  }
  const Type *object = sink ? sink->nonOptional() : nullptr;
  switch (expr->kind()) {
  case Expr::Kind::Name: {
    auto *name = cast<NameExpr>(expr);
    if (sink && name->name == scan.name && !scan.hidden(name->name)) {
      scan.found.push_back({sink, name->loc()});
    }
    break;
  }
  case Expr::Kind::ObjectLit:
    // `draw({ from: p })`: p goes to the field.
    for (const ObjectLitExpr::Property &prop : cast<ObjectLitExpr>(expr)->props) {
      sub(prop.key);
      const Field *field =
          object && object->isObject() && !prop.spread ? object->field(prop.name) : nullptr;
      sub(prop.value, field ? field->type : nullptr);
    }
    break;
  case Expr::Kind::ArrayLit: {
    auto *lit = cast<ArrayLitExpr>(expr);
    for (size_t i = 0; i < lit->elements.size(); ++i) {
      const ArrayLitExpr::Element &e = lit->elements[i];
      const Type *to = nullptr;
      if (object && object->isArray()) {
        to = e.spread ? object : object->inner();
      } else if (object && object->isTuple() && !e.spread && i < object->fields().size()) {
        to = object->fields()[i].type;
      }
      sub(e.expr, to);
    }
    break;
  }
  case Expr::Kind::Conditional: {
    auto *cond = cast<ConditionalExpr>(expr);
    sub(cond->cond);
    sub(cond->thenExpr, sink);
    sub(cond->elseExpr, sink);
    break;
  }
  case Expr::Kind::Binary: {
    auto *bin = cast<BinaryExpr>(expr);
    bool passes =
        bin->op == BinaryOp::Nullish || bin->op == BinaryOp::LogAnd || bin->op == BinaryOp::LogOr;
    sub(bin->lhs, bin->op == BinaryOp::Nullish ? sink : nullptr);
    sub(bin->rhs, passes ? sink : nullptr);
    break;
  }
  case Expr::Kind::Sequence: {
    auto *seq = cast<SequenceExpr>(expr);
    for (size_t i = 0; i < seq->exprs.size(); ++i) {
      sub(seq->exprs[i], i + 1 == seq->exprs.size() ? sink : nullptr);
    }
    break;
  }
  case Expr::Kind::Assign: {
    auto *assign = cast<AssignExpr>(expr);
    auto *target = dyn_cast<NameExpr>(assign->target);
    if (target && target->name == scan.name && !scan.hidden(target->name)) {
      // `p = q`: p must hold what q holds.
      if (const Type *type = assign->op ? nullptr : peekType(assign->value, scan)) {
        scan.found.push_back({type, assign->loc()});
      }
      sub(assign->value);
      break;
    }
    if (!isa<NameExpr>(assign->target)) {
      sub(assign->target);
    }
    sub(assign->value, assign->op ? nullptr : peekType(assign->target, scan));
    break;
  }
  case Expr::Kind::Call: {
    auto *call = cast<CallExpr>(expr);
    std::vector<const Type *> params = calleeParams(call, scan);
    sub(call->callee);
    for (size_t i = 0; i < call->args.size(); ++i) {
      sub(call->args[i], i < params.size() && !call->spreads(i) ? params[i] : nullptr);
    }
    break;
  }
  case Expr::Kind::Function:
    scanFunctionDestinations(cast<FunctionExpr>(expr)->fn, scan);
    break;
  case Expr::Kind::Template:
    for (const Expr *e : cast<TemplateExpr>(expr)->exprs) {
      sub(e);
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
  case Expr::Kind::Update:
    sub(cast<UpdateExpr>(expr)->target);
    break;
  case Expr::Kind::New: {
    // The arguments of the constructor of a class, if it has one variant.
    auto *create = cast<NewExpr>(expr);
    const FuncSymbol *ctor = nullptr;
    if (ClassDecl *cls =
            scan.hidden(create->name) ? nullptr : checker_.scopes().lookup(create->name).cls) {
      if (cls->constructors.size() == 1 && cls->constructors[0]->sym &&
          !cls->constructors[0]->sym->pendingParams) {
        ctor = cls->constructors[0]->sym;
      }
    }
    for (size_t i = 0; i < create->args.size(); ++i) {
      sub(create->args[i],
          ctor && i < ctor->params.size() && !ctor->hasRest ? ctor->params[i] : nullptr);
    }
    break;
  }
  case Expr::Kind::Cast:
    sub(cast<CastExpr>(expr)->operand);
    break;
  case Expr::Kind::OptionalChain:
    sub(cast<OptionalChainExpr>(expr)->chain);
    break;
  case Expr::Kind::Yield:
    if (const Expr *value = cast<YieldExpr>(expr)->value) {
      sub(value);
    }
    break;
  case Expr::Kind::Await:
    sub(cast<AwaitExpr>(expr)->value);
    break;
  default:
    break;
  }
}

const Type *DestinationScanner::peekName(llvm::StringRef name,
                                         const Checker::DestinationScan &scan) const {
  for (const llvm::StringMap<const Type *> &scope : llvm::reverse(scan.locals)) {
    if (auto found = scope.find(name); found != scope.end()) {
      return found->second;
    }
  }
  VarSymbol *var = checker_.scopes().lookup(name).var;

  return var ? var->type : nullptr;
}

const Type *DestinationScanner::peekType(const Expr *expr,
                                         const Checker::DestinationScan &scan) const {
  if (auto *name = dyn_cast<NameExpr>(expr)) {
    return name->name == scan.name ? nullptr : peekName(name->name, scan);
  }
  if (auto *member = dyn_cast<MemberExpr>(expr)) {
    // `crypto.subtle`: the SubtleCrypto of tz:webcrypto, whose methods take algorithms.
    if (member->name == "subtle" && checker_.callChecker().isGlobalName(member->object, "crypto")) {
      VarSymbol *subtle = checker_.scopes().lookup(kSubtleName).var;
      return subtle ? subtle->type : nullptr;
    }
    const Type *object = peekType(member->object, scan);
    object = object ? object->nonOptional() : nullptr;
    if (object && object->isRecord()) { // `r.key`
      return object->valueType();
    }
    const Field *field = object && object->isObject() ? object->field(member->name) : nullptr;
    return field ? field->type : nullptr;
  }
  if (auto *index = dyn_cast<IndexExpr>(expr)) {
    const Type *object = peekType(index->object, scan);
    if (object && object->isRecord()) {
      return object->valueType();
    }
    return object && object->isArray() ? object->inner() : nullptr;
  }
  if (auto *cond = dyn_cast<ConditionalExpr>(expr); cond && Destination::literalShaped(cond)) {
    const Type *a = peekType(cond->thenExpr, scan), *b = peekType(cond->elseExpr, scan);
    const Type *common = a && b ? TypeRelations::unify(a, b) : nullptr;
    return common && !common->isError() ? common : nullptr;
  }
  // `err("x")`, whose other half can be anything.
  if (auto *call = dyn_cast<CallExpr>(expr); call && isResultLiteral(call)) {
    const Type *half = Type::voidTy();
    if (!call->args.empty()) {
      half = peekType(call->args[0], scan);
      if (!half) {
        half = literalType(call->args[0]);
      }
    }
    if (!half) {
      return nullptr;
    }
    return cast<NameExpr>(call->callee)->name == "ok" ? Type::result(half, Type::never())
                                                      : Type::result(Type::never(), half);
  }

  return nullptr;
}

std::vector<const Type *> DestinationScanner::calleeParams(const CallExpr *call,
                                                           const Checker::DestinationScan &scan) {
  // The parameters of a function type, the elements of a rest parameter
  // repeated for the arguments it takes.
  auto spread = [&](llvm::ArrayRef<const Type *> params, bool hasRest) {
    std::vector<const Type *> types(params.begin(), params.end());
    if (hasRest && !types.empty()) {
      const Type *rest = types.back();
      types.pop_back();
      while (types.size() < call->args.size()) {
        types.push_back(rest && rest->isArray() ? rest->inner() : nullptr);
      }
    }

    return types;
  };
  if (auto *name = dyn_cast<NameExpr>(call->callee)) {
    if (scan.hidden(name->name)) {
      const Type *type = peekName(name->name, scan);
      return type && type->isFunction() ? spread(type->params(), type->hasRest())
                                        : std::vector<const Type *>{};
    }
    Checker::ScopeEntry entry = checker_.scopes().lookup(name->name);
    FuncSymbol *func = entry.func;
    if (entry.var && entry.var->function) {
      func = entry.var->function->sym;
    }
    if (func && !func->pendingParams) {
      return spread(func->params, func->hasRest);
    }
    if (entry.var && entry.var->type && entry.var->type->isFunction()) {
      return spread(entry.var->type->params(), entry.var->type->hasRest());
    }
    return {};
  }
  if (auto *member = dyn_cast<MemberExpr>(call->callee)) {
    // A method of a class, or a static one: if it has one variant.
    auto variant = [&](const ClassDecl *cls, bool isStatic) -> std::vector<const Type *> {
      std::vector<FunctionDecl *> methods =
          ClassDeclarer::methodsNamed(cls, member->name, isStatic);
      if (methods.size() != 1 || !methods[0]->sym || methods[0]->sym->pendingParams) {
        return {};
      }

      return spread(methods[0]->sym->params, methods[0]->sym->hasRest);
    };
    if (auto *name = dyn_cast<NameExpr>(member->object); name && !scan.hidden(name->name)) {
      if (ClassDecl *cls = checker_.scopes().lookup(name->name).cls) {
        return variant(cls, /*isStatic=*/true);
      }
    }
    const Type *object = peekType(member->object, scan);
    if (!object) {
      return {};
    }
    if (ClassDecl *cls = ClassDeclarer::classOf(object->nonOptional())) {
      return variant(cls, /*isStatic=*/false);
    }
    object = object->nonOptional();
    if (object->isArray()) {
      // Where the arguments of array methods go.
      static const char *const elements[] = {"push", "unshift", "includes", "indexOf",
                                             "lastIndexOf"};
      if (llvm::is_contained(elements, member->name)) {
        return std::vector<const Type *>(call->args.size(), object->inner());
      }
      if (member->name == "fill") {
        return {object->inner()};
      }
      return {};
    }
    if (object->isObject()) {
      if (const Field *field = object->field(member->name); field && field->type->isFunction()) {
        return spread(field->type->params(), field->type->hasRest());
      }
    }
    // Where the keys and values of a Map and a Set go.
    if (object->isMap() && member->name == "set") {
      return {object->keyType(), object->valueType()};
    }
    if (object->isDict() && !object->isRecord() &&
        (member->name == "get" || member->name == "has" || member->name == "delete" ||
         member->name == "add")) {
      return {object->keyType()};
    }
  }

  return {};
}

} // namespace tz
