#include "compiler/semantic/flow/loop_scanner/loop_scanner.h"
#include "compiler/program/library/array/array_method_table/array_method_table.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/program/library/builtin_names/global_member_table/global_member_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/iterator/iterator_method_table/iterator_method_table.h"
#include "compiler/program/library/result/result_method_table/result_method_table.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/objects/keyed_field_checker/keyed_field_checker.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

const GlobalName kGlobalFunctions[] = {GlobalName::STRING,     GlobalName::NUMBER,
                                       GlobalName::PARSE_INT,  GlobalName::PARSE_FLOAT,
                                       GlobalName::BOOLEAN,    GlobalName::ENCODE_URI_COMPONENT,
                                       GlobalName::ENCODE_URI, GlobalName::DECODE_URI_COMPONENT,
                                       GlobalName::DECODE_URI, GlobalName::OK,
                                       GlobalName::ERR};
}

LoopScanner::LoopScanner(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.getScopes();
  this->callChecker = &all.getCallChecker();
  this->keyedFieldChecker = &all.getKeyedFieldChecker();
}

LoopScan LoopScanner::scanLoop(llvm::ArrayRef<const Stmt *> stmts,
                               llvm::ArrayRef<const Expr *> exprs) const {
  LoopScan scan = this->scanWithNames(stmts, exprs, llvm::StringSet<>());

  return scan;
}

LoopScan LoopScanner::scanForOfLoop(const ForOfStmt *loop) const {
  llvm::StringSet<> bindingName;
  bindingName.insert(loop->binding.name);
  LoopScan scan = this->scanWithNames({loop->body}, {}, bindingName);

  return scan;
}

LoopScan LoopScanner::scanWithNames(llvm::ArrayRef<const Stmt *> stmts,
                                    llvm::ArrayRef<const Expr *> exprs,
                                    const llvm::StringSet<> &headNames) const {
  LoopScan namesPass = this->scanParts(stmts, exprs, headNames);
  LoopScan scan = this->scanParts(stmts, exprs, namesPass.declaredInside);

  return scan;
}

LoopScan LoopScanner::scanParts(llvm::ArrayRef<const Stmt *> stmts,
                                llvm::ArrayRef<const Expr *> exprs,
                                const llvm::StringSet<> &declaredInside) const {
  LoopScan scan;
  scan.declaredInside = declaredInside;
  for (const Stmt *stmt : stmts) {
    if (stmt != nullptr) {
      this->scanStmt(stmt, scan, 0);
    }
  }
  for (const Expr *expr : exprs) {
    if (expr != nullptr) {
      this->scanExpr(expr, scan, 0);
    }
  }

  return scan;
}

bool LoopScanner::namesIterator(const Expr *expr) const {
  auto *name = dyn_cast<NameExpr>(expr);
  const VarSymbol *var = name ? this->scopes->lookup(name->name).var() : nullptr;

  return var && var->type && var->type->nonOptional()->isIterator();
}

void LoopScanner::scanStmt(const Stmt *stmt, LoopScan &scan, unsigned depth) const {
  switch (stmt->kind()) {
  case Stmt::Kind::Block:
    for (const Stmt *s : cast<BlockStmt>(stmt)->body) {
      this->scanStmt(s, scan, depth);
    }
    break;
  case Stmt::Kind::VarDecl:
    for (const VarDeclarator &d : cast<VarDeclStmt>(stmt)->decls) {
      scan.declaredInside.insert(d.name);
      if (d.init) {
        this->scanExpr(d.init, scan, depth);
      }
      if (d.destructured() && d.init && this->namesIterator(d.init)) {
        scan.callsFunctions = true;
      }
    }
    break;
  case Stmt::Kind::Function: {
    auto *fn = cast<FunctionDecl>(stmt);
    scan.declaredInside.insert(fn->name);
    LoopScanner::declareParams(fn, scan);
    this->scanStmt(fn->body, scan, depth + 1);
    break;
  }
  case Stmt::Kind::Continue:
  case Stmt::Kind::Empty:
  case Stmt::Kind::Debugger:
    break;
  case Stmt::Kind::Expr:
    this->scanExpr(cast<ExprStmt>(stmt)->expr, scan, depth);
    break;
  case Stmt::Kind::If: {
    auto *s = cast<IfStmt>(stmt);
    this->scanExpr(s->cond, scan, depth);
    this->scanStmt(s->thenStmt, scan, depth);
    if (s->elseStmt) {
      this->scanStmt(s->elseStmt, scan, depth);
    }
    break;
  }
  case Stmt::Kind::While: {
    auto *s = cast<WhileStmt>(stmt);
    this->scanExpr(s->cond, scan, depth + 1);
    this->scanStmt(s->body, scan, depth + 1);
    break;
  }
  case Stmt::Kind::DoWhile: {
    auto *s = cast<DoWhileStmt>(stmt);
    this->scanStmt(s->body, scan, depth + 1);
    this->scanExpr(s->cond, scan, depth + 1);
    break;
  }
  case Stmt::Kind::For: {
    auto *s = cast<ForStmt>(stmt);
    if (s->init) {
      this->scanStmt(s->init, scan, depth + 1);
    }
    if (s->cond) {
      this->scanExpr(s->cond, scan, depth + 1);
    }
    if (s->step) {
      this->scanExpr(s->step, scan, depth + 1);
    }
    this->scanStmt(s->body, scan, depth + 1);
    break;
  }
  case Stmt::Kind::ForOf: {
    auto *s = cast<ForOfStmt>(stmt);
    scan.declaredInside.insert(s->binding.name);
    if (s->isAwait && !scan.functionDepth) {
      scan.awaits = true;
    }
    this->scanExpr(s->iterable, scan, depth);
    if (this->namesIterator(s->iterable)) {
      scan.callsFunctions = true;
    }
    this->scanStmt(s->body, scan, depth + 1);
    break;
  }
  case Stmt::Kind::Switch: {
    auto *s = cast<SwitchStmt>(stmt);
    this->scanExpr(s->discriminant, scan, depth);
    for (const SwitchCase &c : s->cases) {
      if (c.test) {
        this->scanExpr(c.test, scan, depth);
      }
    }
    ++scan.switches;
    for (const SwitchCase &c : s->cases) {
      for (const Stmt *body : c.body) {
        this->scanStmt(body, scan, depth);
      }
    }
    --scan.switches;
    break;
  }
  case Stmt::Kind::Labeled: {
    auto *s = cast<LabeledStmt>(stmt);
    scan.innerLabels.push_back(s->label);
    this->scanStmt(s->body, scan, depth);
    scan.innerLabels.pop_back();
    break;
  }
  case Stmt::Kind::Return:
    if (auto *value = cast<ReturnStmt>(stmt)->value) {
      this->scanExpr(value, scan, depth);
    }
    break;
  case Stmt::Kind::Break: {
    llvm::StringRef label = cast<BreakStmt>(stmt)->label;
    if (label.empty() ? depth == 0 && scan.switches == 0
                      : !llvm::is_contained(scan.innerLabels, label)) {
      scan.breaks = true;
    }
    break;
  }
  case Stmt::Kind::Enum:
    scan.declaredInside.insert(cast<EnumDecl>(stmt)->name);
    break;
  case Stmt::Kind::Class:
    scan.declaredInside.insert(cast<ClassDecl>(stmt)->name);
    break;
  case Stmt::Kind::TypeAlias:
    break;
  }
}

void LoopScanner::scanExpr(const Expr *expr, LoopScan &scan, unsigned depth) const {
  auto change = [&](const Expr *target, int64_t delta) {
    auto *name = dyn_cast<NameExpr>(target);
    if (!name) {
      return;
    }
    LoopScan::Change &c = scan.changes[name->name];
    c.delta = ++c.count == 1 ? delta : 0;
    c.nested |= depth > 0;
  };
  auto step = [](const Expr *e) -> int64_t {
    auto *lit = dyn_cast<IntLitExpr>(e);

    return lit && lit->value >= 1 && lit->value <= uint64_t(Type::kMaxLength) ? int64_t(lit->value)
                                                                              : 0;
  };
  auto isName = [](const Expr *e, const Expr *target) {
    auto *a = dyn_cast<NameExpr>(e);
    auto *b = dyn_cast<NameExpr>(target);

    return a && b && a->name == b->name;
  };
  auto scanTarget = [&](const Expr *target) {
    if (auto *index = dyn_cast<IndexExpr>(target)) {
      this->scanExpr(index->object, scan, depth);
      this->scanExpr(index->index, scan, depth);
      scan.growsArrays = true;
      scan.releasesElements = true;
      this->keyedFieldChecker->keyNames(
          index->index, [&](llvm::StringRef name) { scan.fieldWrites.insert(name); });
    } else if (auto *member = dyn_cast<MemberExpr>(target)) {
      this->scanExpr(member->object, scan, depth);
      scan.fieldWrites.insert(member->name);
      if (BuiltinPropertyTable::isName(member->name, BuiltinProperty::LENGTH)) {
        scan.growsArrays = scan.shrinksArrays = true;
        scan.releasesElements = true;
      }
    }
  };

  switch (expr->kind()) {
  case Expr::Kind::IntLit:
  case Expr::Kind::FloatLit:
  case Expr::Kind::BoolLit:
  case Expr::Kind::StringLit:
  case Expr::Kind::Undefined:
  case Expr::Kind::Name:
    break;
  case Expr::Kind::Template:
    for (const Expr *e : cast<TemplateExpr>(expr)->exprs) {
      this->scanExpr(e, scan, depth);
    }
    break;
  case Expr::Kind::ArrayLit:
    for (const ArrayLitExpr::Element &e : cast<ArrayLitExpr>(expr)->elements) {
      this->scanExpr(e.expr, scan, depth);
      if (e.spread && this->namesIterator(e.expr)) {
        scan.callsFunctions = true;
      }
    }
    break;
  case Expr::Kind::ObjectLit:
    for (const ObjectLitExpr::Property &p : cast<ObjectLitExpr>(expr)->props) {
      if (p.key()) {
        this->scanExpr(p.key(), scan, depth);
      }
      this->scanExpr(p.value, scan, depth);
    }
    break;
  case Expr::Kind::Member:
    this->scanExpr(cast<MemberExpr>(expr)->object, scan, depth);
    break;
  case Expr::Kind::Index: {
    auto *index = cast<IndexExpr>(expr);
    this->scanExpr(index->object, scan, depth);
    this->scanExpr(index->index, scan, depth);
    break;
  }
  case Expr::Kind::Unary:
    this->scanExpr(cast<UnaryExpr>(expr)->operand, scan, depth);
    break;
  case Expr::Kind::Binary: {
    auto *bin = cast<BinaryExpr>(expr);
    this->scanExpr(bin->lhs, scan, depth);
    this->scanExpr(bin->rhs, scan, depth);
    break;
  }
  case Expr::Kind::Assign: {
    auto *assign = cast<AssignExpr>(expr);
    scanTarget(assign->target);
    this->scanExpr(assign->value, scan, depth);
    int64_t delta = 0;
    if (assign->op && (*assign->op == BinaryOp::Add || *assign->op == BinaryOp::Sub)) {
      delta = step(assign->value);
      if (*assign->op == BinaryOp::Sub) {
        delta = -delta;
      }
    } else if (auto *bin = dyn_cast<BinaryExpr>(assign->value); !assign->op && bin) {
      if (bin->op == BinaryOp::Add && isName(bin->lhs, assign->target)) {
        delta = step(bin->rhs);
      } else if (bin->op == BinaryOp::Add && isName(bin->rhs, assign->target)) {
        delta = step(bin->lhs);
      } else if (bin->op == BinaryOp::Sub && isName(bin->lhs, assign->target)) {
        delta = -step(bin->rhs);
      }
    }
    change(assign->target, delta);
    break;
  }
  case Expr::Kind::Update: {
    auto *update = cast<UpdateExpr>(expr);
    scanTarget(update->target);
    change(update->target, update->increment ? 1 : -1);
    break;
  }
  case Expr::Kind::Call: {
    auto *call = cast<CallExpr>(expr);
    for (const Expr *arg : call->args) {
      this->scanExpr(arg, scan, depth);
    }
    auto *member = dyn_cast<MemberExpr>(call->callee);
    if (member != nullptr) {
      this->scanMethodCall(call, member, scan, depth);
      break;
    }
    bool knownGlobal = llvm::any_of(kGlobalFunctions, [&](GlobalName global) {
      return this->callChecker->isGlobalName(call->callee, global);
    });
    if (!knownGlobal) {
      scan.callsFunctions = true;
    }
    break;
  }
  case Expr::Kind::New: {
    auto *made = cast<NewExpr>(expr);
    if (const Expr *callee = made->callee) {
      this->scanExpr(callee, scan, depth);
    }
    for (const Expr *arg : made->args) {
      this->scanExpr(arg, scan, depth);
    }
    std::optional<GlobalName> madeGlobal = GlobalNameTable::find(made->name);
    if (madeGlobal != GlobalName::ARRAY) {
      scan.callsFunctions = true;
    }
    break;
  }
  case Expr::Kind::Conditional: {
    auto *cond = cast<ConditionalExpr>(expr);
    this->scanExpr(cond->cond, scan, depth);
    this->scanExpr(cond->thenExpr, scan, depth);
    this->scanExpr(cond->elseExpr, scan, depth);
    break;
  }
  case Expr::Kind::Cast:
    this->scanExpr(cast<CastExpr>(expr)->operand, scan, depth);
    break;
  case Expr::Kind::Sequence:
    for (const Expr *e : cast<SequenceExpr>(expr)->exprs) {
      this->scanExpr(e, scan, depth);
    }
    break;
  case Expr::Kind::OptionalChain:
    this->scanExpr(cast<OptionalChainExpr>(expr)->chain, scan, depth);
    break;
  case Expr::Kind::Function: {
    const FunctionDecl *fn = cast<FunctionExpr>(expr)->fn;
    LoopScanner::declareParams(fn, scan);
    ++scan.functionDepth;
    this->scanStmt(fn->body, scan, depth + 1);
    --scan.functionDepth;
    break;
  }
  case Expr::Kind::Yield:
    if (const Expr *value = cast<YieldExpr>(expr)->value) {
      this->scanExpr(value, scan, depth);
    }
    scan.callsFunctions = true;
    break;
  case Expr::Kind::Await:
    if (!scan.functionDepth) {
      scan.awaits = true;
    }
    this->scanExpr(cast<AwaitExpr>(expr)->value, scan, depth);
    scan.callsFunctions = true;
    break;
  case Expr::Kind::Class:
    scan.callsFunctions = true;
    break;
  case Expr::Kind::RegexLit:
    break;
  }
}

void LoopScanner::scanMethodCall(const CallExpr *call, const MemberExpr *member, LoopScan &scan,
                                 unsigned depth) const {
  if (this->callChecker->isGlobalName(member->object, GlobalName::CONSOLE) ||
      this->callChecker->isGlobalName(member->object, GlobalName::MATH)) {
    return;
  }
  this->scanExpr(member->object, scan, depth);
  const Type *receiver = this->getTypeBeforeLoop(member->object, scan);
  if (receiver != nullptr && receiver->isArray()) {
    LoopScanner::scanArrayMethod(call, member->name, scan);
    return;
  }
  if (receiver != nullptr && receiver->isResult()) {
    LoopScanner::scanResultMethod(call, member->name, scan);
    return;
  }
  this->scanMethodByName(call, member, scan);
}

void LoopScanner::scanMethodByName(const CallExpr *call, const MemberExpr *member,
                                   LoopScan &scan) const {
  llvm::StringRef name = member->name;
  if (this->state.getInventoryStore().hasFieldName(name)) {
    scan.callsFunctions = true;
  }
  LoopScanner::scanArrayMethod(call, name, scan);
  LoopScanner::scanResultMethod(call, name, scan);
  const IteratorMethodInfo *iteratorMethod = IteratorMethodTable::find(name);
  bool drivesIterator =
      iteratorMethod != nullptr && (iteratorMethod->method == IteratorMethod::NEXT ||
                                    iteratorMethod->method == IteratorMethod::TO_ARRAY);
  bool fromIterable = this->callChecker->isGlobalMember(member, GlobalMember::ARRAY_FROM) ||
                      this->callChecker->isGlobalMember(member, GlobalMember::ITERATOR_FROM);
  if (drivesIterator || fromIterable) {
    scan.callsFunctions = true;
  }
}

const Type *LoopScanner::getTypeBeforeLoop(const Expr *expr, const LoopScan &scan) const {
  auto *name = dyn_cast<NameExpr>(expr);
  if (name == nullptr) {
    return nullptr;
  }
  if (scan.declaredInside.contains(name->name)) {
    return nullptr;
  }
  const VarSymbol *var = this->scopes->lookup(name->name).var();
  if (var == nullptr || var->type == nullptr) {
    return nullptr;
  }

  return var->type->nonOptional();
}

void LoopScanner::scanArrayMethod(const CallExpr *call, llvm::StringRef name, LoopScan &scan) {
  const ArrayMethodInfo *method = ArrayMethodTable::find(name);
  if (method == nullptr) {
    return;
  }
  switch (method->form.kind()) {
    case ArrayMethodKind::READING:
      break;
    case ArrayMethodKind::RESIZING: {
      LengthChange change = method->form.as<ResizingArrayMethod>()->change;
      if (change != LengthChange::SHRINKS) {
        scan.growsArrays = true;
      }
      if (change != LengthChange::GROWS) {
        scan.shrinksArrays = true;
        scan.releasesElements = true;
      }
      break;
    }
    case ArrayMethodKind::REWRITING:
      if (method->form.as<RewritingArrayMethod>()->releasesElements) {
        scan.releasesElements = true;
      }
      break;
    case ArrayMethodKind::SORTING:
    case ArrayMethodKind::CALLBACK:
      if (LoopScanner::passesNamedFunction(call, 1)) {
        scan.callsFunctions = true;
      }
      break;
  }
}

void LoopScanner::scanResultMethod(const CallExpr *call, llvm::StringRef name, LoopScan &scan) {
  const ResultMethodInfo *method = ResultMethodTable::find(name);
  if (method == nullptr) {
    return;
  }
  int callbackArgs = ResultMethodTable::countCallbacks(*method);
  if (LoopScanner::passesNamedFunction(call, callbackArgs)) {
    scan.callsFunctions = true;
  }
}

void LoopScanner::declareParams(const FunctionDecl *fn, LoopScan &scan) {
  for (const Param &param : fn->params) {
    scan.declaredInside.insert(param.name);
  }
}

bool LoopScanner::passesNamedFunction(const CallExpr *call, int callbackArgs) {
  size_t checked = std::min(size_t(callbackArgs), call->args.size());
  for (size_t i = 0; i < checked; ++i) {
    if (!isa<FunctionExpr>(call->args[i])) {
      return true;
    }
  }

  return false;
}
}
