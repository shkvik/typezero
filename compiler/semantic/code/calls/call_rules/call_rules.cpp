#include "compiler/semantic/code/calls/call_rules/call_rules.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;

namespace tz {

namespace {

constexpr llvm::StringLiteral kRestPartSuffix = ".rest";

}

CallRules::CallRules(SemanticState &owner)
    : state(owner) {}

void CallRules::nextFunctionNameCall(const FunctionNameDraft &draft) {
  auto &tasks = this->state.walk.tasks;
  auto next = draft;
  CallExpr *call = draft.call;
  switch (draft.stage) {
    case FunctionNameStage::EVENTS:
      next.stage = FunctionNameStage::STREAM;
      tasks.pushTask(AfterFunctionNameTry{.draft = next});
      return tasks.pushTask(
          CheckEventsLibraryCall{.call = call, .name = draft.name, .function = draft.function});
    case FunctionNameStage::STREAM:
      next.stage = FunctionNameStage::DIRECT;
      tasks.pushTask(AfterFunctionNameTry{.draft = next});
      return tasks.pushTask(
          CheckStreamLibraryCall{.call = call, .name = draft.name, .function = draft.function});
    case FunctionNameStage::DIRECT:
      tasks.pushTask(NoteHttpsCall{.call = call, .functionDecl = draft.function->decl});
      return this->state.walk.tasks.pushTask(CheckDirectCall{
          .call = call,
          .function = draft.function,
          .loc = draft.name->loc(),
          .expected = draft.expected,
      });
  }
}

void CallRules::startCalleeValueCall(CallExpr *call) {
  auto *calleeName = dyn_cast<NameExpr>(call->callee);
  this->state.walk.tasks.pushTask(AfterCalleeValue{.call = call, .calleeName = calleeName});

  return this->state.walk.tasks.pushTask(CheckExprSlot{.slot = &call->callee, .expected = nullptr});
}

bool CallRules::rejectSpread(CallExpr *call) {
  for (size_t index = 0; index < call->args.size(); ++index) {
    auto plain = this->checkSpreadFree(call, index);
    if (plain.isErr()) {
      this->state.diagnostics.error(call->args[index]->loc(), plain.error().message);
      call->type = Type::error();
      this->state.walk.tasks.pushChecked(call);
      this->state.walk.tasks.pushTask(WalkArgsLoosely{.call = call});
      return true;
    }
  }

  return false;
}

Expr *CallRules::chainLink(Expr *object) {
  const Type *type = object->type;
  if (type->nonOptional()->isJson()) {
    return this->shortCircuitLink(object, Type::json());
  }
  if (!type->isOptional()) {
    return object;
  }

  return this->shortCircuitLink(object, type->inner());
}

Expr *CallRules::shortCircuitLink(Expr *object, const Type *type) {
  auto *link = this->state.ast.make<CastExpr>(object->loc(), object, std::nullopt);
  link->type = type;
  link->shortCircuit = true;
  this->state.walk.functions.markShortCircuit();

  return link;
}

void CallRules::expandListSpreads(std::vector<Expr *> &args, std::vector<bool> &spread) {
  if (!llvm::is_contained(spread, true)) {
    return;
  }
  auto expanded = ExpandedArgs{.args = {}, .spread = {}, .hasChanged = false};
  for (size_t position = 0; position < args.size(); ++position) {
    bool isSpreading = position < spread.size() && spread[position];
    this->addExpandedArgument(args[position], isSpreading, expanded);
  }
  if (!expanded.hasChanged) {
    return;
  }
  args = std::move(expanded.args);
  bool hasSpread = llvm::is_contained(expanded.spread, true);
  spread = hasSpread ? std::move(expanded.spread) : std::vector<bool>();
}

void CallRules::addExpandedArgument(Expr *arg, bool isSpreading, ExpandedArgs &expanded) {
  auto *name = dyn_cast<NameExpr>(arg);
  VarSymbol *variable =
      isSpreading && name != nullptr ? this->state.walk.scopes.findName(name->name).var() : nullptr;
  auto parts =
      variable != nullptr ? this->state.code.arguments.findArgumentList(variable) : std::nullopt;
  if (parts.has_value()) {
    return this->addListParts(*parts, name->loc(), expanded);
  }
  if (variable != nullptr && this->isFixedTuple(variable->type)) {
    return this->addTupleElements(name, variable->type, expanded);
  }
  expanded.args.push_back(arg);
  expanded.spread.push_back(isSpreading);
}

bool CallRules::isFixedTuple(const Type *type) const {
  if (type == nullptr || !type->isTuple() || type->tupleRest() != nullptr) {
    return false;
  }

  return !this->isSpreadingLists(type);
}

bool CallRules::isSpreadingLists(const Type *type) const {
  if (!type->isTuple()) {
    return false;
  }
  for (const Field &field : type->fields()) {
    if (field.type->isListParam()) {
      return true;
    }
  }

  return false;
}

void CallRules::addListParts(llvm::ArrayRef<llvm::StringRef> parts, llvm::SMLoc loc,
                             ExpandedArgs &expanded) {
  expanded.hasChanged = true;
  for (llvm::StringRef part : parts) {
    auto *partName = this->state.ast.make<NameExpr>(loc, part);
    expanded.args.push_back(partName);
    expanded.spread.push_back(part.ends_with(kRestPartSuffix));
  }
}

void CallRules::addTupleElements(const NameExpr *name, const Type *tuple, ExpandedArgs &expanded) {
  auto &ast = this->state.ast;
  llvm::SMLoc loc = name->loc();
  expanded.hasChanged = true;
  for (size_t position = 0; position < tuple->fields().size(); ++position) {
    auto *index = ast.make<IntLitExpr>(loc, position);
    auto *root = ast.make<NameExpr>(loc, name->name);
    auto *element = ast.make<IndexExpr>(loc, root, index);
    expanded.args.push_back(element);
    expanded.spread.push_back(false);
  }
}

CallRules::ResultNoSpreadPassed CallRules::checkSpreadFree(const CallExpr *call,
                                                           size_t index) const {
  if (call->spreads(index)) {
    auto message = ErrorTextSpread::notRest();

    return err(CodeCallsRegistrySpreadFreeError{
        .code = CodeCallsRegistrySpreadFreeErrorCode::SPREAD_ARGUMENT,
        .message = message,
    });
  }

  return ok(call);
}

}
