#include "compiler/semantic/code/functions/function_methods/function_methods.h"
#include "compiler/program/library/array/array_method_table/array_method_table.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/program/library/function/function_method_table/function_method_table.h"
#include "compiler/semantic/code/calls/code_calls_registry_errors.h"
#include "compiler/semantic/code/functions/code_functions_registry_errors.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;

namespace tz {

namespace {

constexpr llvm::StringLiteral kRestPartSuffix = ".rest";

}

FunctionMethods::FunctionMethods(SemanticState &owner)
    : state(owner) {}

void FunctionMethods::checkThisCall(const CheckThisCall &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  call->type = Type::error();
  auto method = this->checkThisCallShape(call, member);
  if (method.isErr()) {
    tasks.pushChecked(call);
    tasks.pushTask(ReportError{.loc = member->loc(), .message = method.error().message});
    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  const Type *type = member->object->type;
  if (method.value() == FunctionMethod::APPLY) {
    call->spread = {false, true};
    this->expandListSpreads(call->args, call->spread);
  }
  call->callee = member->object;
  call->type = type->ret();
  llvm::ArrayRef<const Type *> params = type->params();
  auto arguments = CheckArguments{
      .call = call,
      .params = std::vector<const Type *>(params.begin(), params.end()),
      .minArgs = type->minArgs(),
      .hasRest = type->hasRest(),
      .loc = member->loc(),
      .checked = {},
  };

  return tasks.pushTask(std::move(arguments));
}

FunctionMethods::ResultThisMethodKnown
FunctionMethods::checkThisCallShape(const CallExpr *call, const MemberExpr *member) const {
  auto method = this->checkThisMethod(member);
  if (method.isErr()) {
    return err(method.error());
  }
  auto arguments = this->checkThisArguments(call, member, method.value());
  if (arguments.isErr()) {
    return err(arguments.error());
  }

  return ok(method.value());
}

FunctionMethods::ResultThisMethodKnown
FunctionMethods::checkThisMethod(const MemberExpr *member) const {
  auto method = FunctionMethodTable::find(member->name);
  if (method == FunctionMethod::BIND) {
    auto message = ErrorTextThisCall::noBind();

    return err(CodeFunctionsRegistryCheckThisCallError{
        .code = CodeFunctionsRegistryCheckThisCallErrorCode::NO_BIND,
        .message = message,
    });
  }
  if (!method.has_value()) {
    auto message = ErrorTextThisCall::noProperty(member->name);

    return err(CodeFunctionsRegistryCheckThisCallError{
        .code = CodeFunctionsRegistryCheckThisCallErrorCode::NO_SUCH_PROPERTY,
        .message = message,
    });
  }

  return ok(*method);
}

FunctionMethods::ResultThisArgumentsFit
FunctionMethods::checkThisArguments(const CallExpr *call, const MemberExpr *member,
                                    FunctionMethod method) const {
  if (call->args.empty()) {
    auto message = ErrorTextThisCall::thisFirst(member->name);

    return err(CodeFunctionsRegistryCheckThisCallError{
        .code = CodeFunctionsRegistryCheckThisCallErrorCode::NO_THIS_ARGUMENT,
        .message = message,
    });
  }
  bool isApplyShape = call->args.size() == 2 && !call->hasSpread();
  if (method == FunctionMethod::APPLY && !isApplyShape) {
    auto message = ErrorTextThisCall::applyArgs();

    return err(CodeFunctionsRegistryCheckThisCallError{
        .code = CodeFunctionsRegistryCheckThisCallErrorCode::APPLY_ARGUMENTS,
        .message = message,
    });
  }

  return ok(call);
}

void FunctionMethods::checkListMethod(const CheckListMethod &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  call->type = Type::error();
  tasks.pushChecked(call);
  auto joining = this->checkJoinMethod(member);
  if (joining.isErr()) {
    tasks.pushTask(ReportError{.loc = member->loc(), .message = joining.error().message});
    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  for (size_t index = 0; index < call->args.size(); ++index) {
    auto plain = this->checkSpreadFree(call, index);
    if (plain.isErr()) {
      this->state.diagnostics.error(call->args[index]->loc(), plain.error().message);
      return tasks.pushTask(WalkArgsLoosely{.call = call});
    }
  }
  auto arity = this->checkJoinArity(call);
  if (arity.isErr()) {
    return this->state.diagnostics.error(member->loc(), arity.error().message);
  }
  call->type = Type::string();
  if (!call->args.empty()) {
    return tasks.pushTask(CheckArgSlot{.slot = &call->args[0], .to = Type::string()});
  }
}

FunctionMethods::ResultJoinMethodKnown
FunctionMethods::checkJoinMethod(const MemberExpr *member) const {
  const auto *method = ArrayMethodTable::find(member->name);
  bool isJoining = method != nullptr && method->method == ArrayMethod::JOIN;
  if (!isJoining) {
    auto message = ErrorTextListMethod::noMethod(member->name);

    return err(CodeFunctionsRegistryCheckListMethodError{
        .code = CodeFunctionsRegistryCheckListMethodErrorCode::NO_SUCH_METHOD,
        .message = message,
    });
  }

  return ok(member);
}

FunctionMethods::ResultNoSpreadPassed FunctionMethods::checkSpreadFree(const CallExpr *call,
                                                                       size_t index) const {
  if (call->spreads(index)) {
    auto message = ErrorTextSpread::notRest();

    return err(CodeFunctionsRegistrySpreadFreeError{
        .code = CodeFunctionsRegistrySpreadFreeErrorCode::SPREAD_ARGUMENT,
        .message = message,
    });
  }

  return ok(call);
}

FunctionMethods::ResultJoinArityFits FunctionMethods::checkJoinArity(const CallExpr *call) const {
  size_t count = call->args.size();
  if (count > 1 && call->tagged) {
    auto message = ErrorTextArity::tagged(0, 1, count);

    return err(CodeFunctionsRegistryJoinArityError{
        .code = CodeFunctionsRegistryJoinArityErrorCode::WRONG_COUNT_TAGGED,
        .message = message,
    });
  }
  if (count > 1) {
    auto message = ErrorTextArity::count(0, 1, count);

    return err(CodeFunctionsRegistryJoinArityError{
        .code = CodeFunctionsRegistryJoinArityErrorCode::WRONG_COUNT,
        .message = message,
    });
  }

  return ok(call);
}

void FunctionMethods::findListPart(const FindListPart &task) {
  auto &tasks = this->state.walk.tasks;
  auto *member = dyn_cast<MemberExpr>(task.expr);
  auto *index = dyn_cast<IndexExpr>(task.expr);
  if (member == nullptr && index == nullptr) {
    return tasks.pushChecked(nullptr);
  }
  const Expr *object = member != nullptr ? member->object : index->object;
  auto parts = this->argumentListOf(object);
  if (!parts.has_value()) {
    return tasks.pushChecked(nullptr);
  }
  if (member != nullptr && BuiltinPropertyTable::isName(member->name, BuiltinProperty::LENGTH)) {
    auto *count = this->state.ast.make<IntLitExpr>(member->loc(), parts->size());
    count->type = Type::intTy();
    return tasks.pushChecked(count);
  }
  auto *position = index != nullptr ? dyn_cast<IntLitExpr>(index->index) : nullptr;
  if (position == nullptr || position->value >= parts->size()) {
    return tasks.pushChecked(nullptr);
  }
  llvm::StringRef part = (*parts)[size_t(position->value)];
  auto *partName = this->state.ast.make<NameExpr>(index->loc(), part);

  return tasks.pushTask(CheckExpr{.expr = partName, .expected = nullptr});
}

void FunctionMethods::checkListArguments(const CheckListArguments &task) {
  auto draft = ListArgumentsDraft{
      .call = task.call,
      .list = task.listParam,
      .parts = this->listParts(task.listParam),
      .loc = task.loc,
      .checked = task.checked,
      .isTyped = true,
      .used = 0,
      .index = task.fixed,
  };

  return this->nextListArgument(NextListArgument{.draft = std::move(draft)});
}

void FunctionMethods::nextListArgument(const NextListArgument &task) {
  const ListArgumentsDraft &draft = task.draft;
  CallExpr *call = draft.call;
  if (draft.index == call->args.size()) {
    return this->finishListArguments(draft);
  }
  if (this->isChecked(draft.checked, draft.index)) {
    return this->afterListArgument(AfterListArgument{.draft = draft});
  }
  bool isSpread = call->spreads(draft.index);
  const auto *expected = this->expectedPart(draft.parts, draft.used, isSpread);
  this->state.walk.tasks.pushTask(AfterListArgument{.draft = draft});

  return this->state.walk.tasks.pushTask(
      CheckExprSlot{.slot = &call->args[draft.index], .expected = expected});
}

void FunctionMethods::afterListArgument(const AfterListArgument &task) {
  auto draft = task.draft;
  CallExpr *call = draft.call;
  Expr *&arg = call->args[draft.index];
  bool isSpread = call->spreads(draft.index);
  draft.index += 1;
  if (arg->type->isError()) {
    draft.isTyped = false;
    return this->nextListArgument(NextListArgument{.draft = std::move(draft)});
  }
  auto fits = this->listArgument(draft.list, draft.parts, draft.used, arg, isSpread);
  if (fits.isErr()) {
    return this->state.diagnostics.error(arg->loc(), fits.error().message);
  }
  const Type *part = draft.parts[draft.used];
  draft.used += 1;
  if (isSpread) {
    return this->nextListArgument(NextListArgument{.draft = std::move(draft)});
  }
  this->state.walk.tasks.pushTask(NextListArgument{.draft = std::move(draft)});

  return this->state.walk.tasks.pushTask(CoerceSlot{.slot = &arg, .to = part, .what = ""});
}

bool FunctionMethods::isChecked(const std::vector<bool> &checked, size_t index) const {
  return index < checked.size() && checked[index];
}

void FunctionMethods::finishListArguments(const ListArgumentsDraft &draft) {
  auto filled = this->listFilled(draft.list, draft.parts, draft.used);
  if (draft.isTyped && filled.isErr()) {
    return this->state.diagnostics.error(draft.loc, filled.error().message);
  }

  return this->state.walk.tasks.pushTask(CalledFunction{});
}

bool FunctionMethods::isSpreadingLists(const Type *type) {
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

void FunctionMethods::expandListSpreads(std::vector<Expr *> &args, std::vector<bool> &spread) {
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

void FunctionMethods::addExpandedArgument(Expr *arg, bool isSpreading, ExpandedArgs &expanded) {
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

bool FunctionMethods::isFixedTuple(const Type *type) const {
  if (type == nullptr || !type->isTuple() || type->tupleRest() != nullptr) {
    return false;
  }

  return !this->isSpreadingLists(type);
}

void FunctionMethods::addListParts(llvm::ArrayRef<llvm::StringRef> parts, llvm::SMLoc loc,
                                   ExpandedArgs &expanded) {
  expanded.hasChanged = true;
  for (llvm::StringRef part : parts) {
    auto *partName = this->state.ast.make<NameExpr>(loc, part);
    expanded.args.push_back(partName);
    expanded.spread.push_back(part.ends_with(kRestPartSuffix));
  }
}

void FunctionMethods::addTupleElements(const NameExpr *name, const Type *tuple,
                                       ExpandedArgs &expanded) {
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

std::optional<std::vector<llvm::StringRef>>
FunctionMethods::argumentListOf(const Expr *object) const {
  auto *name = dyn_cast<NameExpr>(object);
  if (name == nullptr) {
    return std::nullopt;
  }
  VarSymbol *variable = this->state.walk.scopes.findName(name->name).var();
  if (variable == nullptr) {
    return std::nullopt;
  }

  return this->state.code.arguments.findArgumentList(variable);
}

FunctionMethods::ResultArgumentFits
FunctionMethods::listArgument(const Type *list, llvm::ArrayRef<const Type *> parts, size_t used,
                              const Expr *arg, bool isSpread) const {
  if (used >= parts.size()) {
    auto message = ErrorTextListArgument::tooMany(list->name());

    return err(CodeFunctionsRegistryListArgumentError{
        .code = CodeFunctionsRegistryListArgumentErrorCode::TOO_MANY_ARGUMENTS,
        .message = message,
    });
  }
  const Type *part = parts[used];
  if (isSpread && arg->type != part) {
    auto message = ErrorTextListArgument::spreadWrong(arg->type->name(), list->name(), part->name(),
                                                      part->isListParam());

    return err(CodeFunctionsRegistryListArgumentError{
        .code = CodeFunctionsRegistryListArgumentErrorCode::SPREAD_MISMATCH,
        .message = message,
    });
  }
  if (!isSpread && part->isListParam()) {
    auto message = ErrorTextListArgument::notSpread(part->name());

    return err(CodeFunctionsRegistryListArgumentError{
        .code = CodeFunctionsRegistryListArgumentErrorCode::LIST_NOT_SPREAD,
        .message = message,
    });
  }

  return ok(arg);
}

FunctionMethods::ResultListIsFilled FunctionMethods::listFilled(const Type *list,
                                                                llvm::ArrayRef<const Type *> parts,
                                                                size_t used) const {
  if (used < parts.size()) {
    const Type *part = parts[used];
    auto message = ErrorTextListFilled::missing(list->name(), part->name(), part->isListParam());

    return err(CodeFunctionsRegistryListFilledError{
        .code = CodeFunctionsRegistryListFilledErrorCode::MISSING_ARGUMENTS,
        .message = message,
    });
  }

  return ok(list);
}

std::vector<const Type *> FunctionMethods::listParts(const Type *list) const {
  if (list->isListParam()) {
    return {list};
  }
  auto parts = std::vector<const Type *>();
  for (const Field &field : list->fields()) {
    parts.push_back(field.type);
  }

  return parts;
}

const Type *FunctionMethods::expectedPart(llvm::ArrayRef<const Type *> parts, size_t used,
                                          bool isSpread) const {
  if (isSpread || used >= parts.size() || parts[used]->isListParam()) {
    return nullptr;
  }

  return parts[used];
}

}
