#include "compiler/semantic/bodies/expr/name_checker/name_checker.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/builtin_names/keyword_table/keyword_table.h"
#include "compiler/semantic/bodies/classes/class_checker/class_checker.h"
#include "compiler/semantic/bodies/classes/class_value_checker/class_value_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker_errors.h"
#include "compiler/semantic/bodies/expr/new_checker/new_checker_errors.h"
#include "compiler/semantic/bodies/functions/nested_functions/nested_functions.h"
#include "compiler/semantic/bodies/top_level_checker/top_level_checker.h"
#include "compiler/semantic/generics/generic_checker/generic_checker.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/semantic/library/builtin_modules/builtin_module_checker/builtin_module_checker.h"
#include "compiler/semantic/library/decimal/decimal_checker/decimal_checker.h"
#include "compiler/semantic/library/decimal/decimal_checker/decimal_checker_errors.h"
#include "compiler/semantic/library/reflect/metadata_checker/metadata_checker.h"
#include "compiler/semantic/library/result/result_types/result_types.h"
#include "compiler/semantic/library/stream/set_encoding_checker/set_encoding_checker.h"
#include "compiler/semantic/library/tz_modules/tz_modules_checker/tz_modules_checker.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

namespace tz {

namespace {

const GlobalName kConversionNames[] = {
    GlobalName::STRING,      GlobalName::NUMBER,  GlobalName::PARSE_INT,
    GlobalName::PARSE_FLOAT, GlobalName::BOOLEAN,
};

const GlobalName kUriNames[] = {
    GlobalName::ENCODE_URI_COMPONENT,
    GlobalName::ENCODE_URI,
    GlobalName::DECODE_URI_COMPONENT,
    GlobalName::DECODE_URI,
};

}

NameChecker::NameChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.scopes;
  this->resultTypes = &all.resultTypes;
  this->classChecker = &all.classChecker;
  this->typeRelations = &all.typeRelations;
  this->genericChecker = &all.genericChecker;
  this->returnInferrer = &all.returnInferrer;
  this->metadataChecker = &all.metadataChecker;
  this->nestedFunctions = &all.nestedFunctions;
  this->topLevelChecker = &all.topLevelChecker;
  this->tzModulesChecker = &all.tzModulesChecker;
  this->classValueChecker = &all.classValueChecker;
  this->builtinModuleChecker = &all.builtinModuleChecker;
}

NameChecker::ResultNoTypeArguments NameChecker::checkTypeArgsAbsent(const NameExpr *name,
                                                                    const ScopeEntry &entry) const {
  if (!name->typeArgs.empty() && (entry.var() != nullptr || entry.func() != nullptr)) {
    auto message = ErrorTextCallTypeArgs::notGeneric(name->name);

    return err(NameCheckerTypeArgsAbsentError{
        .code = NameCheckerTypeArgsAbsentErrorCode::NOT_GENERIC,
        .message = message,
    });
  }

  return ok(name);
}

NameChecker::ResultVariableVisible NameChecker::checkNotCaptured(const NameExpr *name,
                                                                 const ScopeEntry &entry) const {
  const auto *current = this->state.getClassContext().getCurrentClass();
  bool isCaptured = this->state.getClassContext().isInsideStaticInit() && current != nullptr &&
                    current->expression() && !entry.var()->isGlobal() &&
                    llvm::is_contained(current->locals, name->name);
  if (isCaptured) {
    auto message = ErrorTextNameUse::staticUse(name->name);

    return err(NameCheckerNotCapturedError{
        .code = NameCheckerNotCapturedErrorCode::CAPTURED_LOCAL,
        .message = message,
    });
  }

  return ok(name);
}

NameChecker::ResultResolvedNameUse NameChecker::checkUnknownName(NameExpr *name,
                                                                 const ScopeEntry &entry) {
  auto kind = this->unknownNameKind(name, entry);
  auto global = GlobalNameTable::find(name->name);
  llvm::StringRef text = name->name;
  switch (kind) {
    case UnknownNameKind::OK_AS_VALUE:
      return this->unknownError(NameCheckerUnknownNameErrorCode::OK_AS_VALUE,
                                ErrorTextNameUse::okValue(text));
    case UnknownNameKind::RESULT_VALUE:
      return this->unknownError(NameCheckerUnknownNameErrorCode::RESULT_VALUE,
                                ErrorTextNameUse::resultUse(text));
    case UnknownNameKind::TYPE_ONLY:
      return this->unknownError(NameCheckerUnknownNameErrorCode::TYPE_ONLY,
                                this->typeOnlyText(name));
    case UnknownNameKind::THIS_OUTSIDE:
      return this->unknownError(NameCheckerUnknownNameErrorCode::THIS_OUTSIDE,
                                ErrorTextNameUse::noThis());
    case UnknownNameKind::BUILTIN_VALUE:
      return this->unknownError(NameCheckerUnknownNameErrorCode::BUILTIN_VALUE,
                                this->builtinValueText(text, global));
    case UnknownNameKind::SUPER_VALUE:
      return this->unknownError(NameCheckerUnknownNameErrorCode::SUPER_VALUE,
                                ErrorTextNameUse::superUse());
    case UnknownNameKind::ARGUMENTS:
      return this->unknownError(NameCheckerUnknownNameErrorCode::ARGUMENTS,
                                ErrorTextNameUse::arguments());
    case UnknownNameKind::BIG_INT_VALUE:
      return this->reportBigIntName(name);
    case UnknownNameKind::DECIMAL_VALUE:
      return this->unknownError(NameCheckerUnknownNameErrorCode::DECIMAL_VALUE,
                                ErrorTextNameUse::decimal());
    case UnknownNameKind::NULL_VALUE:
      return this->unknownError(NameCheckerUnknownNameErrorCode::NULL_VALUE,
                                ErrorTextNameUse::nullUse());
    case UnknownNameKind::UNSUPPORTED:
      return this->unknownError(
          NameCheckerUnknownNameErrorCode::UNSUPPORTED,
          ErrorTextCallName::noSupport(text, ErrorTextUnsupported::why(text)));
    case UnknownNameKind::ENUM_VALUE:
      return this->unknownError(NameCheckerUnknownNameErrorCode::ENUM_VALUE,
                                ErrorTextNameUse::enumUse(text));
    case UnknownNameKind::CLASS_VALUE:
      return ok(this->classValueChecker->checkClassValue(name, entry.cls()));
    case UnknownNameKind::OWN_CLASS_EXPRESSION:
      return ok(this->classValueChecker->checkClassValue(
          name, this->state.getClassContext().getCurrentClass()));
    case UnknownNameKind::CAPTURED_LOCAL:
      return this->unknownError(NameCheckerUnknownNameErrorCode::CAPTURED_LOCAL,
                                ErrorTextNameUse::laterUse(text));
    case UnknownNameKind::NOT_LOADED:
      return this->notLoadedError(name);
    case UnknownNameKind::DECLARED_AFTER_CALL:
      return this->unknownError(NameCheckerUnknownNameErrorCode::DECLARED_AFTER_CALL,
                                this->afterCallText(name));
    case UnknownNameKind::DECLARED_LATER_IN_BLOCK:
      return this->unknownError(NameCheckerUnknownNameErrorCode::DECLARED_LATER_IN_BLOCK,
                                ErrorTextNameUse::laterBlock(text));
    case UnknownNameKind::USED_BEFORE_DECLARED:
      return this->unknownError(NameCheckerUnknownNameErrorCode::USED_BEFORE_DECLARED,
                                ErrorTextNameUse::beforeDecl(text));
    case UnknownNameKind::OWN_INITIALIZER:
      return this->unknownError(NameCheckerUnknownNameErrorCode::OWN_INITIALIZER,
                                ErrorTextNameUse::ownInit(text));
    case UnknownNameKind::OBJECT_VALUE:
      return this->unknownError(
          NameCheckerUnknownNameErrorCode::OBJECT_VALUE,
          ErrorTextNameUse::objectUse(text, *ErrorTextNameUse::objectExample(global)));
    case UnknownNameKind::UNKNOWN_NAME:
      return this->unknownError(NameCheckerUnknownNameErrorCode::UNKNOWN_NAME,
                                ErrorTextNameUse::notFound(text));
  }

  return this->unknownError(NameCheckerUnknownNameErrorCode::UNKNOWN_NAME,
                            ErrorTextNameUse::notFound(text));
}

NameChecker::UnknownNameKind NameChecker::unknownNameKind(const NameExpr *name,
                                                          const ScopeEntry &entry) const {
  llvm::StringRef text = name->name;
  auto global = GlobalNameTable::find(text);
  auto result = this->resultNameOf(text, entry);
  if (result == GlobalName::OK || result == GlobalName::ERR) {
    return UnknownNameKind::OK_AS_VALUE;
  }
  if (result == GlobalName::RESULT) {
    return UnknownNameKind::RESULT_VALUE;
  }
  if (result == GlobalName::OK_TYPE || result == GlobalName::ERR_TYPE) {
    return UnknownNameKind::TYPE_ONLY;
  }
  auto keyword = KeywordTable::find(text);
  if (keyword == Keyword::THIS) {
    return UnknownNameKind::THIS_OUTSIDE;
  }
  if (this->isConversionName(global) || this->isUriName(global)) {
    return UnknownNameKind::BUILTIN_VALUE;
  }
  if (keyword == Keyword::SUPER) {
    return UnknownNameKind::SUPER_VALUE;
  }
  if (keyword == Keyword::ARGUMENTS) {
    return UnknownNameKind::ARGUMENTS;
  }
  if (global == GlobalName::BIG_INT) {
    return UnknownNameKind::BIG_INT_VALUE;
  }
  if (global == GlobalName::DECIMAL) {
    return UnknownNameKind::DECIMAL_VALUE;
  }
  if (keyword == Keyword::NULL_KEYWORD) {
    return UnknownNameKind::NULL_VALUE;
  }
  if (!ErrorTextUnsupported::why(text).empty()) {
    return UnknownNameKind::UNSUPPORTED;
  }
  if (entry.enumDecl() != nullptr) {
    return UnknownNameKind::ENUM_VALUE;
  }
  if (entry.cls() != nullptr) {
    return UnknownNameKind::CLASS_VALUE;
  }
  return this->laterNameKind(name, global);
}

std::optional<GlobalName> NameChecker::resultNameOf(llvm::StringRef text,
                                                    const ScopeEntry &entry) const {
  if (entry.enumDecl() != nullptr) {
    return std::nullopt;
  }

  return this->resultTypes->resultName(text);
}

NameChecker::UnknownNameKind NameChecker::laterNameKind(const NameExpr *name,
                                                        std::optional<GlobalName> global) const {
  llvm::StringRef text = name->name;
  const auto *current = this->state.getClassContext().getCurrentClass();
  bool isExpression = current != nullptr && current->expression();
  if (isExpression && !current->name.empty() && current->name == text) {
    return UnknownNameKind::OWN_CLASS_EXPRESSION;
  }
  if (isExpression && llvm::is_contained(current->locals, text)) {
    return UnknownNameKind::CAPTURED_LOCAL;
  }
  if (this->topLevelChecker->checkLoaded(name).isErr()) {
    return UnknownNameKind::NOT_LOADED;
  }
  if (this->isDeclaredAfterCall(text)) {
    return UnknownNameKind::DECLARED_AFTER_CALL;
  }
  const auto *later = this->state.getScopeStack().findDeclaredLater(text);
  if (later != nullptr && this->state.getFunctionStack().getFunctionDepth() > later->functions) {
    return UnknownNameKind::DECLARED_LATER_IN_BLOCK;
  }
  if (later != nullptr) {
    return UnknownNameKind::USED_BEFORE_DECLARED;
  }
  if (this->state.getExpressionMarks().isInitializing(text)) {
    return UnknownNameKind::OWN_INITIALIZER;
  }
  if (ErrorTextNameUse::objectExample(global).has_value()) {
    return UnknownNameKind::OBJECT_VALUE;
  }

  return UnknownNameKind::UNKNOWN_NAME;
}

bool NameChecker::isDeclaredAfterCall(llvm::StringRef text) const {
  if (this->state.getFunctionStack().getEarlyFunction() == nullptr) {
    return false;
  }
  auto &modules = this->state.getModuleStates();
  bool isTopLevel = modules.getModuleState(modules.getCurrentModule()).topLevelNames.contains(text);

  return isTopLevel || this->state.getScopeStack().findDeclaredLater(text) != nullptr;
}

bool NameChecker::isConversionName(std::optional<GlobalName> global) const {
  return global.has_value() && llvm::is_contained(kConversionNames, *global);
}

bool NameChecker::isUriName(std::optional<GlobalName> global) const {
  return global.has_value() && llvm::is_contained(kUriNames, *global);
}

NameChecker::ResultResolvedNameUse NameChecker::unknownError(NameCheckerUnknownNameErrorCode code,
                                                             const std::string &message) const {
  return err(NameCheckerUnknownNameError{
      .code = code,
      .message = message,
  });
}

std::string NameChecker::typeOnlyText(const NameExpr *name) const {
  auto result = this->resultTypes->resultName(name->name);
  bool isOkType = result == GlobalName::OK_TYPE;
  llvm::StringRef make = isOkType ? "ok(value)" : "err(error)";
  llvm::StringRef test = isOkType ? "isOk()" : "isErr()";

  return ErrorTextNameUse::typeOnly(name->name, make, test);
}

std::string NameChecker::builtinValueText(llvm::StringRef text,
                                          std::optional<GlobalName> global) const {
  const Type *returned = this->isUriName(global) ? Type::string() : Type::doubleTy();
  llvm::StringRef returns = returned->name();

  return ErrorTextNameUse::builtin(text, returns);
}

NameChecker::ResultResolvedNameUse NameChecker::reportBigIntName(const NameExpr *name) {
  auto quoted = ErrorTextBigInt::quoted(name->name);
  auto bigint = ErrorTextBigInt::none(quoted);
  this->state.getDiagnostics().error(name->loc(), bigint);

  return this->unknownError(NameCheckerUnknownNameErrorCode::ALREADY_REPORTED, "");
}

NameChecker::ResultResolvedNameUse NameChecker::notLoadedError(const NameExpr *name) {
  auto loaded = this->topLevelChecker->checkLoaded(name);
  bool isReported = loaded.error().code == TopLevelCheckerCheckLoadedErrorCode::ALREADY_REPORTED;
  NameCheckerUnknownNameErrorCode code = isReported
                                             ? NameCheckerUnknownNameErrorCode::ALREADY_REPORTED
                                             : NameCheckerUnknownNameErrorCode::NOT_LOADED;

  return this->unknownError(code, loaded.error().message);
}

std::string NameChecker::afterCallText(const NameExpr *name) const {
  const auto *early = this->state.getFunctionStack().getEarlyFunction();

  return ErrorTextNameUse::afterCall(name->name, early->name);
}

Expr *NameChecker::checkName(NameExpr *name) {
  if (KeywordTable::find(name->name) == Keyword::THIS) {
    return this->checkThisName(name);
  }
  if (name->cls() != nullptr && name->type != nullptr) {
    return name;
  }
  auto *node = this->builtinModuleChecker->checkNodeName(name);
  if (node != nullptr) {
    return node;
  }
  auto entry = this->topLevelChecker->lookupValue(name->name);
  auto plain = this->checkTypeArgsAbsent(name, entry);
  if (plain.isErr()) {
    this->state.getDiagnostics().error(name->loc(), plain.error().message);
  }
  if (entry.var() != nullptr) {
    return this->checkVariableName(name, entry);
  }
  if (entry.func() != nullptr) {
    return this->checkFunctionName(name, entry.func());
  }
  auto *classDecl = this->builtinClass(name, entry);
  if (classDecl != nullptr) {
    return this->classValueChecker->checkClassValue(name, classDecl);
  }

  return this->reportUnknownName(name, entry);
}

ClassDecl *NameChecker::builtinClass(const NameExpr *name, const ScopeEntry &entry) const {
  if (entry.isFound()) {
    return nullptr;
  }

  return this->metadataChecker->builtinConstructorNamed(name->name);
}

Expr *NameChecker::checkThisName(NameExpr *name) {
  auto reference = this->classChecker->checkThis(name);
  if (reference.isErr()) {
    this->state.getDiagnostics().error(name->loc(), reference.error().message);
    return name;
  }

  return reference.value();
}

Expr *NameChecker::checkVariableName(NameExpr *name, const ScopeEntry &entry) {
  VarSymbol *variable = entry.var();
  auto visibleHere = this->checkNotCaptured(name, entry);
  if (visibleHere.isErr()) {
    this->state.getDiagnostics().error(name->loc(), visibleHere.error().message);
  }
  name->binding = NamedVar{.var = variable};
  variable->read = true;
  this->nestedFunctions->noteUse(variable, name->loc());
  name->type = this->scopes->varType(variable);
  auto visible = this->scopes->checkVisible(name, variable);
  if (visible.isErr()) {
    this->state.getDiagnostics().error(name->loc(), visible.error().message);
  }

  return this->narrowedName(name, variable);
}

Expr *NameChecker::narrowedName(NameExpr *name, VarSymbol *variable) {
  const auto *view = this->streamView(variable, name->loc());
  if (view != nullptr) {
    return this->typeRelations->convert(name, view);
  }
  const auto *narrowed = this->genericChecker->narrowedType(variable);
  if (narrowed != nullptr) {
    const auto *shown = this->definedType(narrowed, variable);

    return this->typeRelations->convert(name, shown);
  }
  bool isDefined = name->type->isOptional() && this->state.getFlowState().isDefined(variable);
  const Type *type = isDefined ? name->type->inner() : name->type;
  const auto *known = this->knownResultType(type, variable);
  if (known != nullptr) {
    return this->typeRelations->convert(name, known);
  }
  if (isDefined) {
    return this->typeRelations->convert(name, type);
  }

  return name;
}

const Type *NameChecker::streamView(VarSymbol *variable, llvm::SMLoc loc) const {
  const auto &streams = this->state.getStreamStore();
  if (!streams.hasStreamViews()) {
    return nullptr;
  }
  auto path = FieldPath{.var = variable};

  return streams.findStreamView(path, loc);
}

const Type *NameChecker::definedType(const Type *narrowed, const VarSymbol *variable) const {
  bool isDefined = narrowed->isOptional() && this->state.getFlowState().isDefined(variable);

  return isDefined ? narrowed->inner() : narrowed;
}

const Type *NameChecker::knownResultType(const Type *type, const VarSymbol *variable) const {
  if (!type->isResult()) {
    return nullptr;
  }
  auto &flow = this->state.getFlowState();
  bool isOkKnown = flow.isOkResult(variable) && !type->errType()->isNever();
  if (isOkKnown) {
    return Type::result(type->okType(), Type::never());
  }
  bool isErrKnown = flow.isErrResult(variable) && !type->okType()->isNever();
  if (isErrKnown) {
    return Type::result(Type::never(), type->errType());
  }

  return nullptr;
}

Expr *NameChecker::checkFunctionName(NameExpr *name, FuncSymbol *function) {
  name->binding = NamedFunction{.func = function};
  name->type = this->returnInferrer->functionType(function);
  this->tzModulesChecker->noteHttpsValue(function->decl, name->loc());

  return name;
}

Expr *NameChecker::reportUnknownName(NameExpr *name, const ScopeEntry &entry) {
  auto resolved = this->checkUnknownName(name, entry);
  if (resolved.isOk()) {
    return resolved.value();
  }
  if (resolved.error().code != NameCheckerUnknownNameErrorCode::ALREADY_REPORTED) {
    this->state.getDiagnostics().error(name->loc(), resolved.error().message);
  }
  name->type = Type::error();

  return name;
}

}
