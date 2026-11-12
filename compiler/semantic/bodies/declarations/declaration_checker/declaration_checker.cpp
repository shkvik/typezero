#include "compiler/semantic/bodies/declarations/declaration_checker/declaration_checker.h"
#include "compiler/program/library/object/object_member_table/object_member_table.h"
#include "compiler/semantic/bodies/top_level_checker/top_level_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/declare/module_declarer/module_declarer.h"
#include "compiler/semantic/declare/type_declarer/type_declarer.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Path.h"

using llvm::dyn_cast;
using llvm::dyn_cast_or_null;

namespace tz {

namespace {

constexpr const char *kStaticPrefix = "static ";

}

DeclarationChecker::DeclarationChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->typeDeclarer = &all.typeDeclarer;
  this->typeResolver = &all.typeResolver;
  this->classDeclarer = &all.classDeclarer;
  this->moduleDeclarer = &all.moduleDeclarer;
  this->topLevelChecker = &all.topLevelChecker;
}

const char *DeclarationChecker::accessWord(Access access) const {
  switch (access) {
    case Access::Public:
      return "public";
    case Access::Protected:
      return "protected";
    case Access::Private:
      return "private";
  }

  return "private";
}

bool DeclarationChecker::typesAgree(const Type *here, const Type *there) const {
  return here == nullptr || there == nullptr || here == there || here->isError() ||
         there->isError();
}

bool DeclarationChecker::typesAgreeWidened(const Type *here, const Type *there) const {
  return this->typesAgree(here, there) || Type::optional(here) == there;
}

std::string DeclarationChecker::memberKey(llvm::StringRef name, bool isStatic) const {
  if (!isStatic) {
    return name.str();
  }

  return kStaticPrefix + name.str();
}

const ClassField *DeclarationChecker::classField(const ClassDecl *classDecl,
                                                 const DeclaredMember &member) const {
  for (const ClassField &candidate : llvm::reverse(classDecl->fields)) {
    bool isSameField = candidate.name == member.name && candidate.isStatic() == member.isStatic &&
                       candidate.method() == nullptr;
    if (isSameField) {
      return &candidate;
    }
  }

  return nullptr;
}

const FunctionDecl *DeclarationChecker::classMethod(const ClassDecl *classDecl,
                                                    const DeclaredMember &member) const {
  for (const FunctionDecl *candidate : llvm::reverse(classDecl->methods)) {
    if (candidate->name == member.name && candidate->isStatic() == member.isStatic) {
      return candidate;
    }
  }

  return nullptr;
}

std::string DeclarationChecker::fileName() const {
  return llvm::sys::path::filename(this->checkedModule->path).str();
}

const Type *DeclarationChecker::resolveWritten(const std::optional<TypeRef> &ref) {
  if (!ref.has_value()) {
    return nullptr;
  }

  return this->typeResolver->resolveType(*ref);
}

const Type *DeclarationChecker::resolveWrittenOrVoid(const std::optional<TypeRef> &ref) {
  if (!ref.has_value()) {
    return nullptr;
  }

  return this->typeResolver->resolveTypeOrVoid(*ref);
}

DeclarationChecker::ResultModuleExportsName
DeclarationChecker::checkExportFound(llvm::StringRef name) const {
  auto found = this->exportedNames.find(name);
  if (found == this->exportedNames.end()) {
    auto message = ErrorTextExports::unexported(name, this->fileName());

    return err(DeclarationCheckerExportFoundError{
        .code = DeclarationCheckerExportFoundErrorCode::NOT_EXPORTED,
        .message = message,
    });
  }

  return ok(&found->second);
}

DeclarationChecker::ResultExportWasDeclared
DeclarationChecker::checkExportDeclared(llvm::StringRef name, const llvm::StringSet<> &declared,
                                        llvm::StringRef declarations) const {
  if (!declared.contains(name)) {
    auto message = ErrorTextExports::undeclared(name, declarations);

    return err(DeclarationCheckerExportDeclaredError{
        .code = DeclarationCheckerExportDeclaredErrorCode::NOT_DECLARED,
        .message = message,
    });
  }

  return ok(name);
}

void DeclarationChecker::walkDeclarationFile(Module *module) {
  this->checkedModule = module;
  auto *outer = this->state.getModuleStates().getCurrentModule();
  this->moduleDeclarer->useModule(module);
  this->collectExportedNames(module);
  auto declared = llvm::StringSet<>();
  const DeclarationFile &file = *module->declaration;
  for (Stmt *item : file.items) {
    this->walkFileItem(item, declared);
  }
  for (const DeclaredClass &declaredClass : file.classes) {
    this->walkFileClass(declaredClass, declared);
  }
  this->walkExportsDeclared(file, declared);
  if (outer != nullptr) {
    this->moduleDeclarer->useModule(outer);
  }
}

void DeclarationChecker::collectExportedNames(const Module *module) {
  this->exportedNames.clear();
  for (const ExportDecl &exportDecl : module->file.exports) {
    this->addLocalExports(exportDecl);
  }
}

void DeclarationChecker::addLocalExports(const ExportDecl &exportDecl) {
  const LocalExport *local = exportDecl.form.as<LocalExport>();
  if (local == nullptr) {
    return;
  }
  for (const ModuleName &name : local->names) {
    llvm::StringRef shown = name.as.empty() ? name.name : name.as;
    this->exportedNames[shown] = Exported{.local = name.name, .loc = name.loc};
  }
}

void DeclarationChecker::walkFileItem(Stmt *item, llvm::StringSet<> &declared) {
  auto *alias = dyn_cast<TypeAliasDecl>(item);
  auto *functionDecl = dyn_cast<FunctionDecl>(item);
  if (alias != nullptr) {
    this->walkFileAlias(alias, declared);
    return;
  }
  if (functionDecl != nullptr) {
    this->walkFileFunction(functionDecl, declared);
  }
}

void DeclarationChecker::walkFileAlias(TypeAliasDecl *alias, llvm::StringSet<> &declared) {
  declared.insert(alias->name);
  auto exported = this->checkExportFound(alias->name);
  if (exported.isErr()) {
    this->state.getDiagnostics().error(alias->loc(), exported.error().message);
    return;
  }
  auto same = this->checkAlias(alias, *exported.value());
  if (same.isErr()) {
    this->state.getDiagnostics().error(alias->loc(), same.error().message);
  }
}

void DeclarationChecker::walkFileFunction(FunctionDecl *functionDecl, llvm::StringSet<> &declared) {
  declared.insert(functionDecl->name);
  auto exported = this->checkExportFound(functionDecl->name);
  if (exported.isErr()) {
    this->state.getDiagnostics().error(functionDecl->loc(), exported.error().message);
    return;
  }
  this->walkDeclaredFunction(functionDecl, *exported.value());
}

void DeclarationChecker::walkFileClass(const DeclaredClass &declaredClass,
                                       llvm::StringSet<> &declared) {
  declared.insert(declaredClass.name);
  auto exported = this->checkExportFound(declaredClass.name);
  if (exported.isErr()) {
    this->state.getDiagnostics().error(declaredClass.loc, exported.error().message);
    return;
  }
  this->walkDeclaredClass(declaredClass, *exported.value());
}

void DeclarationChecker::walkExportsDeclared(const DeclarationFile &file,
                                             const llvm::StringSet<> &declared) {
  auto declarations = llvm::sys::path::filename(file.path).str();
  for (const auto &entry : this->exportedNames) {
    auto listed = this->checkExportDeclared(entry.getKey(), declared, declarations);
    if (listed.isErr()) {
      this->state.getDiagnostics().error(entry.second.loc, listed.error().message);
    }
  }
}

DeclarationChecker::ResultAliasTypesAreSame
DeclarationChecker::checkAlias(TypeAliasDecl *declared, const Exported &exported) const {
  auto *found = this->typeDeclarer->lookupType(exported.local);
  auto *implemented = dyn_cast_or_null<TypeAliasDecl>(found);
  auto file = this->fileName();
  auto isType = this->checkAliasFound(declared, implemented, file);
  if (isType.isErr()) {
    return err(isType.error());
  }
  auto counted = this->checkAliasParams(declared, implemented, file);
  if (counted.isErr()) {
    return err(counted.error());
  }
  if (!implemented->typeParams.empty()) {
    return ok(implemented);
  }
  const auto *there = this->typeDeclarer->resolveAlias(implemented);
  const auto *here = this->typeDeclarer->resolveAlias(declared);
  if (this->typesAgree(here, there)) {
    return ok(implemented);
  }
  auto what = ErrorTextDeclaration::quoted(declared->name);
  bool isPlainObjects =
      here->isObject() && there->isObject() && !here->isClass() && !there->isClass();
  if (isPlainObjects) {
    auto fields = this->checkSameFields(here, there, what, file);
    if (fields.isErr()) {
      return err(fields.error());
    }
  }

  auto message = ErrorTextDeclaration::mismatch(what, here->name(), there->name(), file);

  return err(DeclarationCheckerCheckAliasError{
      .code = DeclarationCheckerCheckAliasErrorCode::TYPES_DIFFER,
      .message = message,
  });
}

DeclarationChecker::ResultAliasTypesAreSame
DeclarationChecker::checkAliasFound(const TypeAliasDecl *declared, const TypeAliasDecl *implemented,
                                    const std::string &file) const {
  if (implemented == nullptr) {
    auto message = ErrorTextDeclaration::notType(declared->name, file);

    return err(DeclarationCheckerCheckAliasError{
        .code = DeclarationCheckerCheckAliasErrorCode::NOT_A_TYPE,
        .message = message,
    });
  }

  return ok(implemented);
}

DeclarationChecker::ResultAliasTypesAreSame
DeclarationChecker::checkAliasParams(const TypeAliasDecl *declared,
                                     const TypeAliasDecl *implemented,
                                     const std::string &file) const {
  size_t count = declared->typeParams.size();
  size_t implementedCount = implemented->typeParams.size();
  if (count != implementedCount) {
    auto message = ErrorTextDeclaration::typeParams(declared->name, count, implementedCount, file);

    return err(DeclarationCheckerCheckAliasError{
        .code = DeclarationCheckerCheckAliasErrorCode::TYPE_PARAMS_DIFFER,
        .message = message,
    });
  }

  return ok(implemented);
}

DeclarationChecker::ResultFieldsAreSame
DeclarationChecker::checkSameFields(const Type *here, const Type *there, const std::string &what,
                                    const std::string &file) const {
  for (const Field &mine : here->fields()) {
    auto agreed = this->checkFieldAgrees(mine, there, what, file);
    if (agreed.isErr()) {
      return err(agreed.error());
    }
  }
  for (const Field &theirs : there->fields()) {
    auto present = this->checkFieldPresent(theirs, here, what, file);
    if (present.isErr()) {
      return err(present.error());
    }
  }

  return ok(here);
}

DeclarationChecker::ResultFieldsAreSame
DeclarationChecker::checkFieldAgrees(const Field &mine, const Type *there, const std::string &what,
                                     const std::string &file) const {
  const auto *theirs = there->field(mine.name);
  if (theirs == nullptr) {
    auto message = ErrorTextDeclaration::extraField(what, mine.name, file);

    return err(DeclarationCheckerCheckAliasError{
        .code = DeclarationCheckerCheckAliasErrorCode::FIELD_ONLY_HERE,
        .message = message,
    });
  }
  auto field = ErrorTextDeclaration::fieldOf(mine.name, what);
  if (mine.type != theirs->type) {
    auto message =
        ErrorTextDeclaration::mismatch(field, mine.type->name(), theirs->type->name(), file);

    return err(DeclarationCheckerCheckAliasError{
        .code = DeclarationCheckerCheckAliasErrorCode::FIELD_TYPE_DIFFERS,
        .message = message,
    });
  }
  if (mine.readonly != theirs->readonly) {
    auto message = ErrorTextDeclaration::readonly(field, theirs->readonly, file);

    return err(DeclarationCheckerCheckAliasError{
        .code = DeclarationCheckerCheckAliasErrorCode::READONLY_DIFFERS,
        .message = message,
    });
  }

  return ok(there);
}

DeclarationChecker::ResultFieldsAreSame
DeclarationChecker::checkFieldPresent(const Field &theirs, const Type *here,
                                      const std::string &what, const std::string &file) const {
  if (here->field(theirs.name) == nullptr) {
    auto message = ErrorTextDeclaration::absentHere(what, theirs.name, file);

    return err(DeclarationCheckerCheckAliasError{
        .code = DeclarationCheckerCheckAliasErrorCode::FIELD_ONLY_THERE,
        .message = message,
    });
  }

  return ok(here);
}

DeclarationChecker::ResultParamCountMatches
DeclarationChecker::checkParamCount(size_t here, size_t there, const std::string &what) const {
  if (here != there) {
    auto message = ErrorTextDeclaration::paramCount(what, here, there, this->fileName());

    return err(DeclarationCheckerParamCountError{
        .code = DeclarationCheckerParamCountErrorCode::COUNTS_DIFFER,
        .message = message,
    });
  }

  return ok(here);
}

DeclarationChecker::ResultParamShapeMatches
DeclarationChecker::checkParamShape(const Param &mine, const Param &theirs,
                                    const std::string &param) const {
  bool isOptionalThere = theirs.optional() || theirs.defaultValue() != nullptr;
  if (mine.optional() != isOptionalThere || mine.rest() != theirs.rest()) {
    auto message = ErrorTextDeclaration::paramKind(param, mine.optional(), this->fileName());

    return err(DeclarationCheckerParamShapeError{
        .code = DeclarationCheckerParamShapeErrorCode::SHAPE_DIFFERS,
        .message = message,
    });
  }

  return ok(&mine);
}

DeclarationChecker::ResultStoredTypeMatches
DeclarationChecker::checkStoredType(const Type *here, const Type *there,
                                    const std::string &what) const {
  if (!this->typesAgreeWidened(here, there)) {
    auto message =
        ErrorTextDeclaration::mismatch(what, here->name(), there->name(), this->fileName());

    return err(DeclarationCheckerStoredTypeError{
        .code = DeclarationCheckerStoredTypeErrorCode::TYPES_DIFFER,
        .message = message,
    });
  }

  return ok(there);
}

bool DeclarationChecker::reportParamsMismatch(const std::vector<Param> &declared,
                                              const FunctionDecl *functionDecl, llvm::SMLoc loc,
                                              const std::string &what) {
  auto counted = this->checkParamCount(declared.size(), functionDecl->params.size(), what);
  if (counted.isErr()) {
    this->state.getDiagnostics().error(loc, counted.error().message);
    return true;
  }
  for (size_t index = 0; index < declared.size(); ++index) {
    if (this->reportParamMismatch(declared[index], functionDecl->params[index], what)) {
      return true;
    }
  }

  return false;
}

bool DeclarationChecker::reportParamMismatch(const Param &mine, const Param &theirs,
                                             const std::string &what) {
  auto param = ErrorTextDeclaration::paramOf(mine.name, what);
  auto shaped = this->checkParamShape(mine, theirs, param);
  if (shaped.isErr()) {
    this->state.getDiagnostics().error(mine.loc, shaped.error().message);
    return true;
  }
  const auto *here = this->resolveWritten(mine.type);
  auto typed = ErrorTextDeclaration::typeOf(param);
  auto stored = this->checkStoredType(here, theirs.declaredType, typed);
  if (stored.isErr()) {
    this->state.getDiagnostics().error(mine.loc, stored.error().message);
    return true;
  }

  return false;
}

DeclarationChecker::ResultModuleHasFunction
DeclarationChecker::checkFunctionFound(const ScopeEntry &entry, llvm::StringRef name) const {
  if (entry.func() == nullptr || entry.func()->decl == nullptr) {
    auto message = ErrorTextDeclaration::noFunction(name, this->fileName());

    return err(DeclarationCheckerFunctionFoundError{
        .code = DeclarationCheckerFunctionFoundErrorCode::NOT_A_FUNCTION,
        .message = message,
    });
  }

  return ok(entry.func()->decl);
}

DeclarationChecker::ResultTypeParamsMatched
DeclarationChecker::checkTypeParamCount(size_t here, size_t there, const std::string &what) const {
  if (here != there) {
    auto message = ErrorTextDeclaration::otherCount(what, this->fileName());

    return err(DeclarationCheckerTypeParamCountError{
        .code = DeclarationCheckerTypeParamCountErrorCode::COUNTS_DIFFER,
        .message = message,
    });
  }

  return ok(here);
}

DeclarationChecker::ResultReturnTypeWritten
DeclarationChecker::checkReturnWritten(const std::optional<TypeRef> &ref,
                                       const std::string &what) const {
  if (!ref.has_value()) {
    auto message = ErrorTextDeclaration::returnType(what);

    return err(DeclarationCheckerReturnWrittenError{
        .code = DeclarationCheckerReturnWrittenErrorCode::NOT_WRITTEN,
        .message = message,
    });
  }

  return ok(&*ref);
}

DeclarationChecker::ResultReturnTypeMatches
DeclarationChecker::checkSameReturn(const Type *here, const Type *there,
                                    const std::string &what) const {
  if (!this->typesAgree(here, there)) {
    auto gives = ErrorTextDeclaration::gives(what);
    auto message =
        ErrorTextDeclaration::mismatch(gives, here->name(), there->name(), this->fileName());

    return err(DeclarationCheckerSameReturnError{
        .code = DeclarationCheckerSameReturnErrorCode::TYPES_DIFFER,
        .message = message,
    });
  }

  return ok(there);
}

void DeclarationChecker::walkDeclaredFunction(FunctionDecl *declared, const Exported &exported) {
  auto entry = this->topLevelChecker->lookupValue(exported.local);
  auto found = this->checkFunctionFound(entry, declared->name);
  if (found.isErr()) {
    this->state.getDiagnostics().error(declared->loc(), found.error().message);
    return;
  }
  const FunctionDecl *functionDecl = found.value();
  auto what = ErrorTextDeclaration::quoted(declared->name);
  bool isGeneric = !declared->typeParams.empty() || !functionDecl->typeParams.empty();
  if (isGeneric) {
    this->walkTypeParamCount(declared->typeParams.size(), functionDecl->typeParams.size(), what,
                             declared->loc());
    return;
  }
  if (this->reportParamsMismatch(declared->params, functionDecl, declared->loc(), what)) {
    return;
  }
  auto written = this->checkReturnWritten(declared->returnType, what);
  if (written.isErr()) {
    this->state.getDiagnostics().error(declared->loc(), written.error().message);
    return;
  }
  const auto *here = this->resolveWrittenOrVoid(declared->returnType);
  const auto *there = entry.func()->getReturnType();
  auto matches = this->checkSameReturn(here, there, what);
  if (matches.isErr()) {
    this->state.getDiagnostics().error(declared->loc(), matches.error().message);
  }
}

void DeclarationChecker::walkTypeParamCount(size_t here, size_t there, const std::string &what,
                                            llvm::SMLoc loc) {
  auto matched = this->checkTypeParamCount(here, there, what);
  if (matched.isErr()) {
    this->state.getDiagnostics().error(loc, matched.error().message);
  }
}

bool DeclarationChecker::isMentioningHidden(const FunctionDecl *functionDecl) const {
  for (const Param &param : functionDecl->params) {
    const auto *classDecl = this->paramClass(param);
    if (classDecl != nullptr && this->isHiddenClass(classDecl)) {
      return true;
    }
  }

  return false;
}

const ClassDecl *DeclarationChecker::paramClass(const Param &param) const {
  if (param.declaredType == nullptr) {
    return nullptr;
  }
  const Type *type = param.declaredType->nonOptional();

  return this->classDeclarer->classOf(type);
}

bool DeclarationChecker::isHiddenClass(const ClassDecl *classDecl) const {
  bool isShown = this->isExportedLocal(classDecl->name);
  const auto *owner = this->state.getModuleStates().findDeclModule(classDecl);

  return !isShown && owner == this->checkedModule;
}

bool DeclarationChecker::isExportedLocal(llvm::StringRef name) const {
  for (const auto &entry : this->exportedNames) {
    if (entry.second.local == name) {
      return true;
    }
  }

  return false;
}

DeclarationChecker::ResultDeclaredFieldForm
DeclarationChecker::checkDeclaredField(const DeclaredMember &member,
                                       const std::string &what) const {
  const DeclaredField *declared = member.form.as<DeclaredField>();
  if (declared == nullptr) {
    auto message = ErrorTextDeclaredClass::isField(what, this->fileName());

    return err(DeclarationCheckerDeclaredFieldError{
        .code = DeclarationCheckerDeclaredFieldErrorCode::IS_A_FIELD,
        .message = message,
    });
  }

  return ok(declared);
}

DeclarationChecker::ResultReadonlyFlagsSame
DeclarationChecker::checkReadonlyAgrees(const DeclaredField &declared, const ClassField &field,
                                        const std::string &what) const {
  if (declared.readonly != field.readonly) {
    auto message = ErrorTextDeclaration::readonly(what, field.readonly, this->fileName());

    return err(DeclarationCheckerReadonlyAgreesError{
        .code = DeclarationCheckerReadonlyAgreesErrorCode::READONLY_DIFFERS,
        .message = message,
    });
  }

  return ok(&field);
}

DeclarationChecker::ResultOptionalFlagsSame
DeclarationChecker::checkOptionalAgrees(const DeclaredField &declared, const ClassField &field,
                                        const std::string &what) const {
  if (declared.optional != field.optional()) {
    auto message = ErrorTextDeclaration::optional(what, field.optional(), this->fileName());

    return err(DeclarationCheckerOptionalAgreesError{
        .code = DeclarationCheckerOptionalAgreesErrorCode::OPTIONAL_DIFFERS,
        .message = message,
    });
  }

  return ok(&field);
}

void DeclarationChecker::walkDeclaredField(const DeclaredMember &member, const ClassField &field,
                                           const ClassDecl *classDecl) {
  auto what = ErrorTextDeclaration::memberOf(member.name, classDecl->name);
  auto form = this->checkDeclaredField(member, what);
  if (form.isErr()) {
    this->state.getDiagnostics().error(member.loc, form.error().message);
    return;
  }
  const DeclaredField *declared = form.value();
  auto readonlyKept = this->checkReadonlyAgrees(*declared, field, what);
  if (readonlyKept.isErr()) {
    this->state.getDiagnostics().error(member.loc, readonlyKept.error().message);
  }
  auto optionalKept = this->checkOptionalAgrees(*declared, field, what);
  if (optionalKept.isErr()) {
    this->state.getDiagnostics().error(member.loc, optionalKept.error().message);
  }
  const auto *here = this->resolveWritten(member.type);
  auto typed = ErrorTextDeclaration::typeOf(what);
  auto stored = this->checkStoredType(here, field.resolved, typed);
  if (stored.isErr()) {
    this->state.getDiagnostics().error(member.loc, stored.error().message);
  }
}

const DeclaredSignature *DeclarationChecker::memberSignature(const DeclaredMember &member) const {
  switch (member.form.kind()) {
    case DeclaredMemberKind::DECLARED_FIELD:
      return nullptr;
    case DeclaredMemberKind::DECLARED_METHOD:
      return &member.form.as<DeclaredMethod>()->signature;
    case DeclaredMemberKind::DECLARED_CONSTRUCTOR:
      return &member.form.as<DeclaredConstructor>()->signature;
  }

  return nullptr;
}

DeclarationChecker::ResultDeclaredSignature
DeclarationChecker::checkMethodSignature(const DeclaredMember &member,
                                         const std::string &what) const {
  const auto *signature = this->memberSignature(member);
  if (signature == nullptr) {
    auto message = ErrorTextDeclaredClass::isMethod(what, this->fileName());

    return err(DeclarationCheckerMethodSignatureError{
        .code = DeclarationCheckerMethodSignatureErrorCode::IS_A_METHOD,
        .message = message,
    });
  }

  return ok(signature);
}

void DeclarationChecker::walkDeclaredMethod(const DeclaredMember &member,
                                            const FunctionDecl *functionDecl,
                                            const ClassDecl *classDecl) {
  auto what = ErrorTextDeclaration::memberOf(member.name, classDecl->name);
  auto signature = this->checkMethodSignature(member, what);
  if (signature.isErr()) {
    this->state.getDiagnostics().error(member.loc, signature.error().message);
    return;
  }
  const DeclaredSignature *declared = signature.value();
  bool isGeneric = !declared->typeParams.empty() || !functionDecl->typeParams.empty();
  if (isGeneric) {
    this->walkTypeParamCount(declared->typeParams.size(), functionDecl->typeParams.size(), what,
                             member.loc);
    return;
  }
  bool isConstructor = member.form.is<DeclaredConstructor>();
  bool isMismatched = this->reportParamsMismatch(declared->params, functionDecl, member.loc, what);
  if (isMismatched || isConstructor) {
    return;
  }
  auto written = this->checkReturnWritten(member.type, what);
  if (written.isErr()) {
    this->state.getDiagnostics().error(member.loc, written.error().message);
    return;
  }
  const auto *here = this->resolveWrittenOrVoid(member.type);
  const auto *there = functionDecl->sym->getReturnType();
  auto matches = this->checkSameReturn(here, there, what);
  if (matches.isErr()) {
    this->state.getDiagnostics().error(member.loc, matches.error().message);
  }
}

DeclarationChecker::ResultModuleHasTheClass
DeclarationChecker::checkClassFound(const DeclaredClass &declared, const Exported &exported) const {
  auto *found = this->typeDeclarer->lookupType(exported.local);
  auto *classDecl = dyn_cast_or_null<ClassDecl>(found);
  if (classDecl == nullptr) {
    auto message = ErrorTextDeclaredClass::notClass(declared.name, this->fileName());

    return err(DeclarationCheckerClassFoundError{
        .code = DeclarationCheckerClassFoundErrorCode::NOT_A_CLASS,
        .message = message,
    });
  }

  return ok(classDecl);
}

DeclarationChecker::ResultAccessWordWritten
DeclarationChecker::checkAccessWritten(const DeclaredMember &member,
                                       const std::string &what) const {
  if (!member.accessWritten) {
    auto message = ErrorTextDeclaredClass::noAccess(what);

    return err(DeclarationCheckerAccessWrittenError{
        .code = DeclarationCheckerAccessWrittenErrorCode::NOT_WRITTEN,
        .message = message,
    });
  }

  return ok(&member);
}

const FunctionDecl *DeclarationChecker::lastCallableConstructor(const ClassDecl *classDecl) const {
  for (const FunctionDecl *constructor : llvm::reverse(classDecl->constructors)) {
    if (constructor->access() != Access::Private) {
      return constructor;
    }
  }

  return nullptr;
}

DeclarationChecker::ResultConstructorExists
DeclarationChecker::checkCallableConstructor(const ClassDecl *classDecl) const {
  const auto *callable = this->lastCallableConstructor(classDecl);
  if (callable == nullptr) {
    auto message = ErrorTextDeclaredClass::noCallable(classDecl->name, this->fileName());

    return err(DeclarationCheckerCallableConstructorError{
        .code = DeclarationCheckerCallableConstructorErrorCode::NO_CONSTRUCTOR,
        .message = message,
    });
  }

  return ok(callable);
}

DeclarationChecker::ResultConstructorAccess
DeclarationChecker::checkConstructorAccess(const DeclaredMember &member,
                                           const FunctionDecl *constructor,
                                           const ClassDecl *classDecl) const {
  if (member.access != constructor->access()) {
    const char *word = this->accessWord(constructor->access());
    auto message = ErrorTextDeclaredClass::newAccess(classDecl->name, word, this->fileName());

    return err(DeclarationCheckerConstructorAccessError{
        .code = DeclarationCheckerConstructorAccessErrorCode::ACCESS_DIFFERS,
        .message = message,
    });
  }

  return ok(constructor);
}

DeclarationChecker::ResultClassHasTheMember
DeclarationChecker::checkMemberFound(const DeclaredMember &member, const ClassDecl *classDecl,
                                     const ClassField *field, const FunctionDecl *method) const {
  if (field == nullptr && method == nullptr) {
    auto message = ErrorTextDeclaredClass::noMember(classDecl->name, member.name, member.isStatic,
                                                    this->fileName());

    return err(DeclarationCheckerMemberFoundError{
        .code = DeclarationCheckerMemberFoundErrorCode::NO_MEMBER,
        .message = message,
    });
  }

  return ok(&member);
}

DeclarationChecker::ResultPublicOrProtected
DeclarationChecker::checkMemberVisible(Access access, const std::string &what) const {
  if (access == Access::Private) {
    auto message = ErrorTextDeclaredClass::isPrivate(what, this->fileName());

    return err(DeclarationCheckerMemberVisibleError{
        .code = DeclarationCheckerMemberVisibleErrorCode::PRIVATE_MEMBER,
        .message = message,
    });
  }

  return ok(access);
}

DeclarationChecker::ResultAccessWordMatches
DeclarationChecker::checkAccessAgrees(const DeclaredMember &member, Access access,
                                      const std::string &what) const {
  if (member.accessWritten && member.access != access) {
    const char *word = this->accessWord(access);
    auto message = ErrorTextDeclaredClass::accessIs(what, word, this->fileName());

    return err(DeclarationCheckerAccessAgreesError{
        .code = DeclarationCheckerAccessAgreesErrorCode::ACCESS_DIFFERS,
        .message = message,
    });
  }

  return ok(access);
}

DeclarationChecker::ResultMemberWasDeclared
DeclarationChecker::checkMemberListed(const llvm::StringSet<> &seen, llvm::StringRef name,
                                      bool isStatic, const ClassDecl *classDecl) const {
  auto key = this->memberKey(name, isStatic);
  if (!seen.contains(key)) {
    auto declarations = llvm::sys::path::filename(this->checkedModule->declaration->path).str();
    auto message =
        ErrorTextDeclaredClass::undeclared(name, classDecl->name, isStatic, declarations);

    return err(DeclarationCheckerMemberListedError{
        .code = DeclarationCheckerMemberListedErrorCode::NOT_DECLARED,
        .message = message,
    });
  }

  return ok(classDecl);
}

void DeclarationChecker::walkDeclaredConstructor(const DeclaredMember &member,
                                                 const ClassDecl *classDecl) {
  auto exists = this->checkCallableConstructor(classDecl);
  if (exists.isErr()) {
    this->state.getDiagnostics().error(member.loc, exists.error().message);
    return;
  }
  const FunctionDecl *constructor = exists.value();
  auto kept = this->checkConstructorAccess(member, constructor, classDecl);
  if (kept.isErr()) {
    this->state.getDiagnostics().error(member.loc, kept.error().message);
  }
  this->walkDeclaredMethod(member, constructor, classDecl);
}

void DeclarationChecker::walkDeclaredMember(const DeclaredMember &member,
                                            const ClassDecl *classDecl) {
  auto what = ErrorTextDeclaration::memberOf(member.name, classDecl->name);
  auto written = this->checkAccessWritten(member, what);
  if (written.isErr()) {
    this->state.getDiagnostics().error(member.loc, written.error().message);
  }
  if (member.form.is<DeclaredConstructor>()) {
    this->walkDeclaredConstructor(member, classDecl);
    return;
  }
  const auto *field = this->classField(classDecl, member);
  const auto *method = this->classMethod(classDecl, member);
  auto found = this->checkMemberFound(member, classDecl, field, method);
  if (found.isErr()) {
    this->state.getDiagnostics().error(member.loc, found.error().message);
    return;
  }
  Access access = field != nullptr ? field->access : method->access();
  auto visible = this->checkMemberVisible(access, what);
  if (visible.isErr()) {
    this->state.getDiagnostics().error(member.loc, visible.error().message);
    return;
  }
  auto matches = this->checkAccessAgrees(member, access, what);
  if (matches.isErr()) {
    this->state.getDiagnostics().error(member.loc, matches.error().message);
  }
  if (field != nullptr) {
    this->walkDeclaredField(member, *field, classDecl);
    return;
  }
  this->walkDeclaredMethod(member, method, classDecl);
}

void DeclarationChecker::walkListed(const ClassDecl *classDecl, const llvm::StringSet<> &seen) {
  for (const ClassField &field : classDecl->fields) {
    this->walkListedField(field, classDecl, seen);
  }
  for (const FunctionDecl *method : classDecl->methods) {
    this->walkListedMethod(method, classDecl, seen);
  }
  for (const FunctionDecl *constructor : classDecl->constructors) {
    this->walkListedConstructor(constructor, classDecl, seen);
  }
}

void DeclarationChecker::walkListedField(const ClassField &field, const ClassDecl *classDecl,
                                         const llvm::StringSet<> &seen) {
  if (field.method() != nullptr || field.access == Access::Private) {
    return;
  }
  auto listed = this->checkMemberListed(seen, field.name, field.isStatic(), classDecl);
  if (listed.isErr()) {
    this->state.getDiagnostics().error(field.loc, listed.error().message);
  }
}

void DeclarationChecker::walkListedMethod(const FunctionDecl *method, const ClassDecl *classDecl,
                                          const llvm::StringSet<> &seen) {
  if (method->access() == Access::Private) {
    return;
  }
  auto listed = this->checkMemberListed(seen, method->name, method->isStatic(), classDecl);
  if (listed.isErr()) {
    this->state.getDiagnostics().error(method->loc(), listed.error().message);
  }
}

void DeclarationChecker::walkListedConstructor(const FunctionDecl *constructor,
                                               const ClassDecl *classDecl,
                                               const llvm::StringSet<> &seen) {
  if (constructor->access() == Access::Private || this->isMentioningHidden(constructor)) {
    return;
  }
  auto name = ObjectMemberTable::getName(ObjectMember::CONSTRUCTOR);
  auto listed = this->checkMemberListed(seen, name, constructor->isStatic(), classDecl);
  if (listed.isErr()) {
    this->state.getDiagnostics().error(constructor->loc(), listed.error().message);
  }
}

void DeclarationChecker::walkDeclaredClass(const DeclaredClass &declared,
                                           const Exported &exported) {
  auto found = this->checkClassFound(declared, exported);
  if (found.isErr()) {
    this->state.getDiagnostics().error(declared.loc, found.error().message);
    return;
  }
  ClassDecl *classDecl = found.value();
  auto name = ErrorTextDeclaration::quoted(declared.name);
  auto matched =
      this->checkTypeParamCount(declared.typeParams.size(), classDecl->typeParams.size(), name);
  if (matched.isErr()) {
    this->state.getDiagnostics().error(declared.loc, matched.error().message);
    return;
  }
  if (!classDecl->typeParams.empty()) {
    return;
  }
  this->classDeclarer->completeClass(classDecl);
  auto seen = llvm::StringSet<>();
  for (const DeclaredMember &member : declared.members) {
    auto key = this->memberKey(member.name, member.isStatic);
    seen.insert(key);
    this->walkDeclaredMember(member, classDecl);
  }
  this->walkListed(classDecl, seen);
}

}
