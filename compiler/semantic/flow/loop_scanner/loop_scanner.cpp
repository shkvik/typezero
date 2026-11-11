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
using llvm::dyn_cast_or_null;
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
  this->scopes = &all.scopes;
  this->callChecker = &all.callChecker;
  this->keyedFieldChecker = &all.keyedFieldChecker;
}

LoopScan LoopScanner::scanForOfLoop(const ForOfStmt *loop) const {
  PlacedNodes nodes = PlacedNodes::collect({loop->body}, {});
  llvm::StringSet<> declared = LoopScanner::declaredNames(nodes);
  declared.insert(loop->binding.name);
  LoopScan scan = this->scanNodes(nodes, declared);

  return scan;
}

LoopScan LoopScanner::scanLoop(llvm::ArrayRef<const Stmt *> stmts,
                               llvm::ArrayRef<const Expr *> exprs) const {
  PlacedNodes nodes = PlacedNodes::collect(stmts, exprs);
  llvm::StringSet<> declared = LoopScanner::declaredNames(nodes);
  LoopScan scan = this->scanNodes(nodes, declared);

  return scan;
}

LoopScan LoopScanner::scanNodes(const PlacedNodes &nodes, const llvm::StringSet<> &declared) const {
  ArrayEffects arrays = this->arrayEffects(nodes, declared);

  LoopScan scan;
  scan.breaks = LoopScanner::breaksOut(nodes);
  scan.awaits = LoopScanner::awaitsInside(nodes);
  scan.changes = LoopScanner::changedVariables(nodes);
  scan.fieldWrites = this->writtenFields(nodes);
  scan.callsFunctions = this->callsFunctions(nodes, declared);
  scan.growsArrays = arrays.grows;
  scan.shrinksArrays = arrays.shrinks;
  scan.releasesElements = arrays.releases;

  return scan;
}

bool LoopScanner::breaksOut(const PlacedNodes &nodes) {
  for (const PlacedStmt &placed : nodes.getStmts()) {
    auto *exit = dyn_cast<BreakStmt>(placed.stmt);
    if (exit != nullptr && LoopScanner::leavesLoop(exit, placed.place)) {
      return true;
    }
  }

  return false;
}

bool LoopScanner::leavesLoop(const BreakStmt *exit, const TreePlace &place) {
  if (!exit->label.empty()) {
    return !place.hasLabel(exit->label);
  }

  return !place.isInsideLoop() && !place.isInsideFunction() && !place.isInsideSwitch();
}

bool LoopScanner::awaitsInside(const PlacedNodes &nodes) {
  for (const PlacedExpr &placed : nodes.getExprs()) {
    if (isa<AwaitExpr>(placed.expr) && !placed.place.isInsideFunction()) {
      return true;
    }
  }
  for (const PlacedStmt &placed : nodes.getStmts()) {
    auto *loop = dyn_cast<ForOfStmt>(placed.stmt);
    if (loop != nullptr && loop->isAwait && !placed.place.isInsideFunction()) {
      return true;
    }
  }

  return false;
}

llvm::StringSet<> LoopScanner::declaredNames(const PlacedNodes &nodes) {
  llvm::StringSet<> names;
  for (const PlacedStmt &placed : nodes.getStmts()) {
    const Stmt *stmt = placed.stmt;
    if (auto *declaration = dyn_cast<VarDeclStmt>(stmt)) {
      for (const VarDeclarator &declarator : declaration->decls) {
        names.insert(declarator.name);
      }
    }
    if (auto *fn = dyn_cast<FunctionDecl>(stmt)) {
      if (!fn->isExpression() && fn->member() == nullptr) {
        names.insert(fn->name);
      }
      for (const Param &param : fn->params) {
        names.insert(param.name);
      }
    }
    if (auto *loop = dyn_cast<ForOfStmt>(stmt)) {
      names.insert(loop->binding.name);
    }
    if (auto *enumeration = dyn_cast<EnumDecl>(stmt)) {
      names.insert(enumeration->name);
    }
    if (auto *cls = dyn_cast<ClassDecl>(stmt)) {
      names.insert(cls->name);
    }
  }

  return names;
}

llvm::StringMap<LoopScan::Change> LoopScanner::changedVariables(const PlacedNodes &nodes) {
  llvm::StringMap<LoopScan::Change> changes;
  for (const PlacedExpr &placed : nodes.getExprs()) {
    const Expr *target = LoopScanner::changedTarget(placed.expr);
    auto *name = dyn_cast_or_null<NameExpr>(target);
    if (name == nullptr) {
      continue;
    }
    int64_t delta = LoopScanner::changeDelta(placed.expr);
    LoopScan::Change &change = changes[name->name];
    change.count += 1;
    change.delta = change.count == 1 ? delta : 0;
    change.nested = change.nested || placed.place.isNested();
  }

  return changes;
}

const Expr *LoopScanner::changedTarget(const Expr *expr) {
  if (auto *assign = dyn_cast<AssignExpr>(expr)) {
    return assign->target;
  }
  if (auto *update = dyn_cast<UpdateExpr>(expr)) {
    return update->target;
  }

  return nullptr;
}

int64_t LoopScanner::changeDelta(const Expr *expr) {
  if (auto *update = dyn_cast<UpdateExpr>(expr)) {
    return update->increment ? 1 : -1;
  }

  return LoopScanner::assignDelta(cast<AssignExpr>(expr));
}

int64_t LoopScanner::assignDelta(const AssignExpr *assign) {
  if (assign->op == BinaryOp::Add) {
    return LoopScanner::literalStep(assign->value);
  }
  if (assign->op == BinaryOp::Sub) {
    return -LoopScanner::literalStep(assign->value);
  }
  auto *sum = dyn_cast<BinaryExpr>(assign->value);
  if (assign->op.has_value() || sum == nullptr) {
    return 0;
  }
  if (sum->op == BinaryOp::Add && LoopScanner::sameName(sum->lhs, assign->target)) {
    return LoopScanner::literalStep(sum->rhs);
  }
  if (sum->op == BinaryOp::Add && LoopScanner::sameName(sum->rhs, assign->target)) {
    return LoopScanner::literalStep(sum->lhs);
  }
  if (sum->op == BinaryOp::Sub && LoopScanner::sameName(sum->lhs, assign->target)) {
    return -LoopScanner::literalStep(sum->rhs);
  }

  return 0;
}

int64_t LoopScanner::literalStep(const Expr *expr) {
  auto *literal = dyn_cast<IntLitExpr>(expr);
  if (literal == nullptr || literal->value < 1 || literal->value > uint64_t(Type::kMaxLength)) {
    return 0;
  }

  return int64_t(literal->value);
}

bool LoopScanner::sameName(const Expr *expr, const Expr *target) {
  auto *name = dyn_cast<NameExpr>(expr);
  auto *targetName = dyn_cast<NameExpr>(target);

  return name != nullptr && targetName != nullptr && name->name == targetName->name;
}

llvm::StringSet<> LoopScanner::writtenFields(const PlacedNodes &nodes) const {
  llvm::StringSet<> fields;
  auto addField = [&](llvm::StringRef name) { fields.insert(name); };
  for (const PlacedExpr &placed : nodes.getExprs()) {
    const Expr *target = LoopScanner::changedTarget(placed.expr);
    if (auto *index = dyn_cast_or_null<IndexExpr>(target)) {
      this->keyedFieldChecker->keyNames(index->index, addField);
    }
    if (auto *member = dyn_cast_or_null<MemberExpr>(target)) {
      fields.insert(member->name);
    }
  }

  return fields;
}

LoopScanner::ArrayEffects LoopScanner::arrayEffects(const PlacedNodes &nodes,
                                                    const llvm::StringSet<> &declared) const {
  ArrayEffects effects;
  for (const PlacedExpr &placed : nodes.getExprs()) {
    const Expr *target = LoopScanner::changedTarget(placed.expr);
    if (target != nullptr) {
      LoopScanner::addTargetEffects(target, effects);
    }
    if (auto *call = dyn_cast<CallExpr>(placed.expr)) {
      this->addCallEffects(call, declared, effects);
    }
  }

  return effects;
}

void LoopScanner::addTargetEffects(const Expr *target, ArrayEffects &effects) {
  if (isa<IndexExpr>(target)) {
    effects.grows = true;
    effects.releases = true;
    return;
  }
  auto *member = dyn_cast<MemberExpr>(target);
  if (member != nullptr && BuiltinPropertyTable::isName(member->name, BuiltinProperty::LENGTH)) {
    effects.grows = true;
    effects.shrinks = true;
    effects.releases = true;
  }
}

void LoopScanner::addCallEffects(const CallExpr *call, const llvm::StringSet<> &declared,
                                 ArrayEffects &effects) const {
  auto *member = dyn_cast<MemberExpr>(call->callee);
  if (member == nullptr || this->isConsoleOrMath(member)) {
    return;
  }
  const Type *receiver = this->getTypeBeforeLoop(member->object, declared);
  if (receiver != nullptr && receiver->isResult()) {
    return;
  }
  LoopScanner::addArrayMethodEffects(member->name, effects);
}

void LoopScanner::addArrayMethodEffects(llvm::StringRef name, ArrayEffects &effects) {
  const ArrayMethodInfo *method = ArrayMethodTable::find(name);
  if (method == nullptr) {
    return;
  }
  const ResizingArrayMethod *resizing = method->form.as<ResizingArrayMethod>();
  const RewritingArrayMethod *rewriting = method->form.as<RewritingArrayMethod>();
  if (resizing != nullptr && resizing->change != LengthChange::SHRINKS) {
    effects.grows = true;
  }
  if (resizing != nullptr && resizing->change != LengthChange::GROWS) {
    effects.shrinks = true;
    effects.releases = true;
  }
  if (rewriting != nullptr && rewriting->releasesElements) {
    effects.releases = true;
  }
}

bool LoopScanner::callsFunctions(const PlacedNodes &nodes,
                                 const llvm::StringSet<> &declared) const {
  for (const PlacedStmt &placed : nodes.getStmts()) {
    if (this->iteratesInStmt(placed.stmt)) {
      return true;
    }
  }
  for (const PlacedExpr &placed : nodes.getExprs()) {
    if (this->runsOtherCode(placed.expr, declared)) {
      return true;
    }
  }

  return false;
}

bool LoopScanner::iteratesInStmt(const Stmt *stmt) const {
  if (auto *declaration = dyn_cast<VarDeclStmt>(stmt)) {
    for (const VarDeclarator &declarator : declaration->decls) {
      bool isUnpacking = declarator.destructured() && declarator.init != nullptr;
      if (isUnpacking && this->namesIterator(declarator.init)) {
        return true;
      }
    }
  }
  if (auto *loop = dyn_cast<ForOfStmt>(stmt)) {
    return this->namesIterator(loop->iterable);
  }

  return false;
}

bool LoopScanner::runsOtherCode(const Expr *expr, const llvm::StringSet<> &declared) const {
  if (isa<YieldExpr>(expr) || isa<AwaitExpr>(expr) || isa<ClassExpr>(expr)) {
    return true;
  }
  if (auto *made = dyn_cast<NewExpr>(expr)) {
    std::optional<GlobalName> global = GlobalNameTable::find(made->name);
    return global != GlobalName::ARRAY;
  }
  if (auto *list = dyn_cast<ArrayLitExpr>(expr)) {
    for (const ArrayLitExpr::Element &element : list->elements) {
      if (element.spread && this->namesIterator(element.expr)) {
        return true;
      }
    }
    return false;
  }
  if (auto *call = dyn_cast<CallExpr>(expr)) {
    return this->callRunsOtherCode(call, declared);
  }

  return false;
}

bool LoopScanner::callRunsOtherCode(const CallExpr *call, const llvm::StringSet<> &declared) const {
  auto *member = dyn_cast<MemberExpr>(call->callee);
  if (member == nullptr) {
    return !this->isPlainGlobalFunction(call->callee);
  }
  if (this->isConsoleOrMath(member)) {
    return false;
  }
  const Type *receiver = this->getTypeBeforeLoop(member->object, declared);
  if (receiver != nullptr && receiver->isArray()) {
    return LoopScanner::arrayMethodRunsCode(call, member->name);
  }
  if (receiver != nullptr && receiver->isResult()) {
    return LoopScanner::resultMethodRunsCode(call, member->name);
  }

  return this->methodByNameRunsCode(call, member);
}

bool LoopScanner::methodByNameRunsCode(const CallExpr *call, const MemberExpr *member) const {
  llvm::StringRef name = member->name;
  if (this->state.getInventoryStore().hasFieldName(name)) {
    return true;
  }
  bool isKnown =
      LoopScanner::arrayMethodRunsCode(call, name) || LoopScanner::resultMethodRunsCode(call, name);
  if (isKnown) {
    return true;
  }
  const IteratorMethodInfo *iteratorMethod = IteratorMethodTable::find(name);
  bool isDrivingIterator =
      iteratorMethod != nullptr && (iteratorMethod->method == IteratorMethod::NEXT ||
                                    iteratorMethod->method == IteratorMethod::TO_ARRAY);
  bool isFromIterable = this->callChecker->isGlobalMember(member, GlobalMember::ARRAY_FROM) ||
                        this->callChecker->isGlobalMember(member, GlobalMember::ITERATOR_FROM);

  return isDrivingIterator || isFromIterable;
}

bool LoopScanner::arrayMethodRunsCode(const CallExpr *call, llvm::StringRef name) {
  const ArrayMethodInfo *method = ArrayMethodTable::find(name);
  if (method == nullptr) {
    return false;
  }
  bool isTakingFunction =
      method->form.is<CallbackArrayMethod>() || method->form.is<SortingArrayMethod>();

  return isTakingFunction && LoopScanner::passesNamedFunction(call, 1);
}

bool LoopScanner::resultMethodRunsCode(const CallExpr *call, llvm::StringRef name) {
  const ResultMethodInfo *method = ResultMethodTable::find(name);
  if (method == nullptr) {
    return false;
  }
  int callbackArgs = ResultMethodTable::countCallbacks(*method);

  return LoopScanner::passesNamedFunction(call, callbackArgs);
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

bool LoopScanner::isConsoleOrMath(const MemberExpr *member) const {
  return this->callChecker->isGlobalName(member->object, GlobalName::CONSOLE) ||
         this->callChecker->isGlobalName(member->object, GlobalName::MATH);
}

bool LoopScanner::isPlainGlobalFunction(const Expr *callee) const {
  auto named = [&](GlobalName global) { return this->callChecker->isGlobalName(callee, global); };

  return llvm::any_of(kGlobalFunctions, named);
}

bool LoopScanner::namesIterator(const Expr *expr) const {
  auto *name = dyn_cast<NameExpr>(expr);
  if (name == nullptr) {
    return false;
  }
  const VarSymbol *var = this->scopes->lookup(name->name).var();

  return var != nullptr && var->type != nullptr && var->type->nonOptional()->isIterator();
}

const Type *LoopScanner::getTypeBeforeLoop(const Expr *expr,
                                           const llvm::StringSet<> &declared) const {
  auto *name = dyn_cast<NameExpr>(expr);
  if (name == nullptr || declared.contains(name->name)) {
    return nullptr;
  }
  const VarSymbol *var = this->scopes->lookup(name->name).var();
  if (var == nullptr || var->type == nullptr) {
    return nullptr;
  }

  return var->type->nonOptional();
}

}
