#include "compiler/semantic/library_v2/path/library_path_registry.h"
#include "compiler/program/library/builtin_modules/known_modules/known_modules.h"
#include "compiler/program/library/path/path_shape_field_table/path_shape_field_table.h"
#include "compiler/program/library/path/path_shape_name_table/path_shape_name_table.h"
#include "compiler/semantic/library/builtin_modules/node_check_helpers/node_check_helpers.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

LibraryPathRegistry::LibraryPathRegistry(SemanticState &owner)
    : state(owner) {}

const Type *LibraryPathRegistry::parsedPathType() const {
  auto typeName = PathShapeNameTable::getName(PathShapeName::PARSED_PATH);

  return this->pathObjectType(Type::string(), typeName);
}

const Type *LibraryPathRegistry::pathObjectType(const Type *part, llvm::StringRef typeName) const {
  auto fields = std::vector<Field>();
  for (PathShapeField field :
       {PathShapeField::ROOT, PathShapeField::DIRECTORY, PathShapeField::BASE,
        PathShapeField::EXTENSION, PathShapeField::NAME}) {
    auto name = PathShapeFieldTable::getName(field);
    fields.push_back({name.str(), part, false});
  }

  return Type::object(std::move(fields), typeName);
}

const Type *LibraryPathRegistry::formatInputType() const {
  auto typeName = PathShapeNameTable::getName(PathShapeName::FORMAT_INPUT_PATH_OBJECT);

  return this->pathObjectType(Type::optional(Type::string()), typeName);
}

bool LibraryPathRegistry::isWin32Style(NodeModule module) const {
  return module == NodeModule::PathWin32 ||
         (module == NodeModule::Path && NodeCheckHelpers::isTargetWindows());
}

bool LibraryPathRegistry::isPathString(std::optional<PathValue> value) const {
  return value == PathValue::SEPARATOR || value == PathValue::DELIMITER;
}

bool LibraryPathRegistry::isPathModule(std::optional<PathValue> value) const {
  return value == PathValue::POSIX_MODULE || value == PathValue::WIN32_MODULE;
}

llvm::StringRef LibraryPathRegistry::pathStringText(PathValue value, NodeModule module) const {
  bool isWin32 = this->isWin32Style(module);
  switch (value) {
    case PathValue::SEPARATOR:
      return isWin32 ? "\\" : "/";
    case PathValue::DELIMITER:
      return isWin32 ? ";" : ":";
    case PathValue::POSIX_MODULE:
    case PathValue::WIN32_MODULE:
      return "";
  }

  return "";
}

LibraryPathRegistry::ResultPathProperty LibraryPathRegistry::checkPathProperty(llvm::StringRef name,
                                                               NodeModule module) const {
  auto value = PathValueTable::find(name);
  if (this->isPathModule(value)) {
    auto label = KnownModules::getPath(module);
    auto message = ErrorTextPath::throughFns(name, label);

    return err(LibraryPathRegistryPathValueError{
        .code = LibraryPathRegistryPathValueErrorCode::MODULE_AS_VALUE,
        .message = message,
    });
  }

  return ok(name);
}

LibraryPathRegistry::ResultPathFunction LibraryPathRegistry::checkPathFunction(llvm::StringRef functionName) const {
  auto value = PathValueTable::find(functionName);
  if (this->isPathString(value)) {
    auto message = ErrorTextPath::notCalled(functionName);

    return err(LibraryPathRegistryPathFunctionError{
        .code = LibraryPathRegistryPathFunctionErrorCode::STRING_CALLED,
        .message = message,
    });
  }
  if (this->isPathModule(value)) {
    auto message = ErrorTextPath::module(functionName);

    return err(LibraryPathRegistryPathFunctionError{
        .code = LibraryPathRegistryPathFunctionErrorCode::MODULE_CALLED,
        .message = message,
    });
  }

  return ok(functionName);
}

void LibraryPathRegistry::checkPathValue(const CheckPathValue &task) {
  Expr *expr = task.expr;
  auto &tasks = this->state.walk.tasks;
  auto value = PathValueTable::find(task.name);
  if (this->isPathString(value)) {
    auto text = this->pathStringText(value.value(), task.module);
    auto *literal = NodeCheckHelpers::stringLiteral(this->state.ast, expr->loc(), text);

    return tasks.pushChecked(literal);
  }
  auto property = this->checkPathProperty(task.name, task.module);
  if (property.isErr()) {
    this->state.diagnostics.error(expr->loc(), property.error().message);

    return tasks.pushChecked(expr);
  }

  return tasks.pushChecked(nullptr);
}

void LibraryPathRegistry::checkPathCall(const CheckPathCall &task) {
  CallExpr *call = task.call;
  NodeModule module = task.module;
  llvm::StringRef functionName = task.functionName;
  llvm::SMLoc loc = task.loc;
  auto &tasks = this->state.walk.tasks;
  call->type = Type::error();
  tasks.pushChecked(call);
  auto function = this->checkPathFunction(functionName);
  if (function.isErr()) {
    this->state.diagnostics.error(loc, function.error().message);

    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  bool isWin32 = this->isWin32Style(module);
  auto *style = this->state.ast.make<IntLitExpr>(loc, isWin32 ? 1 : 0);
  style->type = Type::intTy();
  const auto *path = PathFunctionTable::find(functionName);
  if (path != nullptr && path->form == PathFunctionForm::JOINED) {
    return this->checkJoinedPath(call, style, path->builtin, loc);
  }
  if (this->state.library.common.hasSpread(call)) {
    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  if (path == nullptr) {
    auto label = KnownModules::getPath(module);
    auto message = ErrorTextPath::notYet(functionName, label);
    this->state.diagnostics.error(loc, message);

    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  auto arity = this->state.library.common.checkArity(call, path->min, path->max);
  if (arity.isErr()) {
    return this->state.diagnostics.error(loc, arity.error().message);
  }
  if (path->form == PathFunctionForm::FORMAT) {
    tasks.pushTask(AfterFormatPathArg{.call = call, .style = style});

    return tasks.pushTask(CheckExprSlot{.slot = &call->args[0], .expected = this->formatInputType()});
  }
  tasks.pushTask(FinishTextPath{.call = call, .style = style, .builtin = path->builtin, .form = path->form});
  for (Expr *&arg : llvm::reverse(call->args)) {
    tasks.pushTask(CheckArgSlot{.slot = &arg, .to = Type::string()});
  }
}

void LibraryPathRegistry::checkJoinedPath(CallExpr *call, Expr *style, Builtin builtin, llvm::SMLoc loc) {
  llvm::SMLoc at = call->args.empty() ? loc : call->args[0]->loc();
  auto *paths = this->state.ast.make<ArrayLitExpr>(at);
  for (size_t index = 0; index < call->args.size(); ++index) {
    paths->elements.push_back({call->args[index], call->spreads(index)});
  }
  const auto *stringArray = Type::array(Type::string());
  call->args = {style, paths};
  call->spread.clear();
  call->target = BuiltinCall{.builtin = builtin};
  call->type = Type::string();

  return this->state.walk.tasks.pushTask(CheckArgSlot{.slot = &call->args[1], .to = stringArray});
}

void LibraryPathRegistry::afterFormatPathArg(const AfterFormatPathArg &task) {
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  const Type *type = call->args[0]->type;
  if (this->isFormatInput(type) || type->isError()) {
    return this->finishFormatPath(FinishFormatPath{.call = call, .style = task.style});
  }
  tasks.pushTask(FinishFormatPath{.call = call, .style = task.style});

  return tasks.pushTask(CoerceSlot{.slot = &call->args[0], .to = this->formatInputType(), .what = ""});
}

void LibraryPathRegistry::finishFormatPath(const FinishFormatPath &task) {
  CallExpr *call = task.call;
  call->args = {task.style, call->args[0]};
  call->target = BuiltinCall{.builtin = Builtin::PathFormat};
  call->type = Type::string();
}

bool LibraryPathRegistry::isFormatInput(const Type *type) const {
  if (!type->isObject() || type->isClass() || type->isTuple()) {
    return false;
  }
  const auto *input = this->formatInputType();
  const auto *stringType = Type::string();
  const auto *optionalString = Type::optional(stringType);
  for (const Field &field : type->fields()) {
    bool isKnown = input->field(field.name) != nullptr;
    if (!isKnown || (field.type != stringType && field.type != optionalString)) {
      return false;
    }
  }

  return true;
}

void LibraryPathRegistry::finishTextPath(const FinishTextPath &task) {
  CallExpr *call = task.call;
  call->args.insert(call->args.begin(), task.style);
  call->target = BuiltinCall{.builtin = task.builtin};
  call->type = this->pathResultType(task.form);
}

const Type *LibraryPathRegistry::pathResultType(PathFunctionForm form) const {
  if (form == PathFunctionForm::TEST) {
    return Type::boolTy();
  }
  if (form == PathFunctionForm::PARSED) {
    return this->parsedPathType();
  }

  return Type::string();
}

}
