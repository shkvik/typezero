#include "compiler/semantic/declaration/files/declaration_files_registry.h"
#include "compiler/program/library/builtin_names/access_keyword_table/access_keyword_table.h"
#include "compiler/program/library/object/object_member_table/object_member_table.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Path.h"

using llvm::dyn_cast;
using llvm::dyn_cast_or_null;

namespace tz {

namespace {

constexpr const char *kStaticPrefix = "static ";

}

DeclarationFilesRegistry::DeclarationFilesRegistry(SemanticState &owner)
    : state(owner) {}

bool DeclarationFilesRegistry::typesAgree(const Type *here, const Type *there) const {
  return here == nullptr || there == nullptr || here == there || here->isError() ||
         there->isError();
}

bool DeclarationFilesRegistry::typesAgreeWidened(const Type *here, const Type *there) const {
  return this->typesAgree(here, there) || Type::optional(here) == there;
}

std::string DeclarationFilesRegistry::memberKey(llvm::StringRef name, bool isStatic) const {
  if (!isStatic) {
    return name.str();
  }

  return kStaticPrefix + name.str();
}

const ClassField *DeclarationFilesRegistry::classField(const ClassDecl *classDecl,
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

const FunctionDecl *DeclarationFilesRegistry::classMethod(const ClassDecl *classDecl,
                                                          const DeclaredMember &member) const {
  for (const FunctionDecl *candidate : llvm::reverse(classDecl->methods)) {
    if (candidate->name == member.name && candidate->isStatic() == member.isStatic) {
      return candidate;
    }
  }

  return nullptr;
}

std::string DeclarationFilesRegistry::fileName() const {
  return llvm::sys::path::filename(this->checkedModule->path).str();
}

DeclarationFilesRegistry::ResultModuleExportsName
DeclarationFilesRegistry::checkExportFound(llvm::StringRef name) const {
  auto found = this->exportedNames.find(name);
  if (found == this->exportedNames.end()) {
    auto message = ErrorTextExports::unexported(name, this->fileName());

    return err(DeclarationFilesRegistryExportFoundError{
        .code = DeclarationFilesRegistryExportFoundErrorCode::NOT_EXPORTED,
        .message = message,
    });
  }

  return ok(&found->second);
}

DeclarationFilesRegistry::ResultExportWasDeclared DeclarationFilesRegistry::checkExportDeclared(
    llvm::StringRef name, const llvm::StringSet<> &declared, llvm::StringRef declarations) const {
  if (!declared.contains(name)) {
    auto message = ErrorTextExports::undeclared(name, declarations);

    return err(DeclarationFilesRegistryExportDeclaredError{
        .code = DeclarationFilesRegistryExportDeclaredErrorCode::NOT_DECLARED,
        .message = message,
    });
  }

  return ok(name);
}

void DeclarationFilesRegistry::collectExportedNames(const Module *module) {
  this->exportedNames.clear();
  for (const ExportDecl &exportDecl : module->file.exports) {
    this->addLocalExports(exportDecl);
  }
}

void DeclarationFilesRegistry::addLocalExports(const ExportDecl &exportDecl) {
  const LocalExport *local = exportDecl.form.as<LocalExport>();
  if (local == nullptr) {
    return;
  }
  for (const ModuleName &name : local->names) {
    llvm::StringRef shown = name.as.empty() ? name.name : name.as;
    this->exportedNames[shown] = Exported{.local = name.name, .loc = name.loc};
  }
}

void DeclarationFilesRegistry::walkExportsDeclared(const DeclarationFile &file,
                                                   const llvm::StringSet<> &declared) {
  auto declarations = llvm::sys::path::filename(file.path).str();
  for (const auto &entry : this->exportedNames) {
    auto listed = this->checkExportDeclared(entry.getKey(), declared, declarations);
    if (listed.isErr()) {
      this->state.diagnostics.error(entry.second.loc, listed.error().message);
    }
  }
}

DeclarationFilesRegistry::ResultAliasTypesAreSame
DeclarationFilesRegistry::checkAliasFound(const TypeAliasDecl *declared,
                                          const TypeAliasDecl *implemented,
                                          const std::string &file) const {
  if (implemented == nullptr) {
    auto message = ErrorTextDeclaration::notType(declared->name, file);

    return err(DeclarationFilesRegistryCheckAliasError{
        .code = DeclarationFilesRegistryCheckAliasErrorCode::NOT_A_TYPE,
        .message = message,
    });
  }

  return ok(implemented);
}

DeclarationFilesRegistry::ResultAliasTypesAreSame
DeclarationFilesRegistry::checkAliasParams(const TypeAliasDecl *declared,
                                           const TypeAliasDecl *implemented,
                                           const std::string &file) const {
  size_t count = declared->typeParams.size();
  size_t implementedCount = implemented->typeParams.size();
  if (count != implementedCount) {
    auto message = ErrorTextDeclaration::typeParams(declared->name, count, implementedCount, file);

    return err(DeclarationFilesRegistryCheckAliasError{
        .code = DeclarationFilesRegistryCheckAliasErrorCode::TYPE_PARAMS_DIFFER,
        .message = message,
    });
  }

  return ok(implemented);
}

DeclarationFilesRegistry::ResultFieldsAreSame
DeclarationFilesRegistry::checkSameFields(const Type *here, const Type *there,
                                          const std::string &what, const std::string &file) const {
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

DeclarationFilesRegistry::ResultFieldsAreSame
DeclarationFilesRegistry::checkFieldAgrees(const Field &mine, const Type *there,
                                           const std::string &what, const std::string &file) const {
  const auto *theirs = there->field(mine.name);
  if (theirs == nullptr) {
    auto message = ErrorTextDeclaration::extraField(what, mine.name, file);

    return err(DeclarationFilesRegistryCheckAliasError{
        .code = DeclarationFilesRegistryCheckAliasErrorCode::FIELD_ONLY_HERE,
        .message = message,
    });
  }
  auto field = ErrorTextDeclaration::fieldOf(mine.name, what);
  if (mine.type != theirs->type) {
    auto message =
        ErrorTextDeclaration::mismatch(field, mine.type->name(), theirs->type->name(), file);

    return err(DeclarationFilesRegistryCheckAliasError{
        .code = DeclarationFilesRegistryCheckAliasErrorCode::FIELD_TYPE_DIFFERS,
        .message = message,
    });
  }
  if (mine.readonly != theirs->readonly) {
    auto message = ErrorTextDeclaration::readonly(field, theirs->readonly, file);

    return err(DeclarationFilesRegistryCheckAliasError{
        .code = DeclarationFilesRegistryCheckAliasErrorCode::READONLY_DIFFERS,
        .message = message,
    });
  }

  return ok(there);
}

DeclarationFilesRegistry::ResultFieldsAreSame DeclarationFilesRegistry::checkFieldPresent(
    const Field &theirs, const Type *here, const std::string &what, const std::string &file) const {
  if (here->field(theirs.name) == nullptr) {
    auto message = ErrorTextDeclaration::absentHere(what, theirs.name, file);

    return err(DeclarationFilesRegistryCheckAliasError{
        .code = DeclarationFilesRegistryCheckAliasErrorCode::FIELD_ONLY_THERE,
        .message = message,
    });
  }

  return ok(here);
}

DeclarationFilesRegistry::ResultParamCountMatches
DeclarationFilesRegistry::checkParamCount(size_t here, size_t there,
                                          const std::string &what) const {
  if (here != there) {
    auto message = ErrorTextDeclaration::paramCount(what, here, there, this->fileName());

    return err(DeclarationFilesRegistryParamCountError{
        .code = DeclarationFilesRegistryParamCountErrorCode::COUNTS_DIFFER,
        .message = message,
    });
  }

  return ok(here);
}

DeclarationFilesRegistry::ResultParamShapeMatches
DeclarationFilesRegistry::checkParamShape(const Param &mine, const Param &theirs,
                                          const std::string &param) const {
  bool isOptionalThere = theirs.optional() || theirs.defaultValue() != nullptr;
  if (mine.optional() != isOptionalThere || mine.rest() != theirs.rest()) {
    auto message = ErrorTextDeclaration::paramKind(param, mine.optional(), this->fileName());

    return err(DeclarationFilesRegistryParamShapeError{
        .code = DeclarationFilesRegistryParamShapeErrorCode::SHAPE_DIFFERS,
        .message = message,
    });
  }

  return ok(&mine);
}

DeclarationFilesRegistry::ResultStoredTypeMatches
DeclarationFilesRegistry::checkStoredType(const Type *here, const Type *there,
                                          const std::string &what) const {
  if (!this->typesAgreeWidened(here, there)) {
    auto message =
        ErrorTextDeclaration::mismatch(what, here->name(), there->name(), this->fileName());

    return err(DeclarationFilesRegistryStoredTypeError{
        .code = DeclarationFilesRegistryStoredTypeErrorCode::TYPES_DIFFER,
        .message = message,
    });
  }

  return ok(there);
}

DeclarationFilesRegistry::ResultModuleHasFunction
DeclarationFilesRegistry::checkFunctionFound(const ScopeEntry &entry, llvm::StringRef name) const {
  if (entry.func() == nullptr || entry.func()->decl == nullptr) {
    auto message = ErrorTextDeclaration::noFunction(name, this->fileName());

    return err(DeclarationFilesRegistryFunctionFoundError{
        .code = DeclarationFilesRegistryFunctionFoundErrorCode::NOT_A_FUNCTION,
        .message = message,
    });
  }

  return ok(entry.func()->decl);
}

DeclarationFilesRegistry::ResultTypeParamsMatched
DeclarationFilesRegistry::checkTypeParamCount(size_t here, size_t there,
                                              const std::string &what) const {
  if (here != there) {
    auto message = ErrorTextDeclaration::otherCount(what, this->fileName());

    return err(DeclarationFilesRegistryTypeParamCountError{
        .code = DeclarationFilesRegistryTypeParamCountErrorCode::COUNTS_DIFFER,
        .message = message,
    });
  }

  return ok(here);
}

DeclarationFilesRegistry::ResultReturnTypeWritten
DeclarationFilesRegistry::checkReturnWritten(const std::optional<TypeRef> &ref,
                                             const std::string &what) const {
  if (!ref.has_value()) {
    auto message = ErrorTextDeclaration::returnType(what);

    return err(DeclarationFilesRegistryReturnWrittenError{
        .code = DeclarationFilesRegistryReturnWrittenErrorCode::NOT_WRITTEN,
        .message = message,
    });
  }

  return ok(&*ref);
}

DeclarationFilesRegistry::ResultReturnTypeMatches
DeclarationFilesRegistry::checkSameReturn(const Type *here, const Type *there,
                                          const std::string &what) const {
  if (!this->typesAgree(here, there)) {
    auto gives = ErrorTextDeclaration::gives(what);
    auto message =
        ErrorTextDeclaration::mismatch(gives, here->name(), there->name(), this->fileName());

    return err(DeclarationFilesRegistrySameReturnError{
        .code = DeclarationFilesRegistrySameReturnErrorCode::TYPES_DIFFER,
        .message = message,
    });
  }

  return ok(there);
}

void DeclarationFilesRegistry::walkTypeParamCount(size_t here, size_t there,
                                                  const std::string &what, llvm::SMLoc loc) {
  auto matched = this->checkTypeParamCount(here, there, what);
  if (matched.isErr()) {
    this->state.diagnostics.error(loc, matched.error().message);
  }
}

bool DeclarationFilesRegistry::isMentioningHidden(const FunctionDecl *functionDecl) const {
  for (const Param &param : functionDecl->params) {
    const auto *classDecl = this->paramClass(param);
    if (classDecl != nullptr && this->isHiddenClass(classDecl)) {
      return true;
    }
  }

  return false;
}

const ClassDecl *DeclarationFilesRegistry::paramClass(const Param &param) const {
  if (param.declaredType == nullptr) {
    return nullptr;
  }
  const Type *type = param.declaredType->nonOptional();

  return type->classDecl();
}

bool DeclarationFilesRegistry::isHiddenClass(const ClassDecl *classDecl) const {
  bool isShown = this->isExportedLocal(classDecl->name);
  const auto *owner = this->state.declaration.modules.findDeclModule(classDecl);

  return !isShown && owner == this->checkedModule;
}

bool DeclarationFilesRegistry::isExportedLocal(llvm::StringRef name) const {
  for (const auto &entry : this->exportedNames) {
    if (entry.second.local == name) {
      return true;
    }
  }

  return false;
}

DeclarationFilesRegistry::ResultDeclaredFieldForm
DeclarationFilesRegistry::checkDeclaredField(const DeclaredMember &member,
                                             const std::string &what) const {
  const DeclaredField *declared = member.form.as<DeclaredField>();
  if (declared == nullptr) {
    auto message = ErrorTextDeclaredClass::isField(what, this->fileName());

    return err(DeclarationFilesRegistryDeclaredFieldError{
        .code = DeclarationFilesRegistryDeclaredFieldErrorCode::IS_A_FIELD,
        .message = message,
    });
  }

  return ok(declared);
}

DeclarationFilesRegistry::ResultReadonlyFlagsSame DeclarationFilesRegistry::checkReadonlyAgrees(
    const DeclaredField &declared, const ClassField &field, const std::string &what) const {
  if (declared.readonly != field.readonly) {
    auto message = ErrorTextDeclaration::readonly(what, field.readonly, this->fileName());

    return err(DeclarationFilesRegistryReadonlyAgreesError{
        .code = DeclarationFilesRegistryReadonlyAgreesErrorCode::READONLY_DIFFERS,
        .message = message,
    });
  }

  return ok(&field);
}

DeclarationFilesRegistry::ResultOptionalFlagsSame DeclarationFilesRegistry::checkOptionalAgrees(
    const DeclaredField &declared, const ClassField &field, const std::string &what) const {
  if (declared.optional != field.optional()) {
    auto message = ErrorTextDeclaration::optional(what, field.optional(), this->fileName());

    return err(DeclarationFilesRegistryOptionalAgreesError{
        .code = DeclarationFilesRegistryOptionalAgreesErrorCode::OPTIONAL_DIFFERS,
        .message = message,
    });
  }

  return ok(&field);
}

DeclaredSignature *DeclarationFilesRegistry::memberSignature(DeclaredMember &member) const {
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

DeclarationFilesRegistry::ResultDeclaredSignature
DeclarationFilesRegistry::checkMethodSignature(DeclaredMember &member,
                                               const std::string &what) const {
  const auto *signature = this->memberSignature(member);
  if (signature == nullptr) {
    auto message = ErrorTextDeclaredClass::isMethod(what, this->fileName());

    return err(DeclarationFilesRegistryMethodSignatureError{
        .code = DeclarationFilesRegistryMethodSignatureErrorCode::IS_A_METHOD,
        .message = message,
    });
  }

  return ok(signature);
}

DeclarationFilesRegistry::ResultModuleHasTheClass
DeclarationFilesRegistry::checkClassFound(const DeclaredClass &declared,
                                          const Exported &exported) const {
  auto *found = this->state.findTypeDeclaration(exported.local);
  auto *classDecl = dyn_cast_or_null<ClassDecl>(found);
  if (classDecl == nullptr) {
    auto message = ErrorTextDeclaredClass::notClass(declared.name, this->fileName());

    return err(DeclarationFilesRegistryClassFoundError{
        .code = DeclarationFilesRegistryClassFoundErrorCode::NOT_A_CLASS,
        .message = message,
    });
  }

  return ok(classDecl);
}

DeclarationFilesRegistry::ResultAccessWordWritten
DeclarationFilesRegistry::checkAccessWritten(const DeclaredMember &member,
                                             const std::string &what) const {
  if (!member.accessWritten) {
    auto message = ErrorTextDeclaredClass::noAccess(what);

    return err(DeclarationFilesRegistryAccessWrittenError{
        .code = DeclarationFilesRegistryAccessWrittenErrorCode::NOT_WRITTEN,
        .message = message,
    });
  }

  return ok(&member);
}

const FunctionDecl *
DeclarationFilesRegistry::lastCallableConstructor(const ClassDecl *classDecl) const {
  for (const FunctionDecl *constructor : llvm::reverse(classDecl->constructors)) {
    if (constructor->access() != Access::Private) {
      return constructor;
    }
  }

  return nullptr;
}

DeclarationFilesRegistry::ResultConstructorExists
DeclarationFilesRegistry::checkCallableConstructor(const ClassDecl *classDecl) const {
  const auto *callable = this->lastCallableConstructor(classDecl);
  if (callable == nullptr) {
    auto message = ErrorTextDeclaredClass::noCallable(classDecl->name, this->fileName());

    return err(DeclarationFilesRegistryCallableConstructorError{
        .code = DeclarationFilesRegistryCallableConstructorErrorCode::NO_CONSTRUCTOR,
        .message = message,
    });
  }

  return ok(callable);
}

DeclarationFilesRegistry::ResultConstructorAccess
DeclarationFilesRegistry::checkConstructorAccess(const DeclaredMember &member,
                                                 const FunctionDecl *constructor,
                                                 const ClassDecl *classDecl) const {
  if (member.access != constructor->access()) {
    auto word = AccessKeywordTable::getName(constructor->access());
    auto message = ErrorTextDeclaredClass::newAccess(classDecl->name, word, this->fileName());

    return err(DeclarationFilesRegistryConstructorAccessError{
        .code = DeclarationFilesRegistryConstructorAccessErrorCode::ACCESS_DIFFERS,
        .message = message,
    });
  }

  return ok(constructor);
}

DeclarationFilesRegistry::ResultClassHasTheMember
DeclarationFilesRegistry::checkMemberFound(const DeclaredMember &member, const ClassDecl *classDecl,
                                           const ClassField *field,
                                           const FunctionDecl *method) const {
  if (field == nullptr && method == nullptr) {
    auto message = ErrorTextDeclaredClass::noMember(classDecl->name, member.name, member.isStatic,
                                                    this->fileName());

    return err(DeclarationFilesRegistryMemberFoundError{
        .code = DeclarationFilesRegistryMemberFoundErrorCode::NO_MEMBER,
        .message = message,
    });
  }

  return ok(&member);
}

DeclarationFilesRegistry::ResultPublicOrProtected
DeclarationFilesRegistry::checkMemberVisible(Access access, const std::string &what) const {
  if (access == Access::Private) {
    auto message = ErrorTextDeclaredClass::isPrivate(what, this->fileName());

    return err(DeclarationFilesRegistryMemberVisibleError{
        .code = DeclarationFilesRegistryMemberVisibleErrorCode::PRIVATE_MEMBER,
        .message = message,
    });
  }

  return ok(access);
}

DeclarationFilesRegistry::ResultAccessWordMatches
DeclarationFilesRegistry::checkAccessAgrees(const DeclaredMember &member, Access access,
                                            const std::string &what) const {
  if (member.accessWritten && member.access != access) {
    auto word = AccessKeywordTable::getName(access);
    auto message = ErrorTextDeclaredClass::accessIs(what, word, this->fileName());

    return err(DeclarationFilesRegistryAccessAgreesError{
        .code = DeclarationFilesRegistryAccessAgreesErrorCode::ACCESS_DIFFERS,
        .message = message,
    });
  }

  return ok(access);
}

DeclarationFilesRegistry::ResultMemberWasDeclared
DeclarationFilesRegistry::checkMemberListed(const llvm::StringSet<> &seen, llvm::StringRef name,
                                            bool isStatic, const ClassDecl *classDecl) const {
  auto key = this->memberKey(name, isStatic);
  if (!seen.contains(key)) {
    auto declarations = llvm::sys::path::filename(this->checkedModule->declaration->path).str();
    auto message =
        ErrorTextDeclaredClass::undeclared(name, classDecl->name, isStatic, declarations);

    return err(DeclarationFilesRegistryMemberListedError{
        .code = DeclarationFilesRegistryMemberListedErrorCode::NOT_DECLARED,
        .message = message,
    });
  }

  return ok(classDecl);
}

void DeclarationFilesRegistry::walkListed(const ClassDecl *classDecl,
                                          const llvm::StringSet<> &seen) {
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

void DeclarationFilesRegistry::walkListedField(const ClassField &field, const ClassDecl *classDecl,
                                               const llvm::StringSet<> &seen) {
  if (field.method() != nullptr || field.access == Access::Private) {
    return;
  }
  auto listed = this->checkMemberListed(seen, field.name, field.isStatic(), classDecl);
  if (listed.isErr()) {
    this->state.diagnostics.error(field.loc, listed.error().message);
  }
}

void DeclarationFilesRegistry::walkListedMethod(const FunctionDecl *method,
                                                const ClassDecl *classDecl,
                                                const llvm::StringSet<> &seen) {
  if (method->access() == Access::Private) {
    return;
  }
  auto listed = this->checkMemberListed(seen, method->name, method->isStatic(), classDecl);
  if (listed.isErr()) {
    this->state.diagnostics.error(method->loc(), listed.error().message);
  }
}

void DeclarationFilesRegistry::walkListedConstructor(const FunctionDecl *constructor,
                                                     const ClassDecl *classDecl,
                                                     const llvm::StringSet<> &seen) {
  if (constructor->access() == Access::Private || this->isMentioningHidden(constructor)) {
    return;
  }
  auto name = ObjectMemberTable::getName(ObjectMember::CONSTRUCTOR);
  auto listed = this->checkMemberListed(seen, name, constructor->isStatic(), classDecl);
  if (listed.isErr()) {
    this->state.diagnostics.error(constructor->loc(), listed.error().message);
  }
}

void DeclarationFilesRegistry::startDeclarationFile(const StartDeclarationFile &task) {
  Module *module = task.module;
  this->checkedModule = module;
  this->declaredNames.clear();
  this->collectExportedNames(module);
  auto *outer = this->state.declaration.modules.getCurrentModule();
  if (outer != nullptr) {
    this->state.walk.tasks.pushTask(UseModule{.module = outer});
  }
  this->state.walk.tasks.pushTask(FinishDeclarationFile{.module = module});
  DeclarationFile &file = *module->declaration;
  for (DeclaredClass &declared : llvm::reverse(file.classes)) {
    this->state.walk.tasks.pushTask(StartDeclaredClass{.declared = &declared});
  }
  for (Stmt *item : llvm::reverse(file.items)) {
    this->pushFileItem(item);
  }
  this->state.walk.tasks.pushTask(UseModule{.module = module});
}

void DeclarationFilesRegistry::pushFileItem(Stmt *item) {
  auto *alias = dyn_cast<TypeAliasDecl>(item);
  if (alias != nullptr) {
    return this->state.walk.tasks.pushTask(StartDeclaredAlias{.declared = alias});
  }
  auto *functionDecl = dyn_cast<FunctionDecl>(item);
  if (functionDecl != nullptr) {
    this->state.walk.tasks.pushTask(StartDeclaredFunction{.declared = functionDecl});
  }
}

void DeclarationFilesRegistry::finishDeclarationFile(const FinishDeclarationFile &task) {
  const DeclarationFile &file = *task.module->declaration;
  this->walkExportsDeclared(file, this->declaredNames);
}

void DeclarationFilesRegistry::startDeclaredAlias(const StartDeclaredAlias &task) {
  TypeAliasDecl *declared = task.declared;
  this->declaredNames.insert(declared->name);
  auto exported = this->checkExportFound(declared->name);
  if (exported.isErr()) {
    this->state.diagnostics.error(declared->loc(), exported.error().message);
    return;
  }
  auto *found = this->state.findTypeDeclaration(exported.value()->local);
  auto *implemented = dyn_cast_or_null<TypeAliasDecl>(found);
  auto file = this->fileName();
  auto isType = this->checkAliasFound(declared, implemented, file);
  if (isType.isErr()) {
    this->state.diagnostics.error(declared->loc(), isType.error().message);
    return;
  }
  auto counted = this->checkAliasParams(declared, implemented, file);
  if (counted.isErr()) {
    this->state.diagnostics.error(declared->loc(), counted.error().message);
    return;
  }
  if (!implemented->typeParams.empty()) {
    return;
  }
  this->state.walk.tasks.pushTask(
      FinishDeclaredAlias{.declared = declared, .implemented = implemented});
  this->state.walk.tasks.pushTask(ResolveAlias{.alias = declared});
  this->state.walk.tasks.pushTask(ResolveAlias{.alias = implemented});
}

void DeclarationFilesRegistry::finishDeclaredAlias(const FinishDeclaredAlias &task) {
  auto same = this->checkAliasTypes(task.declared, task.implemented);
  if (same.isErr()) {
    this->state.diagnostics.error(task.declared->loc(), same.error().message);
  }
}

DeclarationFilesRegistry::ResultAliasTypesAreSame
DeclarationFilesRegistry::checkAliasTypes(const TypeAliasDecl *declared,
                                          const TypeAliasDecl *implemented) const {
  const auto *there = implemented->resolved;
  const auto *here = declared->resolved;
  if (this->typesAgree(here, there)) {
    return ok(implemented);
  }
  auto file = this->fileName();
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

  return err(DeclarationFilesRegistryCheckAliasError{
      .code = DeclarationFilesRegistryCheckAliasErrorCode::TYPES_DIFFER,
      .message = message,
  });
}

void DeclarationFilesRegistry::startDeclaredFunction(const StartDeclaredFunction &task) {
  FunctionDecl *declared = task.declared;
  this->declaredNames.insert(declared->name);
  auto exported = this->checkExportFound(declared->name);
  if (exported.isErr()) {
    this->state.diagnostics.error(declared->loc(), exported.error().message);
    return;
  }
  auto entry = this->state.walk.scopes.findName(exported.value()->local);
  auto found = this->checkFunctionFound(entry, declared->name);
  if (found.isErr()) {
    this->state.diagnostics.error(declared->loc(), found.error().message);
    return;
  }
  const FunctionDecl *functionDecl = found.value();
  auto what = ErrorTextDeclaration::quoted(declared->name);
  bool isGeneric = !declared->typeParams.empty() || !functionDecl->typeParams.empty();
  if (isGeneric) {
    return this->walkTypeParamCount(declared->typeParams.size(), functionDecl->typeParams.size(),
                                    what, declared->loc());
  }
  auto match = DeclaredSignatureMatch{
      .declared = &declared->params,
      .implemented = functionDecl,
      .returned = entry.func(),
      .returnType = &declared->returnType,
      .loc = declared->loc(),
      .what = what,
      .isConstructor = false,
  };
  this->pushSignature(match);
}

void DeclarationFilesRegistry::pushSignature(const DeclaredSignatureMatch &match) {
  auto counted =
      this->checkParamCount(match.declared->size(), match.implemented->params.size(), match.what);
  if (counted.isErr()) {
    this->state.diagnostics.error(match.loc, counted.error().message);
    return;
  }
  size_t first = 0;
  this->pushParamOrReturn(match, first);
}

void DeclarationFilesRegistry::pushParamOrReturn(const DeclaredSignatureMatch &match,
                                                 size_t index) {
  if (index < match.declared->size()) {
    return this->state.walk.tasks.pushTask(StartDeclaredParam{.match = match, .index = index});
  }
  if (!match.isConstructor) {
    this->pushReturn(match);
  }
}

void DeclarationFilesRegistry::startDeclaredParam(const StartDeclaredParam &task) {
  Param &mine = (*task.match.declared)[task.index];
  const Param &theirs = task.match.implemented->params[task.index];
  auto param = ErrorTextDeclaration::paramOf(mine.name, task.match.what);
  auto shaped = this->checkParamShape(mine, theirs, param);
  if (shaped.isErr()) {
    this->state.diagnostics.error(mine.loc, shaped.error().message);
    return;
  }
  bool isVoidAllowed = false;
  this->state.walk.tasks.pushTask(FinishDeclaredParam{.match = task.match, .index = task.index});
  this->pushTypeRef(mine.type, isVoidAllowed);
}

void DeclarationFilesRegistry::finishDeclaredParam(const FinishDeclaredParam &task) {
  const Param &mine = (*task.match.declared)[task.index];
  const Param &theirs = task.match.implemented->params[task.index];
  auto param = ErrorTextDeclaration::paramOf(mine.name, task.match.what);
  const auto *here = this->writtenType(mine.type);
  auto typed = ErrorTextDeclaration::typeOf(param);
  auto stored = this->checkStoredType(here, theirs.declaredType, typed);
  if (stored.isErr()) {
    this->state.diagnostics.error(mine.loc, stored.error().message);
    return;
  }
  size_t next = task.index + 1;
  this->pushParamOrReturn(task.match, next);
}

void DeclarationFilesRegistry::pushReturn(const DeclaredSignatureMatch &match) {
  auto written = this->checkReturnWritten(*match.returnType, match.what);
  if (written.isErr()) {
    this->state.diagnostics.error(match.loc, written.error().message);
    return;
  }
  bool isVoidAllowed = true;
  this->state.walk.tasks.pushTask(FinishDeclaredReturn{.match = match});
  this->state.walk.tasks.pushTask(ReturnTypeOf{.function = match.returned});
  this->pushTypeRef(*match.returnType, isVoidAllowed);
}

void DeclarationFilesRegistry::finishDeclaredReturn(const FinishDeclaredReturn &task) {
  const auto *here = this->writtenType(*task.match.returnType);
  const Type *there = this->state.walk.tasks.takeAnswer();
  auto matches = this->checkSameReturn(here, there, task.match.what);
  if (matches.isErr()) {
    this->state.diagnostics.error(task.match.loc, matches.error().message);
  }
}

void DeclarationFilesRegistry::pushTypeRef(std::optional<TypeRef> &ref, bool isVoidAllowed) {
  if (!ref.has_value()) {
    return;
  }
  this->state.walk.tasks.pushTask(ResolveTypeRef{.ref = &*ref, .isVoidAllowed = isVoidAllowed});
}

const Type *DeclarationFilesRegistry::writtenType(const std::optional<TypeRef> &ref) const {
  if (!ref.has_value()) {
    return nullptr;
  }

  return ref->resolved;
}

void DeclarationFilesRegistry::startDeclaredClass(const StartDeclaredClass &task) {
  DeclaredClass &declared = *task.declared;
  this->declaredNames.insert(declared.name);
  auto exported = this->checkExportFound(declared.name);
  if (exported.isErr()) {
    this->state.diagnostics.error(declared.loc, exported.error().message);
    return;
  }
  auto found = this->checkClassFound(declared, *exported.value());
  if (found.isErr()) {
    this->state.diagnostics.error(declared.loc, found.error().message);
    return;
  }
  ClassDecl *classDecl = found.value();
  auto name = ErrorTextDeclaration::quoted(declared.name);
  auto matched =
      this->checkTypeParamCount(declared.typeParams.size(), classDecl->typeParams.size(), name);
  if (matched.isErr()) {
    this->state.diagnostics.error(declared.loc, matched.error().message);
    return;
  }
  if (!classDecl->typeParams.empty()) {
    return;
  }
  this->state.walk.tasks.pushTask(
      FinishDeclaredClass{.declared = &declared, .classDecl = classDecl});
  for (DeclaredMember &member : llvm::reverse(declared.members)) {
    this->state.walk.tasks.pushTask(StartDeclaredMember{.member = &member, .classDecl = classDecl});
  }
  this->state.walk.tasks.pushTask(CompleteClass{.classDecl = classDecl});
}

void DeclarationFilesRegistry::finishDeclaredClass(const FinishDeclaredClass &task) {
  auto seen = llvm::StringSet<>();
  for (const DeclaredMember &member : task.declared->members) {
    auto key = this->memberKey(member.name, member.isStatic);
    seen.insert(key);
  }
  this->walkListed(task.classDecl, seen);
}

void DeclarationFilesRegistry::startDeclaredMember(const StartDeclaredMember &task) {
  DeclaredMember &member = *task.member;
  const ClassDecl *classDecl = task.classDecl;
  auto what = ErrorTextDeclaration::memberOf(member.name, classDecl->name);
  auto written = this->checkAccessWritten(member, what);
  if (written.isErr()) {
    this->state.diagnostics.error(member.loc, written.error().message);
  }
  if (member.form.is<DeclaredConstructor>()) {
    return this->startDeclaredConstructor(member, classDecl);
  }
  const auto *field = this->classField(classDecl, member);
  const auto *method = this->classMethod(classDecl, member);
  auto found = this->checkMemberFound(member, classDecl, field, method);
  if (found.isErr()) {
    this->state.diagnostics.error(member.loc, found.error().message);
    return;
  }
  Access access = field != nullptr ? field->access : method->access();
  auto visible = this->checkMemberVisible(access, what);
  if (visible.isErr()) {
    this->state.diagnostics.error(member.loc, visible.error().message);
    return;
  }
  auto matches = this->checkAccessAgrees(member, access, what);
  if (matches.isErr()) {
    this->state.diagnostics.error(member.loc, matches.error().message);
  }
  if (field != nullptr) {
    return this->startDeclaredField(member, *field, classDecl);
  }

  return this->startDeclaredMethod(member, method, classDecl);
}

void DeclarationFilesRegistry::startDeclaredConstructor(DeclaredMember &member,
                                                        const ClassDecl *classDecl) {
  auto exists = this->checkCallableConstructor(classDecl);
  if (exists.isErr()) {
    this->state.diagnostics.error(member.loc, exists.error().message);
    return;
  }
  const FunctionDecl *constructor = exists.value();
  auto kept = this->checkConstructorAccess(member, constructor, classDecl);
  if (kept.isErr()) {
    this->state.diagnostics.error(member.loc, kept.error().message);
  }

  return this->startDeclaredMethod(member, constructor, classDecl);
}

void DeclarationFilesRegistry::startDeclaredField(DeclaredMember &member, const ClassField &field,
                                                  const ClassDecl *classDecl) {
  auto what = ErrorTextDeclaration::memberOf(member.name, classDecl->name);
  auto form = this->checkDeclaredField(member, what);
  if (form.isErr()) {
    this->state.diagnostics.error(member.loc, form.error().message);
    return;
  }
  const DeclaredField *declared = form.value();
  auto readonlyKept = this->checkReadonlyAgrees(*declared, field, what);
  if (readonlyKept.isErr()) {
    this->state.diagnostics.error(member.loc, readonlyKept.error().message);
  }
  auto optionalKept = this->checkOptionalAgrees(*declared, field, what);
  if (optionalKept.isErr()) {
    this->state.diagnostics.error(member.loc, optionalKept.error().message);
  }
  bool isVoidAllowed = false;
  this->state.walk.tasks.pushTask(
      FinishDeclaredField{.member = &member, .field = &field, .classDecl = classDecl});
  this->pushTypeRef(member.type, isVoidAllowed);
}

void DeclarationFilesRegistry::finishDeclaredField(const FinishDeclaredField &task) {
  auto what = ErrorTextDeclaration::memberOf(task.member->name, task.classDecl->name);
  const auto *here = this->writtenType(task.member->type);
  auto typed = ErrorTextDeclaration::typeOf(what);
  auto stored = this->checkStoredType(here, task.field->resolved, typed);
  if (stored.isErr()) {
    this->state.diagnostics.error(task.member->loc, stored.error().message);
  }
}

void DeclarationFilesRegistry::startDeclaredMethod(DeclaredMember &member,
                                                   const FunctionDecl *functionDecl,
                                                   const ClassDecl *classDecl) {
  auto what = ErrorTextDeclaration::memberOf(member.name, classDecl->name);
  auto signature = this->checkMethodSignature(member, what);
  if (signature.isErr()) {
    this->state.diagnostics.error(member.loc, signature.error().message);
    return;
  }
  DeclaredSignature *declared = signature.value();
  bool isGeneric = !declared->typeParams.empty() || !functionDecl->typeParams.empty();
  if (isGeneric) {
    return this->walkTypeParamCount(declared->typeParams.size(), functionDecl->typeParams.size(),
                                    what, member.loc);
  }
  auto match = DeclaredSignatureMatch{
      .declared = &declared->params,
      .implemented = functionDecl,
      .returned = functionDecl->sym,
      .returnType = &member.type,
      .loc = member.loc,
      .what = what,
      .isConstructor = member.form.is<DeclaredConstructor>(),
  };
  this->pushSignature(match);
}

}
