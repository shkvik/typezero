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

constexpr int kArrayCallbackArgs = 1;

}

LoopScanner::LoopScanner(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.scopes;
  this->callChecker = &all.callChecker;
  this->keyedFieldChecker = &all.keyedFieldChecker;
}

LoopScan LoopScanner::scanForOfLoop(const ForOfStmt *loop) const {
  auto nodes = PlacedNodes::collect({loop->body}, {});
  auto declared = this->declaredNames(nodes);
  declared.insert(loop->binding.name);
  auto scan = this->scanNodes(nodes, declared);

  return scan;
}

LoopScan LoopScanner::scanLoop(llvm::ArrayRef<const Stmt *> stmts,
                               llvm::ArrayRef<const Expr *> exprs) const {
  auto nodes = PlacedNodes::collect(stmts, exprs);
  auto declared = this->declaredNames(nodes);
  auto scan = this->scanNodes(nodes, declared);

  return scan;
}

LoopScan LoopScanner::scanNodes(const PlacedNodes &nodes, const llvm::StringSet<> &declared) const {
  auto arrays = this->arrayEffects(nodes, declared);

  return LoopScan{
      .changes = this->changedVariables(nodes),
      .isCallingFunctions = this->isCallingFunctions(nodes, declared),
      .isGrowingArrays = arrays.isGrowing,
      .isShrinkingArrays = arrays.isShrinking,
      .fieldWrites = this->writtenFields(nodes),
      .isReleasingElements = arrays.isReleasing,
      .isBreakingOut = this->isBreakingOut(nodes),
      .hasAwait = this->isAwaitingInside(nodes),
  };
}

bool LoopScanner::isBreakingOut(const PlacedNodes &nodes) const {
  for (const PlacedStmt &placed : nodes.getStmts()) {
    auto *exit = dyn_cast<BreakStmt>(placed.stmt);
    if (exit != nullptr && this->isLeavingLoop(exit, placed.place)) {
      return true;
    }
  }

  return false;
}

bool LoopScanner::isLeavingLoop(const BreakStmt *exit, const TreePlace &place) const {
  if (!exit->label.empty()) {
    return !place.hasLabel(exit->label);
  }

  return !place.isInsideLoop() && !place.isInsideFunction() && !place.isInsideSwitch();
}

bool LoopScanner::isAwaitingInside(const PlacedNodes &nodes) const {
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

llvm::StringSet<> LoopScanner::declaredNames(const PlacedNodes &nodes) const {
  auto names = llvm::StringSet<>();
  for (const PlacedStmt &placed : nodes.getStmts()) {
    this->addDeclaredNames(placed.stmt, names);
  }

  return names;
}

void LoopScanner::addDeclaredNames(const Stmt *stmt, llvm::StringSet<> &names) const {
  auto *declaration = dyn_cast<VarDeclStmt>(stmt);
  auto *functionDecl = dyn_cast<FunctionDecl>(stmt);
  auto *loop = dyn_cast<ForOfStmt>(stmt);
  auto *enumeration = dyn_cast<EnumDecl>(stmt);
  auto *classDecl = dyn_cast<ClassDecl>(stmt);
  if (declaration != nullptr) {
    this->addDeclaratorNames(declaration, names);
    return;
  }
  if (functionDecl != nullptr) {
    this->addFunctionNames(functionDecl, names);
    return;
  }
  if (loop != nullptr) {
    names.insert(loop->binding.name);
    return;
  }
  if (enumeration != nullptr) {
    names.insert(enumeration->name);
    return;
  }
  if (classDecl != nullptr) {
    names.insert(classDecl->name);
  }
}

void LoopScanner::addDeclaratorNames(const VarDeclStmt *declaration,
                                     llvm::StringSet<> &names) const {
  for (const VarDeclarator &declarator : declaration->decls) {
    names.insert(declarator.name);
  }
}

void LoopScanner::addFunctionNames(const FunctionDecl *functionDecl,
                                   llvm::StringSet<> &names) const {
  if (!functionDecl->isExpression() && functionDecl->member() == nullptr) {
    names.insert(functionDecl->name);
  }
  for (const Param &param : functionDecl->params) {
    names.insert(param.name);
  }
}

llvm::StringMap<LoopScan::Change> LoopScanner::changedVariables(const PlacedNodes &nodes) const {
  auto changes = llvm::StringMap<LoopScan::Change>();
  for (const PlacedExpr &placed : nodes.getExprs()) {
    const auto *target = this->changedTarget(placed.expr);
    auto *name = dyn_cast_or_null<NameExpr>(target);
    if (name != nullptr) {
      LoopScan::Change &change = changes[name->name];
      this->addChange(change, placed);
    }
  }

  return changes;
}

void LoopScanner::addChange(LoopScan::Change &change, const PlacedExpr &placed) const {
  int64_t delta = this->changeDelta(placed.expr);
  change.count += 1;
  change.delta = change.count == 1 ? delta : 0;
  change.isNested = change.isNested || placed.place.isNested();
}

const Expr *LoopScanner::changedTarget(const Expr *expr) const {
  auto *assign = dyn_cast<AssignExpr>(expr);
  auto *update = dyn_cast<UpdateExpr>(expr);
  if (assign != nullptr) {
    return assign->target;
  }
  if (update != nullptr) {
    return update->target;
  }

  return nullptr;
}

int64_t LoopScanner::changeDelta(const Expr *expr) const {
  auto *update = dyn_cast<UpdateExpr>(expr);
  if (update != nullptr) {
    return update->increment ? 1 : -1;
  }

  return this->assignDelta(cast<AssignExpr>(expr));
}

int64_t LoopScanner::assignDelta(const AssignExpr *assign) const {
  if (assign->op == BinaryOp::Add) {
    return this->literalStep(assign->value);
  }
  if (assign->op == BinaryOp::Sub) {
    return -this->literalStep(assign->value);
  }
  auto *sum = dyn_cast<BinaryExpr>(assign->value);
  if (assign->op.has_value() || sum == nullptr) {
    return 0;
  }
  if (sum->op == BinaryOp::Add && this->isSameName(sum->lhs, assign->target)) {
    return this->literalStep(sum->rhs);
  }
  if (sum->op == BinaryOp::Add && this->isSameName(sum->rhs, assign->target)) {
    return this->literalStep(sum->lhs);
  }
  if (sum->op == BinaryOp::Sub && this->isSameName(sum->lhs, assign->target)) {
    return -this->literalStep(sum->rhs);
  }

  return 0;
}

int64_t LoopScanner::literalStep(const Expr *expr) const {
  auto *literal = dyn_cast<IntLitExpr>(expr);
  if (literal == nullptr || literal->value < 1 || literal->value > uint64_t(Type::kMaxLength)) {
    return 0;
  }

  return int64_t(literal->value);
}

bool LoopScanner::isSameName(const Expr *expr, const Expr *target) const {
  auto *name = dyn_cast<NameExpr>(expr);
  auto *targetName = dyn_cast<NameExpr>(target);

  return name != nullptr && targetName != nullptr && name->name == targetName->name;
}

llvm::StringSet<> LoopScanner::writtenFields(const PlacedNodes &nodes) const {
  auto fields = llvm::StringSet<>();
  for (const PlacedExpr &placed : nodes.getExprs()) {
    const auto *target = this->changedTarget(placed.expr);
    if (target != nullptr) {
      this->addWrittenFields(target, fields);
    }
  }

  return fields;
}

void LoopScanner::addWrittenFields(const Expr *target, llvm::StringSet<> &fields) const {
  auto *index = dyn_cast<IndexExpr>(target);
  auto *member = dyn_cast<MemberExpr>(target);
  if (index != nullptr) {
    this->addKeyNames(index, fields);
    return;
  }
  if (member != nullptr) {
    fields.insert(member->name);
  }
}

void LoopScanner::addKeyNames(const IndexExpr *index, llvm::StringSet<> &fields) const {
  auto names = this->keyedFieldChecker->keyNames(index->index);
  for (const std::string &name : names) {
    fields.insert(name);
  }
}

LoopScanner::ArrayEffects LoopScanner::arrayEffects(const PlacedNodes &nodes,
                                                    const llvm::StringSet<> &declared) const {
  auto effects = ArrayEffects{};
  for (const PlacedExpr &placed : nodes.getExprs()) {
    const auto *target = this->changedTarget(placed.expr);
    auto *call = dyn_cast<CallExpr>(placed.expr);
    if (target != nullptr) {
      this->addTargetEffects(target, effects);
    }
    if (call != nullptr) {
      this->addCallEffects(call, declared, effects);
    }
  }

  return effects;
}

void LoopScanner::addTargetEffects(const Expr *target, ArrayEffects &effects) const {
  auto *member = dyn_cast<MemberExpr>(target);
  bool isLength =
      member != nullptr && BuiltinPropertyTable::isName(member->name, BuiltinProperty::LENGTH);
  if (isa<IndexExpr>(target)) {
    effects.isGrowing = true;
    effects.isReleasing = true;
    return;
  }
  if (isLength) {
    effects.isGrowing = true;
    effects.isShrinking = true;
    effects.isReleasing = true;
  }
}

void LoopScanner::addCallEffects(const CallExpr *call, const llvm::StringSet<> &declared,
                                 ArrayEffects &effects) const {
  auto *member = dyn_cast<MemberExpr>(call->callee);
  if (member == nullptr || this->isConsoleOrMath(member)) {
    return;
  }
  const auto *receiver = this->getTypeBeforeLoop(member->object, declared);
  if (receiver != nullptr && receiver->isResult()) {
    return;
  }
  this->addArrayMethodEffects(member->name, effects);
}

void LoopScanner::addArrayMethodEffects(llvm::StringRef name, ArrayEffects &effects) const {
  const auto *method = ArrayMethodTable::find(name);
  if (method == nullptr) {
    return;
  }
  const ResizingArrayMethod *resizing = method->form.as<ResizingArrayMethod>();
  const RewritingArrayMethod *rewriting = method->form.as<RewritingArrayMethod>();
  if (resizing != nullptr && resizing->change != LengthChange::SHRINKS) {
    effects.isGrowing = true;
  }
  if (resizing != nullptr && resizing->change != LengthChange::GROWS) {
    effects.isShrinking = true;
    effects.isReleasing = true;
  }
  if (rewriting != nullptr && rewriting->releasesElements) {
    effects.isReleasing = true;
  }
}

bool LoopScanner::isCallingFunctions(const PlacedNodes &nodes,
                                     const llvm::StringSet<> &declared) const {
  for (const PlacedStmt &placed : nodes.getStmts()) {
    if (this->isIteratingInStmt(placed.stmt)) {
      return true;
    }
  }
  for (const PlacedExpr &placed : nodes.getExprs()) {
    if (this->isRunningOtherCode(placed.expr, declared)) {
      return true;
    }
  }

  return false;
}

bool LoopScanner::isIteratingInStmt(const Stmt *stmt) const {
  auto *declaration = dyn_cast<VarDeclStmt>(stmt);
  auto *loop = dyn_cast<ForOfStmt>(stmt);
  if (declaration != nullptr) {
    return this->isUnpackingIterator(declaration);
  }
  if (loop != nullptr) {
    return this->isNamingIterator(loop->iterable);
  }

  return false;
}

bool LoopScanner::isUnpackingIterator(const VarDeclStmt *declaration) const {
  for (const VarDeclarator &declarator : declaration->decls) {
    bool isUnpacking = declarator.destructured() && declarator.init != nullptr;
    if (isUnpacking && this->isNamingIterator(declarator.init)) {
      return true;
    }
  }

  return false;
}

bool LoopScanner::isRunningOtherCode(const Expr *expr, const llvm::StringSet<> &declared) const {
  auto *made = dyn_cast<NewExpr>(expr);
  auto *list = dyn_cast<ArrayLitExpr>(expr);
  auto *call = dyn_cast<CallExpr>(expr);
  bool isOpaque = isa<YieldExpr>(expr) || isa<AwaitExpr>(expr) || isa<ClassExpr>(expr);
  if (isOpaque) {
    return true;
  }
  if (made != nullptr) {
    return this->isMakingNonArray(made);
  }
  if (list != nullptr) {
    return this->isSpreadingIterator(list);
  }
  if (call != nullptr) {
    return this->isCallRunningOtherCode(call, declared);
  }

  return false;
}

bool LoopScanner::isMakingNonArray(const NewExpr *made) const {
  auto global = GlobalNameTable::find(made->name);

  return global != GlobalName::ARRAY;
}

bool LoopScanner::isSpreadingIterator(const ArrayLitExpr *list) const {
  for (const ArrayLitExpr::Element &element : list->elements) {
    if (element.spread && this->isNamingIterator(element.expr)) {
      return true;
    }
  }

  return false;
}

bool LoopScanner::isCallRunningOtherCode(const CallExpr *call,
                                         const llvm::StringSet<> &declared) const {
  auto *member = dyn_cast<MemberExpr>(call->callee);
  if (member == nullptr) {
    return !this->isPlainGlobalFunction(call->callee);
  }
  if (this->isConsoleOrMath(member)) {
    return false;
  }
  const auto *receiver = this->getTypeBeforeLoop(member->object, declared);
  if (receiver != nullptr && receiver->isArray()) {
    return this->isArrayMethodRunningCode(call, member->name);
  }
  if (receiver != nullptr && receiver->isResult()) {
    return this->isResultMethodRunningCode(call, member->name);
  }

  return this->isMethodByNameRunningCode(call, member);
}

bool LoopScanner::isMethodByNameRunningCode(const CallExpr *call, const MemberExpr *member) const {
  llvm::StringRef name = member->name;
  if (this->state.getInventoryStore().hasFieldName(name)) {
    return true;
  }
  bool isKnown =
      this->isArrayMethodRunningCode(call, name) || this->isResultMethodRunningCode(call, name);
  if (isKnown) {
    return true;
  }
  const auto *iteratorMethod = IteratorMethodTable::find(name);
  bool isDrivingIterator =
      iteratorMethod != nullptr && (iteratorMethod->method == IteratorMethod::NEXT ||
                                    iteratorMethod->method == IteratorMethod::TO_ARRAY);
  bool isFromIterable = this->callChecker->isGlobalMember(member, GlobalMember::ARRAY_FROM) ||
                        this->callChecker->isGlobalMember(member, GlobalMember::ITERATOR_FROM);

  return isDrivingIterator || isFromIterable;
}

bool LoopScanner::isArrayMethodRunningCode(const CallExpr *call, llvm::StringRef name) const {
  const auto *method = ArrayMethodTable::find(name);
  if (method == nullptr) {
    return false;
  }
  bool isTakingFunction =
      method->form.is<CallbackArrayMethod>() || method->form.is<SortingArrayMethod>();

  return isTakingFunction && this->isPassingNamedFunction(call, kArrayCallbackArgs);
}

bool LoopScanner::isResultMethodRunningCode(const CallExpr *call, llvm::StringRef name) const {
  const auto *method = ResultMethodTable::find(name);
  if (method == nullptr) {
    return false;
  }
  int callbackArgs = ResultMethodTable::countCallbacks(*method);

  return this->isPassingNamedFunction(call, callbackArgs);
}

bool LoopScanner::isPassingNamedFunction(const CallExpr *call, int callbackArgs) const {
  size_t checked = std::min(size_t(callbackArgs), call->args.size());
  for (size_t index = 0; index < checked; ++index) {
    if (!isa<FunctionExpr>(call->args[index])) {
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
  for (GlobalName global : kGlobalFunctions) {
    if (this->callChecker->isGlobalName(callee, global)) {
      return true;
    }
  }

  return false;
}

bool LoopScanner::isNamingIterator(const Expr *expr) const {
  auto *name = dyn_cast<NameExpr>(expr);
  if (name == nullptr) {
    return false;
  }
  const VarSymbol *variable = this->scopes->lookup(name->name).var();

  return variable != nullptr && variable->type != nullptr &&
         variable->type->nonOptional()->isIterator();
}

const Type *LoopScanner::getTypeBeforeLoop(const Expr *expr,
                                           const llvm::StringSet<> &declared) const {
  auto *name = dyn_cast<NameExpr>(expr);
  if (name == nullptr || declared.contains(name->name)) {
    return nullptr;
  }
  const VarSymbol *variable = this->scopes->lookup(name->name).var();
  if (variable == nullptr || variable->type == nullptr) {
    return nullptr;
  }

  return variable->type->nonOptional();
}

}
