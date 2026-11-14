#include "compiler/semantic/code/loops/for_of_loops/for_of_loops.h"
#include "compiler/program/library/builtin_names/global_member_table/global_member_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/builtin_names/library_class_table/library_class_table.h"
#include "compiler/program/library/process/process_member_table/process_member_table.h"
#include "compiler/program/types/union_slots/union_slots.h"
#include "compiler/semantic/bodies/objects/object_check_helpers/object_check_helpers.h"
#include "compiler/semantic/flow/facts/facts.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr const char *kStringPlaceholder = "s";
constexpr const char *kArrayPlaceholder = "a";

}

ForOfLoops::ForOfLoops(SemanticState &owner)
    : state(owner) {}

void ForOfLoops::startScannedBody(Stmt *body, const LoopScan &scan) {
  this->pushTarget(JumpTarget::Kind::Loop);
  if (scan.hasAwait) {
    this->state.walk.functions.enterAwaitLoop();
  }
  this->state.walk.tasks.pushTask(LeaveLoopBody{.isAwait = scan.hasAwait});

  return this->state.walk.tasks.pushTask(WalkBody{.body = body});
}

void ForOfLoops::walkForOf(const WalkForOf &task) {
  ForOfStmt *stmt = task.forOf;
  if (stmt->isAwait) {
    this->state.walk.tasks.pushTask(AfterForAwait{.forOf = stmt});
    return this->state.walk.tasks.pushTask(WalkForAwait{.forOf = stmt});
  }
  this->state.walk.tasks.pushTask(AfterForOfSource{.forOf = stmt});
  if (stmt->keys) {
    return this->startForIn(stmt);
  }

  return this->startIterable(stmt);
}

void ForOfLoops::afterForAwait(const AfterForAwait &task) {
  return this->state.walk.tasks.pushTask(NoteErrorsHandled{.expr = task.forOf->iterable});
}

void ForOfLoops::startIterable(ForOfStmt *stmt) {
  this->state.walk.tasks.pushTask(AfterIterable{.forOf = stmt});
  this->state.walk.tasks.pushTask(IterableOfSlot{.slot = &stmt->iterable});
  this->state.walk.tasks.pushTask(ApparentSlot{.slot = &stmt->iterable});
  this->state.walk.tasks.pushTask(CheckExprSlot{.slot = &stmt->iterable, .expected = nullptr});

  return this->state.walk.tasks.pushTask(MarkFused{.expr = stmt->iterable});
}

void ForOfLoops::afterIterable(const AfterIterable &task) {
  ForOfStmt *stmt = task.forOf;
  auto *name = dyn_cast<NameExpr>(stmt->iterable);
  if (name == nullptr || !this->isListParam(name)) {
    return;
  }
  auto *elements = this->listElements(name);
  if (elements == nullptr) {
    return;
  }
  const Type *expected = this->listElementsType(name);
  stmt->iterable = elements;

  return this->state.walk.tasks.pushTask(
      CheckExprSlot{.slot = &stmt->iterable, .expected = expected});
}

ArrayLitExpr *ForOfLoops::listElements(const NameExpr *name) {
  auto &ast = this->state.ast;
  llvm::SMLoc loc = name->loc();
  size_t count = name->type->fields().size();
  if (count == 0) {
    return nullptr;
  }
  auto *elements = ast.make<ArrayLitExpr>(loc);
  for (size_t index = 0; index < count; ++index) {
    auto *root = ast.make<NameExpr>(loc, name->name);
    auto *position = ast.make<IntLitExpr>(loc, index);
    auto *item = ast.make<IndexExpr>(loc, root, position);
    auto element = ArrayLitExpr::Element{.expr = item, .spread = false};
    elements->elements.push_back(element);
  }

  return elements;
}

const Type *ForOfLoops::listElementsType(const NameExpr *name) const {
  auto kinds = std::vector<const Type *>();
  for (const Field &field : name->type->fields()) {
    kinds.push_back(field.type);
  }
  const Type *element = Type::unionOf(kinds);
  bool isTyped = element != nullptr && UnionSlots::unionProblem(element).empty();

  return isTyped ? Type::array(element) : nullptr;
}

void ForOfLoops::startForIn(ForOfStmt *stmt) {
  auto *variable = dyn_cast<NameExpr>(stmt->iterable);
  bool isEnv = this->state.library.process.isProcessMember(stmt->iterable, ProcessMember::ENV);
  this->state.walk.tasks.pushTask(
      AfterForInObject{.forOf = stmt, .variable = variable, .isEnv = isEnv});

  return this->state.walk.tasks.pushTask(
      CheckExprSlot{.slot = &stmt->iterable, .expected = nullptr});
}

void ForOfLoops::afterForInObject(const AfterForInObject &task) {
  ForOfStmt *stmt = task.forOf;
  Expr *object = stmt->iterable;
  const Type *type = object->type;
  bool isRecordLoop = type->isRecord() && (task.variable != nullptr || task.isEnv) &&
                      !isa<VarDeclStmt>(stmt->body) && !isa<FunctionDecl>(stmt->body);
  if (isRecordLoop) {
    this->addKeyGuard(stmt, task.variable);
  }
  auto text = this->forInText(object, type);
  auto allowed = this->checkWalkable(type, text);
  if (allowed.isErr()) {
    this->state.diagnostics.error(object->loc(), allowed.error().message);
  }
  bool isWalking = allowed.isOk() && !type->isError();
  stmt->iterable = this->forInKeys(object, isWalking);
}

void ForOfLoops::afterForOfSource(const AfterForOfSource &task) {
  ForOfStmt *stmt = task.forOf;
  if (!stmt->iterable->type->isMatch()) {
    return this->bindForOf(BindForOf{.forOf = stmt});
  }
  this->state.walk.tasks.pushTask(BindForOf{.forOf = stmt});
  this->state.walk.tasks.pushTask(WriteCheckedSlot{.slot = &stmt->iterable});
  this->state.walk.tasks.pushChecked(stmt->iterable);

  return this->state.walk.tasks.pushTask(MatchItems{});
}

void ForOfLoops::bindForOf(const BindForOf &task) {
  ForOfStmt *stmt = task.forOf;
  const Type *type = stmt->iterable->type;
  const auto *iterated = this->iteratedElement(stmt);
  const Type *element = this->boundElement(stmt, iterated);
  auto scan = this->state.flow.loops.scanForOfLoop(stmt);
  if (type->isIterator()) {
    scan.isCallingFunctions = true;
  }
  bool isOwnedList = stmt->isConst && type->isArray() && element->isManaged();
  stmt->borrowElements = isOwnedList && !scan.isCallingFunctions && !scan.isReleasingElements;
  auto head = this->state.flow.loops.loopHead(scan);
  this->state.walk.flow.replaceFlow(head);
  this->state.walk.scopes.openScope();
  VarDeclarator &binding = stmt->binding;
  this->state.walk.tasks.pushTask(AfterForOfBinding{
      .forOf = stmt,
      .element = element,
      .scan = std::move(scan),
      .head = std::move(head),
  });

  return this->state.walk.tasks.pushTask(DeclareVariableSlot{
      .slot = &binding.sym,
      .name = binding.name,
      .loc = binding.loc,
      .type = element,
      .isConst = stmt->isConst,
  });
}

void ForOfLoops::afterForOfBinding(const AfterForOfBinding &task) {
  this->addLiteralBounds(task.forOf, task.element);
  this->state.walk.tasks.pushTask(RestoreLoopFlow{.flow = task.head});
  this->state.walk.tasks.pushTask(CloseScope{});

  return this->startScannedBody(task.forOf->body, task.scan);
}

const Type *ForOfLoops::iteratedElement(ForOfStmt *stmt) {
  const Type *type = stmt->iterable->type;
  if (type->isArray()) {
    return type->inner();
  }
  if (type->isString()) {
    return Type::string();
  }
  if (type->isTypedArray()) {
    return type->elementType();
  }
  if (type->isIterator()) {
    auto label = ErrorTextForOf::loop();
    auto nothing = this->state.code.iteration.checkSendsNothing(type, label);
    if (nothing.isErr()) {
      this->state.diagnostics.error(stmt->iterable->loc(), nothing.error().message);
    }

    return type->yielded();
  }
  auto *copy = this->arrayCopy(stmt->iterable);
  if (copy != nullptr) {
    stmt->iterable = copy;

    return copy->type->inner();
  }
  auto iterable = this->iterableType(type);
  if (iterable.isErr()) {
    this->state.diagnostics.error(stmt->iterable->loc(), iterable.error().message);
  }

  return Type::error();
}

Expr *ForOfLoops::arrayCopy(Expr *tuple) {
  const Type *type = tuple->type;
  if (type == nullptr || !type->isTuple() || type->tupleRest() != nullptr ||
      type->fields().empty() || type->tupleMin() != type->fields().size()) {
    return nullptr;
  }
  const Type *element = type->fields().front().type;
  for (const Field &field : type->fields()) {
    if (field.type != element) {
      return nullptr;
    }
  }
  auto *copy = this->state.ast.make<ArrayLitExpr>(tuple->loc());
  copy->elements.push_back(ArrayLitExpr::Element{.expr = tuple, .spread = true});
  copy->type = Type::array(element);

  return copy;
}

bool ForOfLoops::isOptionalIterable(const Type *type) const {
  if (!type->isOptional()) {
    return false;
  }
  const auto *element = this->state.code.iteration.iteratedType(type->inner());

  return element != nullptr;
}

bool ForOfLoops::isAsyncOnly(const Type *type) const {
  if (!type->isRuntimeObject()) {
    return false;
  }
  const auto *element = this->state.code.iteration.asyncIteratedType(type);

  return element != nullptr;
}

void ForOfLoops::pushTarget(JumpTarget::Kind kind) {
  auto labels = this->state.walk.block.takePendingLabels();
  auto breaks = Flow();
  breaks.unreachable = true;
  auto target = JumpTarget{.kind = kind, .labels = std::move(labels), .breaks = std::move(breaks)};
  this->state.walk.block.enterJumpTarget(std::move(target));
}

const Type *ForOfLoops::boundElement(const ForOfStmt *stmt, const Type *element) {
  auto typed = this->bindingType(stmt);
  if (typed.isErr()) {
    this->state.diagnostics.error(stmt->binding.typeAnn->loc, typed.error().message);
  }
  auto shape = this->bindingShape(stmt, element);
  if (shape.isErr()) {
    this->state.diagnostics.error(stmt->binding.loc, shape.error().message);

    return Type::error();
  }

  return element;
}

bool ForOfLoops::isListParam(const NameExpr *name) {
  bool isTuple =
      name->var() != nullptr && name->type->isTuple() && name->type->tupleRest() == nullptr;
  if (!isTuple) {
    return false;
  }
  for (const FunctionDecl *functionDecl : this->state.walk.functions.getFunctions()) {
    if (functionDecl->listName == name->name && name->var()->owner() == functionDecl) {
      return true;
    }
  }

  return false;
}

void ForOfLoops::addLiteralBounds(const ForOfStmt *stmt, const Type *element) {
  auto *literal = dyn_cast<ArrayLitExpr>(stmt->iterable);
  bool isBounded = literal != nullptr && element->isInt() && stmt->binding.sym != nullptr &&
                   !stmt->binding.destructured() && !literal->elements.empty();
  if (!isBounded) {
    return;
  }
  auto range = this->literalRange(literal);
  if (!range.lo.has_value()) {
    return;
  }
  auto &facts = this->state.walk.flow.getFacts();
  auto self = Term::of(stmt->binding.sym);
  auto belowLowest = Facts::saturatingSub(1, *range.lo);
  auto aboveHighest = Facts::saturatingAdd(*range.hi, 1);
  facts.add(Term::zero(), self, belowLowest);
  facts.add(self, Term::zero(), aboveHighest);
}

IntegerRange ForOfLoops::literalRange(const ArrayLitExpr *literal) const {
  IntegerRange range;
  for (const ArrayLitExpr::Element &element : literal->elements) {
    auto value = this->elementConstant(element);
    if (!value.has_value()) {
      return {};
    }
    range.lo = range.lo.has_value() ? std::min(*range.lo, *value) : *value;
    range.hi = range.hi.has_value() ? std::max(*range.hi, *value) : *value;
  }

  return range;
}

std::optional<int64_t> ForOfLoops::elementConstant(const ArrayLitExpr::Element &element) const {
  if (element.spread || element.hole) {
    return std::nullopt;
  }

  return this->state.code.constants.intConstant(element.expr);
}

ForOfLoops::ResultIterableType ForOfLoops::iterableType(const Type *type) const {
  if (this->isOptionalIterable(type)) {
    auto message = ErrorTextForOf::mayBeUndef(type->name());

    return err(CodeLoopsRegistryIterableTypeError{
        .code = CodeLoopsRegistryIterableTypeErrorCode::MAY_BE_UNDEFINED,
        .message = message,
    });
  }
  if (type->isTuple()) {
    auto message = ErrorTextForOf::tuple();

    return err(CodeLoopsRegistryIterableTypeError{
        .code = CodeLoopsRegistryIterableTypeErrorCode::TUPLE_NOT_ITERABLE,
        .message = message,
    });
  }
  if (type->isRecord()) {
    auto message = ErrorTextForOf::record();

    return err(CodeLoopsRegistryIterableTypeError{
        .code = CodeLoopsRegistryIterableTypeErrorCode::RECORD_NOT_ITERABLE,
        .message = message,
    });
  }
  if (this->isAsyncOnly(type)) {
    auto message = ErrorTextForOf::later(type->name());

    return err(CodeLoopsRegistryIterableTypeError{
        .code = CodeLoopsRegistryIterableTypeErrorCode::ASYNC_ONLY,
        .message = message,
    });
  }
  if (this->isConcurrentMap(type)) {
    auto message = ErrorTextForOf::concurrent();

    return err(CodeLoopsRegistryIterableTypeError{
        .code = CodeLoopsRegistryIterableTypeErrorCode::CONCURRENT_MAP,
        .message = message,
    });
  }
  if (!type->isError()) {
    auto message = ErrorTextForOf::cannot(type->name());

    return err(CodeLoopsRegistryIterableTypeError{
        .code = CodeLoopsRegistryIterableTypeErrorCode::NOT_ITERABLE,
        .message = message,
    });
  }

  return ok(type);
}

bool ForOfLoops::isConcurrentMap(const Type *type) const {
  const ClassDecl *classDecl = type->classDecl();
  if (classDecl == nullptr || !classDecl->library() || classDecl->generic() == nullptr) {
    return false;
  }

  return LibraryClassTable::isName(classDecl->generic()->name, LibraryClass::CONCURRENT_MAP);
}

ForOfLoops::ResultBindingTyped ForOfLoops::bindingType(const ForOfStmt *stmt) const {
  bool isAnnotated = stmt->binding.typeAnn.has_value();
  if (isAnnotated && stmt->keys) {
    auto message = ErrorTextLoopBinding::forIn();

    return err(CodeLoopsRegistryBindingTypeError{
        .code = CodeLoopsRegistryBindingTypeErrorCode::FOR_IN_ANNOTATED,
        .message = message,
    });
  }
  if (isAnnotated) {
    auto message = ErrorTextLoopBinding::forOf();

    return err(CodeLoopsRegistryBindingTypeError{
        .code = CodeLoopsRegistryBindingTypeErrorCode::FOR_OF_ANNOTATED,
        .message = message,
    });
  }

  return ok(&stmt->binding);
}

ForOfLoops::ResultBindingShape ForOfLoops::bindingShape(const ForOfStmt *stmt,
                                                        const Type *element) const {
  bool isListLike =
      element->isArray() || element->isTuple() || element->isMatch() || element->isError();
  bool isFitting = !stmt->binding.destructured() || stmt->binding.objectPattern() || isListLike;
  if (!isFitting) {
    auto message = ErrorTextLoopShape::notArray(element->name());

    return err(CodeLoopsRegistryBindingShapeError{
        .code = CodeLoopsRegistryBindingShapeErrorCode::NOT_AN_ARRAY,
        .message = message,
    });
  }

  return ok(element);
}

ForOfLoops::ResultForInKeysAllowed ForOfLoops::checkWalkable(const Type *type,
                                                             llvm::StringRef text) const {
  if (type->isArray() || type->isString() || type->isTuple()) {
    auto what = this->indexedKindText(type);
    auto message = ErrorTextForIn::indices(what, text, type->isArray());

    return err(CodeLoopsRegistryWalkableError{
        .code = CodeLoopsRegistryWalkableErrorCode::INDICES_AS_STRINGS,
        .message = message,
    });
  }
  if (type->isOptional() && type->inner()->isObject()) {
    auto message = ErrorTextForIn::undefined();

    return err(CodeLoopsRegistryWalkableError{
        .code = CodeLoopsRegistryWalkableErrorCode::MAY_BE_UNDEFINED,
        .message = message,
    });
  }
  if (type->nonOptional()->isJson() || type->isRecord()) {
    return ok(type);
  }
  if (type->isDict()) {
    auto message = ErrorTextForIn::dict(text, type->isMap());

    return err(CodeLoopsRegistryWalkableError{
        .code = CodeLoopsRegistryWalkableErrorCode::DICT_PROPERTIES,
        .message = message,
    });
  }
  if (type->isOptional() && type->inner()->isRecord()) {
    auto message = ErrorTextForIn::undefined();

    return err(CodeLoopsRegistryWalkableError{
        .code = CodeLoopsRegistryWalkableErrorCode::MAY_BE_UNDEFINED,
        .message = message,
    });
  }
  bool isNotObject = (!type->isObject() || type->isRegExp() || type->isMatch() || type->isDate() ||
                      type->isIntl() || type->isBytes() || type->isCrypto()) &&
                     !type->isError();
  if (isNotObject) {
    auto message = ErrorTextForIn::notObject(type->name());

    return err(CodeLoopsRegistryWalkableError{
        .code = CodeLoopsRegistryWalkableErrorCode::NOT_AN_OBJECT,
        .message = message,
    });
  }

  return ok(type);
}

std::string ForOfLoops::indexedKindText(const Type *type) const {
  if (type->isArray()) {
    return ErrorTextForIn::anArray();
  }
  if (type->isString()) {
    return ErrorTextForIn::aString();
  }

  return ErrorTextForIn::aTuple();
}

void ForOfLoops::addKeyGuard(ForOfStmt *stmt, const NameExpr *variable) {
  llvm::SMLoc loc = stmt->body->loc();
  auto *again = this->recordAgain(loc, variable);
  auto *key = this->state.ast.make<NameExpr>(loc, stmt->binding.name);
  auto *has = this->state.ast.make<BinaryExpr>(loc, BinaryOp::In, key, again);
  auto *missing = this->state.ast.make<UnaryExpr>(loc, UnaryOp::Not, has);
  auto *next = this->state.ast.make<ContinueStmt>(loc, "");
  auto *skip = this->state.ast.make<IfStmt>(loc, missing, next, nullptr);
  auto *block = this->bodyBlock(stmt);
  block->body.insert(block->body.begin(), skip);
}

Expr *ForOfLoops::recordAgain(llvm::SMLoc loc, const NameExpr *variable) {
  if (variable != nullptr) {
    return this->state.ast.make<NameExpr>(loc, variable->name);
  }
  auto processName = GlobalNameTable::getName(GlobalName::PROCESS);
  auto *process = this->state.ast.make<NameExpr>(loc, processName);
  auto envName = ProcessMemberTable::getName(ProcessMember::ENV);

  return this->state.ast.make<MemberExpr>(loc, process, envName);
}

BlockStmt *ForOfLoops::bodyBlock(ForOfStmt *stmt) {
  auto *block = dyn_cast<BlockStmt>(stmt->body);
  if (block != nullptr) {
    return block;
  }
  auto *wrapped = this->state.ast.make<BlockStmt>(stmt->body->loc());
  wrapped->body.push_back(stmt->body);
  stmt->body = wrapped;

  return wrapped;
}

std::string ForOfLoops::forInText(const Expr *object, const Type *type) const {
  const Expr *unnarrowed = ObjectCheckHelpers::unnarrowed(object);
  auto *name = dyn_cast<NameExpr>(unnarrowed);
  if (name != nullptr) {
    return name->name.str();
  }

  return type->isString() ? kStringPlaceholder : kArrayPlaceholder;
}

Expr *ForOfLoops::forInKeys(Expr *object, bool isWalking) {
  llvm::SMLoc loc = object->loc();
  auto objectName = GlobalNameTable::getName(GlobalName::OBJECT);
  auto keysName = GlobalMemberTable::getName(GlobalMember::OBJECT_KEYS);
  auto *owner = this->state.ast.make<NameExpr>(loc, objectName);
  auto *callee = this->state.ast.make<MemberExpr>(loc, owner, keysName);
  auto *keys = this->state.ast.make<CallExpr>(loc, callee, std::vector<Expr *>{object});
  keys->target = BuiltinCall{.builtin = Builtin::ObjectKeys};
  keys->type = isWalking ? Type::array(Type::string()) : Type::error();
  keys->forIn = true;

  return keys;
}

}
