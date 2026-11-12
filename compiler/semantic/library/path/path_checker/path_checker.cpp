#include "compiler/semantic/library/path/path_checker/path_checker.h"
#include "compiler/program/library/path/path_shape_field_table/path_shape_field_table.h"
#include "compiler/program/library/path/path_shape_name_table/path_shape_name_table.h"
#include "compiler/program/library/path/path_value_table/path_value_table.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/library/builtin_modules/node_check_helpers/node_check_helpers.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

PathChecker::PathChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
  this->typeRelations = &all.typeRelations;
  this->argumentChecker = &all.argumentChecker;
}

const Type *PathChecker::parsedPathType() const {
  auto typeName = PathShapeNameTable::getName(PathShapeName::PARSED_PATH);

  return this->pathObjectType(Type::string(), typeName);
}

const Type *PathChecker::pathObjectType(const Type *part, llvm::StringRef typeName) const {
  auto fields = std::vector<Field>();
  for (PathShapeField field :
       {PathShapeField::ROOT, PathShapeField::DIRECTORY, PathShapeField::BASE,
        PathShapeField::EXTENSION, PathShapeField::NAME}) {
    auto name = PathShapeFieldTable::getName(field);
    fields.push_back({name.str(), part, false});
  }

  return Type::object(std::move(fields), typeName);
}

const Type *PathChecker::formatInputType() const {
  auto typeName = PathShapeNameTable::getName(PathShapeName::FORMAT_INPUT_PATH_OBJECT);

  return this->pathObjectType(Type::optional(Type::string()), typeName);
}

bool PathChecker::isWin32Style(NodeModule module) const {
  return module == NodeModule::PathWin32 ||
         (module == NodeModule::Path && NodeCheckHelpers::isTargetWindows());
}

bool PathChecker::isPathString(std::optional<PathValue> value) const {
  return value == PathValue::SEPARATOR || value == PathValue::DELIMITER;
}

bool PathChecker::isPathModule(std::optional<PathValue> value) const {
  return value == PathValue::POSIX_MODULE || value == PathValue::WIN32_MODULE;
}

llvm::StringRef PathChecker::pathStringText(PathValue value, NodeModule module) const {
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

PathChecker::ResultPathProperty PathChecker::checkPathProperty(llvm::StringRef name,
                                                               NodeModule module) const {
  auto value = PathValueTable::find(name);
  if (this->isPathModule(value)) {
    auto label = KnownModules::getPath(module);
    auto message = ErrorTextPath::throughFns(name, label);

    return err(PathCheckerPathValueError{
        .code = PathCheckerPathValueErrorCode::MODULE_AS_VALUE,
        .message = message,
    });
  }

  return ok(name);
}

PathChecker::ResultPathFunction PathChecker::checkPathFunction(llvm::StringRef functionName) const {
  auto value = PathValueTable::find(functionName);
  if (this->isPathString(value)) {
    auto message = ErrorTextPath::notCalled(functionName);

    return err(PathCheckerPathFunctionError{
        .code = PathCheckerPathFunctionErrorCode::STRING_CALLED,
        .message = message,
    });
  }
  if (this->isPathModule(value)) {
    auto message = ErrorTextPath::module(functionName);

    return err(PathCheckerPathFunctionError{
        .code = PathCheckerPathFunctionErrorCode::MODULE_CALLED,
        .message = message,
    });
  }

  return ok(functionName);
}

Expr *PathChecker::checkPathValue(NodeModule module, llvm::StringRef name, Expr *expr) {
  auto value = PathValueTable::find(name);
  if (this->isPathString(value)) {
    auto text = this->pathStringText(value.value(), module);
    return NodeCheckHelpers::stringLiteral(this->state.getAst(), expr->loc(), text);
  }
  auto property = this->checkPathProperty(name, module);
  if (property.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), property.error().message);
    return expr;
  }

  return nullptr;
}

Expr *PathChecker::checkPathCall(CallExpr *call, NodeModule module, llvm::StringRef functionName,
                                 llvm::SMLoc loc) {
  call->type = Type::error();
  auto function = this->checkPathFunction(functionName);
  if (function.isErr()) {
    this->state.getDiagnostics().error(loc, function.error().message);
    this->argumentChecker->walkArgsLoosely(call);
    return call;
  }
  bool isWin32 = this->isWin32Style(module);
  auto *style = this->state.getAst().make<IntLitExpr>(loc, isWin32 ? 1 : 0);
  style->type = Type::intTy();
  const auto *path = PathFunctionTable::find(functionName);
  if (path != nullptr && path->form == PathFunctionForm::JOINED) {
    return this->checkJoinedPath(call, style, path->builtin, loc);
  }
  if (this->callChecker->rejectSpread(call)) {
    return call;
  }
  if (path == nullptr) {
    auto label = KnownModules::getPath(module);
    auto message = ErrorTextPath::notYet(functionName, label);
    this->state.getDiagnostics().error(loc, message);
    this->argumentChecker->walkArgsLoosely(call);
    return call;
  }
  auto arity = this->argumentChecker->checkArity(call, path->min, path->max);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  if (path->form == PathFunctionForm::FORMAT) {
    return this->checkFormatPath(call, style);
  }

  return this->checkTextPath(call, style, *path);
}

Expr *PathChecker::checkJoinedPath(CallExpr *call, Expr *style, Builtin builtin, llvm::SMLoc loc) {
  llvm::SMLoc at = call->args.empty() ? loc : call->args[0]->loc();
  auto *paths = this->state.getAst().make<ArrayLitExpr>(at);
  for (size_t index = 0; index < call->args.size(); ++index) {
    paths->elements.push_back({call->args[index], call->spreads(index)});
  }
  const auto *stringArray = Type::array(Type::string());
  auto *array = this->argumentChecker->checkArg(paths, stringArray);
  call->args = {style, array};
  call->spread.clear();
  call->target = BuiltinCall{.builtin = builtin};
  call->type = Type::string();

  return call;
}

Expr *PathChecker::checkFormatPath(CallExpr *call, Expr *style) {
  const auto *input = this->formatInputType();
  auto *arg = this->exprChecker->checkExpr(call->args[0], input);
  const Type *type = arg->type;
  if (!this->isFormatInput(type) && !type->isError()) {
    arg = this->typeRelations->coerce(arg, input);
  }
  call->args = {style, arg};
  call->target = BuiltinCall{.builtin = Builtin::PathFormat};
  call->type = Type::string();

  return call;
}

bool PathChecker::isFormatInput(const Type *type) const {
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

Expr *PathChecker::checkTextPath(CallExpr *call, Expr *style, const PathFunctionInfo &path) {
  for (Expr *&arg : call->args) {
    arg = this->argumentChecker->checkArg(arg, Type::string());
  }
  call->args.insert(call->args.begin(), style);
  call->target = BuiltinCall{.builtin = path.builtin};
  call->type = this->pathResultType(path.form);

  return call;
}

const Type *PathChecker::pathResultType(PathFunctionForm form) const {
  if (form == PathFunctionForm::TEST) {
    return Type::boolTy();
  }
  if (form == PathFunctionForm::PARSED) {
    return this->parsedPathType();
  }

  return Type::string();
}

}
