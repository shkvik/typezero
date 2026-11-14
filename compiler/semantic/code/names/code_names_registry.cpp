#include "compiler/semantic/code/names/code_names_registry.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/builtin_names/keyword_table/keyword_table.h"
#include "compiler/program/module/load_chain/load_chain.h"
#include "compiler/semantic/code/calls/code_calls_registry_errors.h"
#include "compiler/semantic/declaration/modules/declaration_modules_registry_errors.h"
#include "compiler/semantic/declaration/names/declaration_names_registry_errors.h"
#include "compiler/semantic/library/decimal/decimal_checker/decimal_checker_errors.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

namespace tz {

CodeNamesRegistry::CodeNamesRegistry(SemanticState &owner)
    : state(owner) {}

void CodeNamesRegistry::checkName(const CheckName &task) {
  NameExpr *name = task.name;
  if (KeywordTable::find(name->name) == Keyword::THIS) {
    return this->state.walk.tasks.pushTask(CheckThis{.name = name});
  }
  if (name->cls() != nullptr && name->type != nullptr) {
    return this->state.walk.tasks.pushChecked(name);
  }
  if (this->state.findNodeName(name->name) != nullptr) {
    return this->state.walk.tasks.pushTask(CheckNodeName{.name = name});
  }
  auto load = this->state.findEarlyLoad(name->name);
  if (!load.has_value()) {
    auto lookup = LookupName{.name = name};
    return this->lookupName(lookup);
  }
  this->state.walk.tasks.pushTask(LookupName{.name = name});

  return this->state.walk.tasks.pushTask(*load);
}

void CodeNamesRegistry::lookupName(const LookupName &task) {
  NameExpr *name = task.name;
  auto entry = this->state.walk.scopes.findName(name->name);
  auto plain = this->checkTypeArgsAbsent(name, entry);
  if (plain.isErr()) {
    this->state.diagnostics.error(name->loc(), plain.error().message);
  }
  if (entry.var() != nullptr) {
    return this->startVariableName(name, entry);
  }
  if (entry.func() != nullptr) {
    return this->startFunctionName(name, entry.func());
  }
  auto *classDecl = this->builtinClass(name, entry);
  if (classDecl != nullptr) {
    return this->state.walk.tasks.pushTask(CheckClassValue{.name = name, .classDecl = classDecl});
  }

  return this->reportUnknownName(name, entry);
}

void CodeNamesRegistry::startVariableName(NameExpr *name, const ScopeEntry &entry) {
  VarSymbol *variable = entry.var();
  auto visibleHere = this->checkNotCaptured(name, entry);
  if (visibleHere.isErr()) {
    this->state.diagnostics.error(name->loc(), visibleHere.error().message);
  }
  name->binding = NamedVar{.var = variable};
  variable->read = true;
  this->state.walk.tasks.pushTask(FinishVariableName{.name = name, .variable = variable});
  FunctionDecl *functionDecl =
      variable->function() != nullptr ? variable->function() : variable->lambda();
  bool isKnown =
      variable->type != nullptr || functionDecl == nullptr || functionDecl->sym == nullptr;
  if (isKnown) {
    name->type = variable->type != nullptr ? variable->type : Type::error();
  } else {
    this->state.walk.tasks.pushTask(VariableTypeKnown{.name = name, .variable = variable});
    this->state.walk.tasks.pushTask(FunctionTypeOf{.function = functionDecl->sym});
  }

  return this->state.walk.tasks.pushTask(NoteUse{.variable = variable, .loc = name->loc()});
}

void CodeNamesRegistry::variableTypeKnown(const VariableTypeKnown &task) {
  VarSymbol *variable = task.variable;
  const Type *type = this->state.walk.tasks.takeAnswer();
  bool isNamedFunction =
      variable->function() != nullptr && variable->function()->sym->hasReturnType();
  if (isNamedFunction) {
    variable->type = type;
  }
  task.name->type = type;
}

void CodeNamesRegistry::finishVariableName(const FinishVariableName &task) {
  NameExpr *name = task.name;
  VarSymbol *variable = task.variable;
  auto visible = this->checkVisible(name, variable);
  if (visible.isErr()) {
    this->state.diagnostics.error(name->loc(), visible.error().message);
  }
  this->state.walk.tasks.pushChecked(name);
  const auto *shown = this->shownType(name, variable);
  if (shown != nullptr) {
    this->state.walk.tasks.pushTask(Convert{.to = shown});
  }
}

const Type *CodeNamesRegistry::shownType(const NameExpr *name, const VarSymbol *variable) const {
  const auto *view = this->streamView(variable, name->loc());
  if (view != nullptr) {
    return view;
  }
  const auto *narrowed = this->state.walk.flow.findNarrowed(variable);
  if (narrowed != nullptr) {
    return this->definedType(narrowed, variable);
  }
  bool isDefined = name->type->isOptional() && this->state.walk.flow.isDefined(variable);
  const Type *type = isDefined ? name->type->inner() : name->type;
  const auto *known = this->knownResultType(type, variable);
  if (known != nullptr) {
    return known;
  }

  return isDefined ? type : nullptr;
}

CodeNamesRegistry::ResultVisibleVariable
CodeNamesRegistry::checkVisible(const NameExpr *name, const VarSymbol *variable) const {
  if (this->state.walk.block.isOtherCaseVar(variable)) {
    auto message = ErrorTextCheckVisible::otherCase(name->name);

    return err(DeclarationNamesRegistryCheckVisibleError{
        .code = DeclarationNamesRegistryCheckVisibleErrorCode::DECLARED_IN_OTHER_CASE,
        .message = message,
    });
  }

  return ok(variable);
}

void CodeNamesRegistry::startFunctionName(NameExpr *name, FuncSymbol *function) {
  name->binding = NamedFunction{.func = function};
  this->state.walk.tasks.pushTask(FinishFunctionName{.name = name, .function = function});

  return this->state.walk.tasks.pushTask(FunctionTypeOf{.function = function});
}

void CodeNamesRegistry::finishFunctionName(const FinishFunctionName &task) {
  NameExpr *name = task.name;
  name->type = this->state.walk.tasks.takeAnswer();
  this->state.walk.tasks.pushChecked(name);

  return this->state.walk.tasks.pushTask(
      NoteHttpsValue{.functionDecl = task.function->decl, .loc = name->loc()});
}

void CodeNamesRegistry::reportUnknownName(NameExpr *name, const ScopeEntry &entry) {
  auto kind = this->unknownNameKind(name, entry);
  if (kind == UnknownNameKind::CLASS_VALUE) {
    return this->state.walk.tasks.pushTask(CheckClassValue{.name = name, .classDecl = entry.cls()});
  }
  if (kind == UnknownNameKind::OWN_CLASS_EXPRESSION) {
    auto *current = this->state.walk.self.getCurrentClass();
    return this->state.walk.tasks.pushTask(CheckClassValue{.name = name, .classDecl = current});
  }
  auto resolved = this->checkUnknownName(name, entry);
  bool isReported = resolved.isErr() && resolved.error().code !=
                                            CodeNamesRegistryUnknownNameErrorCode::ALREADY_REPORTED;
  if (isReported) {
    this->state.diagnostics.error(name->loc(), resolved.error().message);
  }
  name->type = Type::error();

  return this->state.walk.tasks.pushChecked(name);
}

CodeNamesRegistry::ResultResolvedNameUse CodeNamesRegistry::checkNotLoaded(const NameExpr *name) {
  auto loaded = this->checkLoaded(name);
  bool isReported =
      loaded.error().code == DeclarationModulesRegistryCheckLoadedErrorCode::ALREADY_REPORTED;
  CodeNamesRegistryUnknownNameErrorCode code =
      isReported ? CodeNamesRegistryUnknownNameErrorCode::ALREADY_REPORTED
                 : CodeNamesRegistryUnknownNameErrorCode::NOT_LOADED;

  return this->unknownError(code, loaded.error().message);
}

CodeNamesRegistry::ResultSourceIsLoaded CodeNamesRegistry::checkLoaded(const NameExpr *name) const {
  const auto &modules = this->state.declaration.modules;
  if (modules.getModuleState(modules.getCurrentModule()).badImports.contains(name->name)) {
    return err(DeclarationModulesRegistryCheckLoadedError{
        .code = DeclarationModulesRegistryCheckLoadedErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  llvm::StringRef declared;
  auto *source = this->state.findLaterSource(name->name, declared);
  Module *reader = modules.getLoadingModule() != nullptr ? modules.getLoadingModule()
                                                         : modules.getCurrentModule();
  std::string what = source != nullptr ? this->loadLabel(name->name, declared, source) : "";
  bool isTopLevel = !this->state.walk.functions.isInsideFunction() &&
                    modules.getCurrentModule() == modules.getLoadingModule();
  if (source != nullptr && isTopLevel) {
    auto chain = LoadChain::describe(reader, source);
    auto message =
        ErrorTextNotLoaded::whileLoads(reader->name, what, source->name, chain, declared);

    return err(DeclarationModulesRegistryCheckLoadedError{
        .code = DeclarationModulesRegistryCheckLoadedErrorCode::READ_WHILE_LOADING,
        .message = message,
    });
  }
  const auto *early = this->state.walk.functions.getEarlyFunction();
  if (source != nullptr && early != nullptr) {
    auto message = ErrorTextNotLoaded::beforeCall(what, reader->name, early->name);

    return err(DeclarationModulesRegistryCheckLoadedError{
        .code = DeclarationModulesRegistryCheckLoadedErrorCode::NO_TYPE_BEFORE_CALL,
        .message = message,
    });
  }
  if (source != nullptr) {
    auto message = ErrorTextNotLoaded::inFunction(what, reader->name);

    return err(DeclarationModulesRegistryCheckLoadedError{
        .code = DeclarationModulesRegistryCheckLoadedErrorCode::NO_TYPE_IN_FUNCTION,
        .message = message,
    });
  }

  return ok(name);
}

std::string CodeNamesRegistry::loadLabel(llvm::StringRef name, llvm::StringRef declared,
                                         const Module *source) const {
  if (declared != name) {
    return ErrorTextNotLoaded::label(name, declared, source->name);
  }
  if (source != this->state.declaration.modules.getLoadingModule()) {
    return ErrorTextNotLoaded::labelOf(name, source->name);
  }

  return ErrorTextNotLoaded::quoted(name);
}

CodeNamesRegistry::ResultNoTypeArguments
CodeNamesRegistry::checkTypeArgsAbsent(const NameExpr *name, const ScopeEntry &entry) const {
  if (!name->typeArgs.empty() && (entry.var() != nullptr || entry.func() != nullptr)) {
    auto message = ErrorTextCallTypeArgs::notGeneric(name->name);

    return err(CodeNamesRegistryTypeArgsAbsentError{
        .code = CodeNamesRegistryTypeArgsAbsentErrorCode::NOT_GENERIC,
        .message = message,
    });
  }

  return ok(name);
}

CodeNamesRegistry::ResultVariableVisible
CodeNamesRegistry::checkNotCaptured(const NameExpr *name, const ScopeEntry &entry) const {
  const auto *current = this->state.walk.self.getCurrentClass();
  bool isCaptured = this->state.walk.self.isInsideStaticInit() && current != nullptr &&
                    current->expression() && !entry.var()->isGlobal() &&
                    llvm::is_contained(current->locals, name->name);
  if (isCaptured) {
    auto message = ErrorTextNameUse::staticUse(name->name);

    return err(CodeNamesRegistryNotCapturedError{
        .code = CodeNamesRegistryNotCapturedErrorCode::CAPTURED_LOCAL,
        .message = message,
    });
  }

  return ok(name);
}

CodeNamesRegistry::ResultResolvedNameUse
CodeNamesRegistry::checkUnknownName(NameExpr *name, const ScopeEntry &entry) {
  auto kind = this->unknownNameKind(name, entry);
  auto global = GlobalNameTable::find(name->name);
  llvm::StringRef text = name->name;
  switch (kind) {
    case UnknownNameKind::OK_AS_VALUE:
      return this->unknownError(CodeNamesRegistryUnknownNameErrorCode::OK_AS_VALUE,
                                ErrorTextNameUse::okValue(text));
    case UnknownNameKind::RESULT_VALUE:
      return this->unknownError(CodeNamesRegistryUnknownNameErrorCode::RESULT_VALUE,
                                ErrorTextNameUse::resultUse(text));
    case UnknownNameKind::TYPE_ONLY:
      return this->unknownError(CodeNamesRegistryUnknownNameErrorCode::TYPE_ONLY,
                                this->typeOnlyText(name));
    case UnknownNameKind::THIS_OUTSIDE:
      return this->unknownError(CodeNamesRegistryUnknownNameErrorCode::THIS_OUTSIDE,
                                ErrorTextNameUse::noThis());
    case UnknownNameKind::BUILTIN_VALUE:
      return this->unknownError(CodeNamesRegistryUnknownNameErrorCode::BUILTIN_VALUE,
                                this->builtinValueText(text, global));
    case UnknownNameKind::SUPER_VALUE:
      return this->unknownError(CodeNamesRegistryUnknownNameErrorCode::SUPER_VALUE,
                                ErrorTextNameUse::superUse());
    case UnknownNameKind::ARGUMENTS:
      return this->unknownError(CodeNamesRegistryUnknownNameErrorCode::ARGUMENTS,
                                ErrorTextNameUse::arguments());
    case UnknownNameKind::BIG_INT_VALUE:
      return this->reportBigIntName(name);
    case UnknownNameKind::DECIMAL_VALUE:
      return this->unknownError(CodeNamesRegistryUnknownNameErrorCode::DECIMAL_VALUE,
                                ErrorTextNameUse::decimal());
    case UnknownNameKind::NULL_VALUE:
      return this->unknownError(CodeNamesRegistryUnknownNameErrorCode::NULL_VALUE,
                                ErrorTextNameUse::nullUse());
    case UnknownNameKind::UNSUPPORTED:
      return this->unknownError(
          CodeNamesRegistryUnknownNameErrorCode::UNSUPPORTED,
          ErrorTextCallName::noSupport(text, ErrorTextUnsupported::why(text)));
    case UnknownNameKind::ENUM_VALUE:
      return this->unknownError(CodeNamesRegistryUnknownNameErrorCode::ENUM_VALUE,
                                ErrorTextNameUse::enumUse(text));
    case UnknownNameKind::CLASS_VALUE:
      return ok(name);
    case UnknownNameKind::OWN_CLASS_EXPRESSION:
      return ok(name);
    case UnknownNameKind::CAPTURED_LOCAL:
      return this->unknownError(CodeNamesRegistryUnknownNameErrorCode::CAPTURED_LOCAL,
                                ErrorTextNameUse::laterUse(text));
    case UnknownNameKind::NOT_LOADED:
      return this->checkNotLoaded(name);
    case UnknownNameKind::DECLARED_AFTER_CALL:
      return this->unknownError(CodeNamesRegistryUnknownNameErrorCode::DECLARED_AFTER_CALL,
                                this->afterCallText(name));
    case UnknownNameKind::DECLARED_LATER_IN_BLOCK:
      return this->unknownError(CodeNamesRegistryUnknownNameErrorCode::DECLARED_LATER_IN_BLOCK,
                                ErrorTextNameUse::laterBlock(text));
    case UnknownNameKind::USED_BEFORE_DECLARED:
      return this->unknownError(CodeNamesRegistryUnknownNameErrorCode::USED_BEFORE_DECLARED,
                                ErrorTextNameUse::beforeDecl(text));
    case UnknownNameKind::OWN_INITIALIZER:
      return this->unknownError(CodeNamesRegistryUnknownNameErrorCode::OWN_INITIALIZER,
                                ErrorTextNameUse::ownInit(text));
    case UnknownNameKind::OBJECT_VALUE:
      return this->unknownError(
          CodeNamesRegistryUnknownNameErrorCode::OBJECT_VALUE,
          ErrorTextNameUse::objectUse(text, *ErrorTextNameUse::objectExample(global)));
    case UnknownNameKind::UNKNOWN_NAME:
      return this->unknownError(CodeNamesRegistryUnknownNameErrorCode::UNKNOWN_NAME,
                                ErrorTextNameUse::notFound(text));
  }

  return this->unknownError(CodeNamesRegistryUnknownNameErrorCode::UNKNOWN_NAME,
                            ErrorTextNameUse::notFound(text));
}

CodeNamesRegistry::UnknownNameKind
CodeNamesRegistry::unknownNameKind(const NameExpr *name, const ScopeEntry &entry) const {
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

std::optional<GlobalName> CodeNamesRegistry::resultNameOf(llvm::StringRef text,
                                                          const ScopeEntry &entry) const {
  if (entry.enumDecl() != nullptr) {
    return std::nullopt;
  }

  return this->state.library.result.resultName(text);
}

CodeNamesRegistry::UnknownNameKind
CodeNamesRegistry::laterNameKind(const NameExpr *name, std::optional<GlobalName> global) const {
  llvm::StringRef text = name->name;
  const auto *current = this->state.walk.self.getCurrentClass();
  bool isExpression = current != nullptr && current->expression();
  if (isExpression && !current->name.empty() && current->name == text) {
    return UnknownNameKind::OWN_CLASS_EXPRESSION;
  }
  if (isExpression && llvm::is_contained(current->locals, text)) {
    return UnknownNameKind::CAPTURED_LOCAL;
  }
  if (this->checkLoaded(name).isErr()) {
    return UnknownNameKind::NOT_LOADED;
  }
  if (this->isDeclaredAfterCall(text)) {
    return UnknownNameKind::DECLARED_AFTER_CALL;
  }
  const auto *later = this->state.walk.scopes.findDeclaredLater(text);
  if (later != nullptr && this->state.walk.functions.getFunctionDepth() > later->functions) {
    return UnknownNameKind::DECLARED_LATER_IN_BLOCK;
  }
  if (later != nullptr) {
    return UnknownNameKind::USED_BEFORE_DECLARED;
  }
  if (this->state.code.variables.isInitializing(text)) {
    return UnknownNameKind::OWN_INITIALIZER;
  }
  if (ErrorTextNameUse::objectExample(global).has_value()) {
    return UnknownNameKind::OBJECT_VALUE;
  }

  return UnknownNameKind::UNKNOWN_NAME;
}

bool CodeNamesRegistry::isDeclaredAfterCall(llvm::StringRef text) const {
  if (this->state.walk.functions.getEarlyFunction() == nullptr) {
    return false;
  }
  auto &modules = this->state.declaration.modules;
  bool isTopLevel = modules.getModuleState(modules.getCurrentModule()).topLevelNames.contains(text);

  return isTopLevel || this->state.walk.scopes.findDeclaredLater(text) != nullptr;
}

bool CodeNamesRegistry::isConversionName(std::optional<GlobalName> global) const {
  return global.has_value() && llvm::is_contained(kConversionNames, *global);
}

bool CodeNamesRegistry::isUriName(std::optional<GlobalName> global) const {
  return global.has_value() && llvm::is_contained(kUriNames, *global);
}

CodeNamesRegistry::ResultResolvedNameUse
CodeNamesRegistry::unknownError(CodeNamesRegistryUnknownNameErrorCode code,
                                const std::string &message) const {
  return err(CodeNamesRegistryUnknownNameError{
      .code = code,
      .message = message,
  });
}

std::string CodeNamesRegistry::typeOnlyText(const NameExpr *name) const {
  auto result = this->state.library.result.resultName(name->name);
  bool isOkType = result == GlobalName::OK_TYPE;
  llvm::StringRef make = isOkType ? "ok(value)" : "err(error)";
  llvm::StringRef test = isOkType ? "isOk()" : "isErr()";

  return ErrorTextNameUse::typeOnly(name->name, make, test);
}

std::string CodeNamesRegistry::builtinValueText(llvm::StringRef text,
                                                std::optional<GlobalName> global) const {
  const Type *returned = this->isUriName(global) ? Type::string() : Type::doubleTy();
  llvm::StringRef returns = returned->name();

  return ErrorTextNameUse::builtin(text, returns);
}

CodeNamesRegistry::ResultResolvedNameUse CodeNamesRegistry::reportBigIntName(const NameExpr *name) {
  auto quoted = ErrorTextBigInt::quoted(name->name);
  auto bigint = ErrorTextBigInt::none(quoted);
  this->state.diagnostics.error(name->loc(), bigint);

  return this->unknownError(CodeNamesRegistryUnknownNameErrorCode::ALREADY_REPORTED, "");
}

std::string CodeNamesRegistry::afterCallText(const NameExpr *name) const {
  const auto *early = this->state.walk.functions.getEarlyFunction();

  return ErrorTextNameUse::afterCall(name->name, early->name);
}

ClassDecl *CodeNamesRegistry::builtinClass(const NameExpr *name, const ScopeEntry &entry) const {
  if (entry.isFound()) {
    return nullptr;
  }

  return this->state.library.reflection.builtinConstructorNamed(name->name);
}

const Type *CodeNamesRegistry::streamView(VarSymbol *variable, llvm::SMLoc loc) const {
  const auto &streams = this->state.library.streams;
  if (!streams.hasStreamViews()) {
    return nullptr;
  }
  auto path = FieldPath{.var = variable};

  return streams.findStreamView(path, loc);
}

const Type *CodeNamesRegistry::definedType(const Type *narrowed, const VarSymbol *variable) const {
  bool isDefined = narrowed->isOptional() && this->state.walk.flow.isDefined(variable);

  return isDefined ? narrowed->inner() : narrowed;
}

const Type *CodeNamesRegistry::knownResultType(const Type *type, const VarSymbol *variable) const {
  if (!type->isResult()) {
    return nullptr;
  }
  auto &flow = this->state.walk.flow;
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

}
