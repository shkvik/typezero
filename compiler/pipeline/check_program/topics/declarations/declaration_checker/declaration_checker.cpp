#include "compiler/pipeline/check_program/topics/declarations/declaration_checker/declaration_checker.h"
#include "compiler/pipeline/check_program/check/top_level_checker/top_level_checker.h"
#include "compiler/pipeline/check_program/declare/class_declarer/class_declarer.h"
#include "compiler/pipeline/check_program/declare/module_declarer/module_declarer.h"
#include "compiler/pipeline/check_program/declare/type_declarer/type_declarer.h"
#include "compiler/pipeline/check_program/infer/return_inferrer/return_inferrer.h"
#include "compiler/pipeline/check_program/types/type_resolver/type_resolver.h"
#include "llvm/Support/Path.h"

using llvm::dyn_cast;
using llvm::dyn_cast_or_null;

namespace tz {

const char *DeclarationChecker::accessWord(Access access) {
  return access == Access::Public      ? "public"
         : access == Access::Protected ? "protected"
                                       : "private";
}

bool DeclarationChecker::typesAgree(const Type *here, const Type *there) {
  return !here || !there || here == there || here->isError() || there->isError();
}

bool DeclarationChecker::typesAgreeWidened(const Type *here, const Type *there) {
  return DeclarationChecker::typesAgree(here, there) || Type::optional(here) == there;
}

std::string DeclarationChecker::memberKey(llvm::StringRef name, bool isStatic) {
  return (isStatic ? "static " : "") + name.str();
}

const ClassField *DeclarationChecker::classField(const ClassDecl *cls,
                                                 const DeclaredMember &member) {
  const ClassField *field = nullptr;
  for (const ClassField &candidate : cls->fields) {
    if (candidate.name == member.name && candidate.isStatic() == member.isStatic &&
        !candidate.method()) {
      field = &candidate;
    }
  }

  return field;
}

const FunctionDecl *DeclarationChecker::classMethod(const ClassDecl *cls,
                                                    const DeclaredMember &member) {
  const FunctionDecl *method = nullptr;
  for (const FunctionDecl *candidate : cls->methods) {
    if (candidate->name == member.name && candidate->isStatic() == member.isStatic) {
      method = candidate;
    }
  }

  return method;
}

DeclarationChecker::DeclarationChecker(CheckState &owner)
    : state(owner) {}

std::string DeclarationChecker::fileName() const {
  return llvm::sys::path::filename(this->checkedModule->path).str();
}

const Type *DeclarationChecker::resolve(const std::optional<TypeRef> &ref, bool allowVoid) {
  if (!ref) {
    return nullptr;
  }

  return TypeResolver(this->state).resolveType(*ref, allowVoid);
}

DeclarationChecker::ResultModuleExportsName
DeclarationChecker::exportFound(llvm::StringRef name) const {
  auto found = this->exportedNames.find(name);
  if (found == this->exportedNames.end()) {
    std::string message = ErrorTextExports::unexported(name, this->fileName());

    return err(DeclarationCheckerExportFoundError{
        .code = DeclarationCheckerExportFoundErrorCode::NOT_EXPORTED,
        .message = message,
    });
  }

  return ok(&found->second);
}

DeclarationChecker::ResultExportWasDeclared
DeclarationChecker::exportDeclared(llvm::StringRef name, const llvm::StringSet<> &declared,
                                   llvm::StringRef declarations) {
  if (!declared.contains(name)) {
    std::string message = ErrorTextExports::undeclared(name, declarations);

    return err(DeclarationCheckerExportDeclaredError{
        .code = DeclarationCheckerExportDeclaredErrorCode::NOT_DECLARED,
        .message = message,
    });
  }

  return ok(name);
}

void DeclarationChecker::check(Module *module) {
  this->checkedModule = module;
  Module *outer = this->state.getModuleStates().getCurrentModule();
  ModuleDeclarer(this->state).useModule(module);
  this->exportedNames.clear();
  for (const ExportDecl &decl : module->file.exports) {
    const LocalExport *local = decl.form.as<LocalExport>();
    if (!local) {
      continue;
    }
    for (const ModuleName &name : local->names) {
      this->exportedNames[name.as.empty() ? name.name : name.as] = {name.name, name.loc};
    }
  }
  llvm::StringSet<> declared;
  const DeclarationFile &file = *module->declaration;
  for (Stmt *item : file.items) {
    if (auto *alias = dyn_cast<TypeAliasDecl>(item)) {
      declared.insert(alias->name);
      ResultModuleExportsName exported = this->exportFound(alias->name);
      if (exported.isErr()) {
        this->state.getDiagnostics().error(alias->loc(), exported.error().message);
        continue;
      }
      ResultAliasTypesAreSame same = this->checkAlias(alias, *exported.value());
      if (same.isErr()) {
        this->state.getDiagnostics().error(alias->loc(), same.error().message);
      }
    } else if (auto *fn = dyn_cast<FunctionDecl>(item)) {
      declared.insert(fn->name);
      ResultModuleExportsName exported = this->exportFound(fn->name);
      if (exported.isErr()) {
        this->state.getDiagnostics().error(fn->loc(), exported.error().message);
        continue;
      }
      this->checkFunction(fn, *exported.value());
    }
  }
  for (const DeclaredClass &cls : file.classes) {
    declared.insert(cls.name);
    ResultModuleExportsName exported = this->exportFound(cls.name);
    if (exported.isErr()) {
      this->state.getDiagnostics().error(cls.loc, exported.error().message);
      continue;
    }
    this->checkClass(cls, *exported.value());
  }
  std::string declarations = llvm::sys::path::filename(file.path).str();
  for (const auto &entry : this->exportedNames) {
    ResultExportWasDeclared listed =
        DeclarationChecker::exportDeclared(entry.getKey(), declared, declarations);
    if (listed.isErr()) {
      this->state.getDiagnostics().error(entry.second.loc, listed.error().message);
    }
  }
  if (outer) {
    ModuleDeclarer(this->state).useModule(outer);
  }
}

DeclarationChecker::ResultAliasTypesAreSame
DeclarationChecker::checkAlias(TypeAliasDecl *declared, const Exported &exported) const {
  auto *implemented =
      dyn_cast_or_null<TypeAliasDecl>(TypeDeclarer(this->state).lookupType(exported.local));
  std::string file = this->fileName();
  if (!implemented) {
    std::string message = ErrorTextDeclaration::notType(declared->name, file);

    return err(DeclarationCheckerCheckAliasError{
        .code = DeclarationCheckerCheckAliasErrorCode::NOT_A_TYPE,
        .message = message,
    });
  }
  size_t count = declared->typeParams.size();
  size_t implementedCount = implemented->typeParams.size();
  if (count != implementedCount) {
    std::string message =
        ErrorTextDeclaration::typeParams(declared->name, count, implementedCount, file);

    return err(DeclarationCheckerCheckAliasError{
        .code = DeclarationCheckerCheckAliasErrorCode::TYPE_PARAMS_DIFFER,
        .message = message,
    });
  }
  if (!implemented->typeParams.empty()) {
    return ok(implemented);
  }
  const Type *there = TypeDeclarer(this->state).resolveAlias(implemented);
  const Type *here = TypeDeclarer(this->state).resolveAlias(declared);
  if (DeclarationChecker::typesAgree(here, there)) {
    return ok(implemented);
  }
  std::string what = ErrorTextDeclaration::quoted(declared->name);
  if (here->isObject() && there->isObject() && !here->isClass() && !there->isClass()) {
    for (const Field &mine : here->fields()) {
      const Field *theirs = there->field(mine.name);
      if (!theirs) {
        std::string message = ErrorTextDeclaration::extraField(what, mine.name, file);

        return err(DeclarationCheckerCheckAliasError{
            .code = DeclarationCheckerCheckAliasErrorCode::FIELD_ONLY_HERE,
            .message = message,
        });
      }
      std::string field = ErrorTextDeclaration::fieldOf(mine.name, what);
      if (mine.type != theirs->type) {
        std::string message =
            ErrorTextDeclaration::mismatch(field, mine.type->name(), theirs->type->name(), file);

        return err(DeclarationCheckerCheckAliasError{
            .code = DeclarationCheckerCheckAliasErrorCode::FIELD_TYPE_DIFFERS,
            .message = message,
        });
      }
      if (mine.readonly != theirs->readonly) {
        std::string message = ErrorTextDeclaration::readonly(field, theirs->readonly, file);

        return err(DeclarationCheckerCheckAliasError{
            .code = DeclarationCheckerCheckAliasErrorCode::READONLY_DIFFERS,
            .message = message,
        });
      }
    }
    for (const Field &theirs : there->fields()) {
      if (!here->field(theirs.name)) {
        std::string message = ErrorTextDeclaration::absentHere(what, theirs.name, file);

        return err(DeclarationCheckerCheckAliasError{
            .code = DeclarationCheckerCheckAliasErrorCode::FIELD_ONLY_THERE,
            .message = message,
        });
      }
    }
  }
  std::string message = ErrorTextDeclaration::mismatch(what, here->name(), there->name(), file);

  return err(DeclarationCheckerCheckAliasError{
      .code = DeclarationCheckerCheckAliasErrorCode::TYPES_DIFFER,
      .message = message,
  });
}

DeclarationChecker::ResultParamCountMatches
DeclarationChecker::paramCount(size_t here, size_t there, const std::string &what) const {
  if (here != there) {
    std::string message = ErrorTextDeclaration::paramCount(what, here, there, this->fileName());

    return err(DeclarationCheckerParamCountError{
        .code = DeclarationCheckerParamCountErrorCode::COUNTS_DIFFER,
        .message = message,
    });
  }

  return ok(here);
}

DeclarationChecker::ResultParamShapeMatches
DeclarationChecker::paramShape(const Param &mine, const Param &theirs,
                               const std::string &param) const {
  bool optionalThere = theirs.optional() || theirs.defaultValue();
  if (mine.optional() != optionalThere || mine.rest() != theirs.rest()) {
    std::string message = ErrorTextDeclaration::paramKind(param, mine.optional(), this->fileName());

    return err(DeclarationCheckerParamShapeError{
        .code = DeclarationCheckerParamShapeErrorCode::SHAPE_DIFFERS,
        .message = message,
    });
  }

  return ok(&mine);
}

DeclarationChecker::ResultStoredTypeMatches
DeclarationChecker::storedType(const Type *here, const Type *there, const std::string &what) const {
  if (!DeclarationChecker::typesAgreeWidened(here, there)) {
    std::string message =
        ErrorTextDeclaration::mismatch(what, here->name(), there->name(), this->fileName());

    return err(DeclarationCheckerStoredTypeError{
        .code = DeclarationCheckerStoredTypeErrorCode::TYPES_DIFFER,
        .message = message,
    });
  }

  return ok(there);
}

bool DeclarationChecker::sameParams(const std::vector<Param> &declared, const FunctionDecl *fn,
                                    llvm::SMLoc loc, const std::string &what) {
  ResultParamCountMatches counted = this->paramCount(declared.size(), fn->params.size(), what);
  if (counted.isErr()) {
    this->state.getDiagnostics().error(loc, counted.error().message);
    return false;
  }
  for (size_t index = 0; index < declared.size(); ++index) {
    const Param &mine = declared[index];
    const Param &theirs = fn->params[index];
    std::string param = ErrorTextDeclaration::paramOf(mine.name, what);
    ResultParamShapeMatches shaped = this->paramShape(mine, theirs, param);
    if (shaped.isErr()) {
      this->state.getDiagnostics().error(mine.loc, shaped.error().message);
      return false;
    }
    const Type *here = this->resolve(mine.type, false);
    std::string typed = ErrorTextDeclaration::typeOf(param);
    ResultStoredTypeMatches stored = this->storedType(here, theirs.declaredType, typed);
    if (stored.isErr()) {
      this->state.getDiagnostics().error(mine.loc, stored.error().message);
      return false;
    }
  }

  return true;
}

DeclarationChecker::ResultModuleHasFunction
DeclarationChecker::functionFound(const ScopeEntry &entry, llvm::StringRef name) const {
  if (!entry.func() || !entry.func()->decl) {
    std::string message = ErrorTextDeclaration::noFunction(name, this->fileName());

    return err(DeclarationCheckerFunctionFoundError{
        .code = DeclarationCheckerFunctionFoundErrorCode::NOT_A_FUNCTION,
        .message = message,
    });
  }

  return ok(entry.func()->decl);
}

DeclarationChecker::ResultTypeParamsMatched
DeclarationChecker::typeParamCount(size_t here, size_t there, const std::string &what) const {
  if (here != there) {
    std::string message = ErrorTextDeclaration::otherCount(what, this->fileName());

    return err(DeclarationCheckerTypeParamCountError{
        .code = DeclarationCheckerTypeParamCountErrorCode::COUNTS_DIFFER,
        .message = message,
    });
  }

  return ok(here);
}

DeclarationChecker::ResultReturnTypeWritten
DeclarationChecker::returnWritten(const std::optional<TypeRef> &ref, const std::string &what) {
  if (!ref) {
    std::string message = ErrorTextDeclaration::returnType(what);

    return err(DeclarationCheckerReturnWrittenError{
        .code = DeclarationCheckerReturnWrittenErrorCode::NOT_WRITTEN,
        .message = message,
    });
  }

  return ok(&*ref);
}

DeclarationChecker::ResultReturnTypeMatches
DeclarationChecker::sameReturn(const Type *here, const Type *there, const std::string &what) const {
  if (!DeclarationChecker::typesAgree(here, there)) {
    std::string gives = ErrorTextDeclaration::gives(what);
    std::string message =
        ErrorTextDeclaration::mismatch(gives, here->name(), there->name(), this->fileName());

    return err(DeclarationCheckerSameReturnError{
        .code = DeclarationCheckerSameReturnErrorCode::TYPES_DIFFER,
        .message = message,
    });
  }

  return ok(there);
}

void DeclarationChecker::checkFunction(FunctionDecl *declared, const Exported &exported) {
  ScopeEntry entry = TopLevelChecker(this->state).lookupValue(exported.local);
  ResultModuleHasFunction found = this->functionFound(entry, declared->name);
  if (found.isErr()) {
    this->state.getDiagnostics().error(declared->loc(), found.error().message);
    return;
  }
  const FunctionDecl *fn = found.value();
  std::string what = ErrorTextDeclaration::quoted(declared->name);
  if (!declared->typeParams.empty() || !fn->typeParams.empty()) {
    ResultTypeParamsMatched matched =
        this->typeParamCount(declared->typeParams.size(), fn->typeParams.size(), what);
    if (matched.isErr()) {
      this->state.getDiagnostics().error(declared->loc(), matched.error().message);
    }
    return;
  }
  ReturnInferrer(this->state).resolveParams(entry.func());
  if (!this->sameParams(declared->params, fn, declared->loc(), what)) {
    return;
  }
  ResultReturnTypeWritten written = DeclarationChecker::returnWritten(declared->returnType, what);
  if (written.isErr()) {
    this->state.getDiagnostics().error(declared->loc(), written.error().message);
    return;
  }
  const Type *here = this->resolve(declared->returnType, true);
  const Type *there = ReturnInferrer(this->state).returnType(entry.func());
  ResultReturnTypeMatches matches = this->sameReturn(here, there, what);
  if (matches.isErr()) {
    this->state.getDiagnostics().error(declared->loc(), matches.error().message);
  }
}

bool DeclarationChecker::mentionsHidden(const FunctionDecl *fn) const {
  for (const Param &param : fn->params) {
    const Type *type = param.declaredType ? param.declaredType->nonOptional() : nullptr;
    const ClassDecl *cls = type ? ClassDeclarer::classOf(type) : nullptr;
    if (!cls) {
      continue;
    }
    bool shown = false;
    for (const auto &entry : this->exportedNames) {
      if (entry.second.local == cls->name) {
        shown = true;
      }
    }
    if (!shown && this->state.getModuleStates().findDeclModule(cls) == this->checkedModule) {
      return true;
    }
  }

  return false;
}

DeclarationChecker::ResultDeclaredFieldForm
DeclarationChecker::declaredField(const DeclaredMember &member, const std::string &what) const {
  const DeclaredField *declared = member.form.as<DeclaredField>();
  if (!declared) {
    std::string message = ErrorTextDeclaredClass::isField(what, this->fileName());

    return err(DeclarationCheckerDeclaredFieldError{
        .code = DeclarationCheckerDeclaredFieldErrorCode::IS_A_FIELD,
        .message = message,
    });
  }

  return ok(declared);
}

DeclarationChecker::ResultReadonlyFlagsSame
DeclarationChecker::readonlyAgrees(const DeclaredField &declared, const ClassField &field,
                                   const std::string &what) const {
  if (declared.readonly != field.readonly) {
    std::string message = ErrorTextDeclaration::readonly(what, field.readonly, this->fileName());

    return err(DeclarationCheckerReadonlyAgreesError{
        .code = DeclarationCheckerReadonlyAgreesErrorCode::READONLY_DIFFERS,
        .message = message,
    });
  }

  return ok(&field);
}

DeclarationChecker::ResultOptionalFlagsSame
DeclarationChecker::optionalAgrees(const DeclaredField &declared, const ClassField &field,
                                   const std::string &what) const {
  if (declared.optional != field.optional()) {
    std::string message = ErrorTextDeclaration::optional(what, field.optional(), this->fileName());

    return err(DeclarationCheckerOptionalAgreesError{
        .code = DeclarationCheckerOptionalAgreesErrorCode::OPTIONAL_DIFFERS,
        .message = message,
    });
  }

  return ok(&field);
}

void DeclarationChecker::checkField(const DeclaredMember &member, const ClassField &field,
                                    const ClassDecl *cls) {
  std::string what = ErrorTextDeclaration::memberOf(member.name, cls->name);
  ResultDeclaredFieldForm form = this->declaredField(member, what);
  if (form.isErr()) {
    this->state.getDiagnostics().error(member.loc, form.error().message);
    return;
  }
  const DeclaredField *declared = form.value();
  ResultReadonlyFlagsSame readonlyKept = this->readonlyAgrees(*declared, field, what);
  if (readonlyKept.isErr()) {
    this->state.getDiagnostics().error(member.loc, readonlyKept.error().message);
  }
  ResultOptionalFlagsSame optionalKept = this->optionalAgrees(*declared, field, what);
  if (optionalKept.isErr()) {
    this->state.getDiagnostics().error(member.loc, optionalKept.error().message);
  }
  const Type *here = this->resolve(member.type, false);
  std::string typed = ErrorTextDeclaration::typeOf(what);
  ResultStoredTypeMatches stored = this->storedType(here, field.resolved, typed);
  if (stored.isErr()) {
    this->state.getDiagnostics().error(member.loc, stored.error().message);
  }
}

DeclarationChecker::ResultDeclaredSignature
DeclarationChecker::methodSignature(const DeclaredMember &member, const std::string &what) const {
  const DeclaredSignature *signature = nullptr;
  switch (member.form.kind()) {
    case DeclaredMemberKind::DECLARED_FIELD:
      break;
    case DeclaredMemberKind::DECLARED_METHOD:
      signature = &member.form.as<DeclaredMethod>()->signature;
      break;
    case DeclaredMemberKind::DECLARED_CONSTRUCTOR:
      signature = &member.form.as<DeclaredConstructor>()->signature;
      break;
  }
  if (!signature) {
    std::string message = ErrorTextDeclaredClass::isMethod(what, this->fileName());

    return err(DeclarationCheckerMethodSignatureError{
        .code = DeclarationCheckerMethodSignatureErrorCode::IS_A_METHOD,
        .message = message,
    });
  }

  return ok(signature);
}

void DeclarationChecker::checkMethod(const DeclaredMember &member, const FunctionDecl *fn,
                                     const ClassDecl *cls) {
  std::string what = ErrorTextDeclaration::memberOf(member.name, cls->name);
  ResultDeclaredSignature signature = this->methodSignature(member, what);
  if (signature.isErr()) {
    this->state.getDiagnostics().error(member.loc, signature.error().message);
    return;
  }
  const DeclaredSignature *declared = signature.value();
  if (!declared->typeParams.empty() || !fn->typeParams.empty()) {
    ResultTypeParamsMatched matched =
        this->typeParamCount(declared->typeParams.size(), fn->typeParams.size(), what);
    if (matched.isErr()) {
      this->state.getDiagnostics().error(member.loc, matched.error().message);
    }
    return;
  }
  ReturnInferrer(this->state).resolveParams(fn->sym);
  bool constructor = member.form.is<DeclaredConstructor>();
  if (!this->sameParams(declared->params, fn, member.loc, what) || constructor) {
    return;
  }
  ResultReturnTypeWritten written = DeclarationChecker::returnWritten(member.type, what);
  if (written.isErr()) {
    this->state.getDiagnostics().error(member.loc, written.error().message);
    return;
  }
  const Type *here = this->resolve(member.type, true);
  const Type *there = ReturnInferrer(this->state).returnType(fn->sym);
  ResultReturnTypeMatches matches = this->sameReturn(here, there, what);
  if (matches.isErr()) {
    this->state.getDiagnostics().error(member.loc, matches.error().message);
  }
}

DeclarationChecker::ResultModuleHasTheClass
DeclarationChecker::classFound(const DeclaredClass &declared, const Exported &exported) const {
  auto *cls = dyn_cast_or_null<ClassDecl>(TypeDeclarer(this->state).lookupType(exported.local));
  if (!cls) {
    std::string message = ErrorTextDeclaredClass::notClass(declared.name, this->fileName());

    return err(DeclarationCheckerClassFoundError{
        .code = DeclarationCheckerClassFoundErrorCode::NOT_A_CLASS,
        .message = message,
    });
  }

  return ok(cls);
}

DeclarationChecker::ResultAccessWordWritten
DeclarationChecker::accessWritten(const DeclaredMember &member, const std::string &what) {
  if (!member.accessWritten) {
    std::string message = ErrorTextDeclaredClass::noAccess(what);

    return err(DeclarationCheckerAccessWrittenError{
        .code = DeclarationCheckerAccessWrittenErrorCode::NOT_WRITTEN,
        .message = message,
    });
  }

  return ok(&member);
}

DeclarationChecker::ResultConstructorExists
DeclarationChecker::callableConstructor(const ClassDecl *cls) const {
  const FunctionDecl *callable = nullptr;
  for (const FunctionDecl *constructor : cls->constructors) {
    if (constructor->access() != Access::Private) {
      callable = constructor;
    }
  }
  if (!callable) {
    std::string message = ErrorTextDeclaredClass::noCallable(cls->name, this->fileName());

    return err(DeclarationCheckerCallableConstructorError{
        .code = DeclarationCheckerCallableConstructorErrorCode::NO_CONSTRUCTOR,
        .message = message,
    });
  }

  return ok(callable);
}

DeclarationChecker::ResultConstructorAccess
DeclarationChecker::constructorAccess(const DeclaredMember &member, const FunctionDecl *constructor,
                                      const ClassDecl *cls) const {
  if (member.access != constructor->access()) {
    const char *word = DeclarationChecker::accessWord(constructor->access());
    std::string message = ErrorTextDeclaredClass::newAccess(cls->name, word, this->fileName());

    return err(DeclarationCheckerConstructorAccessError{
        .code = DeclarationCheckerConstructorAccessErrorCode::ACCESS_DIFFERS,
        .message = message,
    });
  }

  return ok(constructor);
}

DeclarationChecker::ResultClassHasTheMember
DeclarationChecker::memberFound(const DeclaredMember &member, const ClassDecl *cls,
                                const ClassField *field, const FunctionDecl *method) const {
  if (!field && !method) {
    std::string message =
        ErrorTextDeclaredClass::noMember(cls->name, member.name, member.isStatic, this->fileName());

    return err(DeclarationCheckerMemberFoundError{
        .code = DeclarationCheckerMemberFoundErrorCode::NO_MEMBER,
        .message = message,
    });
  }

  return ok(&member);
}

DeclarationChecker::ResultPublicOrProtected
DeclarationChecker::memberVisible(Access access, const std::string &what) const {
  if (access == Access::Private) {
    std::string message = ErrorTextDeclaredClass::isPrivate(what, this->fileName());

    return err(DeclarationCheckerMemberVisibleError{
        .code = DeclarationCheckerMemberVisibleErrorCode::PRIVATE_MEMBER,
        .message = message,
    });
  }

  return ok(access);
}

DeclarationChecker::ResultAccessWordMatches
DeclarationChecker::accessAgrees(const DeclaredMember &member, Access access,
                                 const std::string &what) const {
  if (member.accessWritten && member.access != access) {
    const char *word = DeclarationChecker::accessWord(access);
    std::string message = ErrorTextDeclaredClass::accessIs(what, word, this->fileName());

    return err(DeclarationCheckerAccessAgreesError{
        .code = DeclarationCheckerAccessAgreesErrorCode::ACCESS_DIFFERS,
        .message = message,
    });
  }

  return ok(access);
}

DeclarationChecker::ResultMemberWasDeclared
DeclarationChecker::memberListed(const llvm::StringSet<> &seen, llvm::StringRef name, bool isStatic,
                                 const ClassDecl *cls) const {
  std::string key = DeclarationChecker::memberKey(name, isStatic);
  if (!seen.contains(key)) {
    std::string declarations =
        llvm::sys::path::filename(this->checkedModule->declaration->path).str();
    std::string message =
        ErrorTextDeclaredClass::undeclared(name, cls->name, isStatic, declarations);

    return err(DeclarationCheckerMemberListedError{
        .code = DeclarationCheckerMemberListedErrorCode::NOT_DECLARED,
        .message = message,
    });
  }

  return ok(cls);
}

void DeclarationChecker::checkConstructor(const DeclaredMember &member, const ClassDecl *cls) {
  ResultConstructorExists exists = this->callableConstructor(cls);
  if (exists.isErr()) {
    this->state.getDiagnostics().error(member.loc, exists.error().message);
    return;
  }
  const FunctionDecl *constructor = exists.value();
  ResultConstructorAccess kept = this->constructorAccess(member, constructor, cls);
  if (kept.isErr()) {
    this->state.getDiagnostics().error(member.loc, kept.error().message);
  }
  this->checkMethod(member, constructor, cls);
}

void DeclarationChecker::checkMember(const DeclaredMember &member, const ClassDecl *cls) {
  std::string what = ErrorTextDeclaration::memberOf(member.name, cls->name);
  ResultAccessWordWritten written = DeclarationChecker::accessWritten(member, what);
  if (written.isErr()) {
    this->state.getDiagnostics().error(member.loc, written.error().message);
  }
  if (member.form.is<DeclaredConstructor>()) {
    this->checkConstructor(member, cls);
    return;
  }
  const ClassField *field = DeclarationChecker::classField(cls, member);
  const FunctionDecl *method = DeclarationChecker::classMethod(cls, member);
  ResultClassHasTheMember found = this->memberFound(member, cls, field, method);
  if (found.isErr()) {
    this->state.getDiagnostics().error(member.loc, found.error().message);
    return;
  }
  Access access = field ? field->access : method->access();
  ResultPublicOrProtected visible = this->memberVisible(access, what);
  if (visible.isErr()) {
    this->state.getDiagnostics().error(member.loc, visible.error().message);
    return;
  }
  ResultAccessWordMatches matches = this->accessAgrees(member, access, what);
  if (matches.isErr()) {
    this->state.getDiagnostics().error(member.loc, matches.error().message);
  }
  if (field) {
    this->checkField(member, *field, cls);
  } else {
    this->checkMethod(member, method, cls);
  }
}

void DeclarationChecker::checkListed(const ClassDecl *cls, const llvm::StringSet<> &seen) {
  for (const ClassField &field : cls->fields) {
    if (field.method() || field.access == Access::Private) {
      continue;
    }
    ResultMemberWasDeclared listed = this->memberListed(seen, field.name, field.isStatic(), cls);
    if (listed.isErr()) {
      this->state.getDiagnostics().error(field.loc, listed.error().message);
    }
  }
  for (const FunctionDecl *method : cls->methods) {
    if (method->access() == Access::Private) {
      continue;
    }
    ResultMemberWasDeclared listed =
        this->memberListed(seen, method->name, method->isStatic(), cls);
    if (listed.isErr()) {
      this->state.getDiagnostics().error(method->loc(), listed.error().message);
    }
  }
  for (const FunctionDecl *constructor : cls->constructors) {
    if (constructor->access() == Access::Private || this->mentionsHidden(constructor)) {
      continue;
    }
    ResultMemberWasDeclared listed = this->memberListed(seen, "constructor", false, cls);
    if (listed.isErr()) {
      this->state.getDiagnostics().error(constructor->loc(), listed.error().message);
    }
  }
}

void DeclarationChecker::checkClass(const DeclaredClass &declared, const Exported &exported) {
  ResultModuleHasTheClass found = this->classFound(declared, exported);
  if (found.isErr()) {
    this->state.getDiagnostics().error(declared.loc, found.error().message);
    return;
  }
  ClassDecl *cls = found.value();
  std::string name = ErrorTextDeclaration::quoted(declared.name);
  ResultTypeParamsMatched matched =
      this->typeParamCount(declared.typeParams.size(), cls->typeParams.size(), name);
  if (matched.isErr()) {
    this->state.getDiagnostics().error(declared.loc, matched.error().message);
    return;
  }
  if (!cls->typeParams.empty()) {
    return;
  }
  ClassDeclarer(this->state).completeClass(cls);
  llvm::StringSet<> seen;
  for (const DeclaredMember &member : declared.members) {
    std::string key = DeclarationChecker::memberKey(member.name, member.isStatic);
    seen.insert(key);
    this->checkMember(member, cls);
  }
  this->checkListed(cls, seen);
}

}
