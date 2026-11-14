#include "compiler/semantic/declaration/names/declaration_names_registry/declaration_names_registry.h"
#include "compiler/program/ast/ast.h"
#include "compiler/program/ast_walker/ast_walker.h"
#include "compiler/program/library/builtin_names/type_name_table/type_name_table.h"
#include "compiler/program/library/bytes/buffer_export_table/buffer_export_table.h"
#include "compiler/program/library/library_module_names/library_module_names.h"
#include "llvm/ADT/StringSet.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

DeclarationNamesRegistry::DeclarationNamesRegistry(SemanticState &owner)
    : state(owner) {}

void DeclarationNamesRegistry::openScope() {
  this->state.getScopeStack().openScope();
}

void DeclarationNamesRegistry::closeScope() {
  this->state.getScopeStack().closeScope();
}

ScopeEntry DeclarationNamesRegistry::findName(llvm::StringRef name) const {
  return this->state.getScopeStack().findName(name);
}

const NodeName *DeclarationNamesRegistry::findNodeName(llvm::StringRef name) const {
  auto &modules = this->state.getModuleStates();
  auto *current = modules.getCurrentModule();
  if (current == nullptr || !modules.hasModuleState(current)) {
    return nullptr;
  }
  const auto &names = modules.getModuleState(current).nodeNames;
  auto found = names.find(name);
  if (found == names.end()) {
    return nullptr;
  }
  bool isBufferName = BufferExportTable::find(name) == BufferExport::BUFFER;
  bool isGlobalBuffer = found->second.module == NodeModule::Buffer && isBufferName;
  if (isGlobalBuffer) {
    return nullptr;
  }
  auto entry = this->findName(name);
  if (entry.isFound()) {
    return nullptr;
  }

  return &found->second;
}

ClassDecl *DeclarationNamesRegistry::findClassOf(const Type *type) const {
  if (type == nullptr) {
    return nullptr;
  }
  const auto *classDecl = type->classDecl();

  return const_cast<ClassDecl *>(classDecl);
}

bool DeclarationNamesRegistry::hasMethodFields(const ClassDecl *classDecl) const {
  if (classDecl->literal() != nullptr) {
    return true;
  }
  for (const ClassDecl *owner = classDecl; owner != nullptr; owner = owner->parent) {
    if (this->hasOwnMethodField(owner)) {
      return true;
    }
  }

  return false;
}

bool DeclarationNamesRegistry::hasOwnMethodField(const ClassDecl *classDecl) const {
  for (const ClassField &field : classDecl->fields) {
    if (field.method() != nullptr) {
      return true;
    }
  }

  return false;
}

bool DeclarationNamesRegistry::isUnshadowedGlobal(const NameExpr *name) const {
  const auto *nodeName = this->findNodeName(name->name);
  if (nodeName != nullptr) {
    return false;
  }
  auto shadow = this->findName(name->name);

  return shadow.var() == nullptr && shadow.func() == nullptr && shadow.cls() == nullptr;
}

void DeclarationNamesRegistry::rememberNamesInFunctions(SourceFile &program) {
  auto names = llvm::StringSet<>();
  for (Stmt *item : program.items) {
    bool isFunction = isa<FunctionDecl>(item);
    AstWalker::collectNames(item, names, isFunction);
  }
  for (const auto &name : names) {
    this->state.getScopeStack().markUsedInFunction(name.getKey());
  }
  this->state.getScopeStack().markUsedInFunction(LibraryModuleNames::kSubtleName);
}

Stmt *DeclarationNamesRegistry::findTypeDeclaration(llvm::StringRef name) const {
  auto *declared = this->state.getScopeStack().findType(name);
  if (declared != nullptr) {
    return declared;
  }
  auto *alias = this->state.getAliasStore().findBuiltinAlias(name);
  if (alias != nullptr) {
    return alias;
  }
  const auto *nodeName = this->findNodeName(name);
  if (nodeName != nullptr) {
    return this->findModuleExport(*nodeName);
  }
  if (this->state.getTypeContext().isInsideBuiltinModule()) {
    return this->state.getLibraryDeclarations().findModuleAlias(name);
  }

  return nullptr;
}

TypeAliasDecl *DeclarationNamesRegistry::findModuleExport(const NodeName &nodeName) const {
  auto &declarations = this->state.getLibraryDeclarations();
  if (!declarations.isModuleExport(nodeName.module, nodeName.name)) {
    return nullptr;
  }

  return declarations.findModuleAlias(nodeName.name);
}

FuncSymbol *DeclarationNamesRegistry::findGenericFunction(const NameExpr *name) const {
  auto entry = this->findName(name->name);
  FuncSymbol *function = entry.func();
  bool isGenericFunction = function != nullptr && entry.var() == nullptr && function->isGeneric();
  if (!isGenericFunction) {
    return nullptr;
  }

  return function;
}

bool DeclarationNamesRegistry::isGlobalName(const Expr *expr, GlobalName global) const {
  bool isThroughGlobalThis = global == GlobalName::CRYPTO && this->isGlobalThisMember(expr, global);
  if (isThroughGlobalThis) {
    return true;
  }

  return this->isNamingGlobal(expr, global);
}

bool DeclarationNamesRegistry::isGlobalThisMember(const Expr *expr, GlobalName global) const {
  auto name = GlobalNameTable::getName(global);
  const auto *member = dyn_cast<MemberExpr>(expr);
  if (member == nullptr || member->optional || member->name != name) {
    return false;
  }

  return this->isNamingGlobal(member->object, GlobalName::GLOBAL_THIS);
}

bool DeclarationNamesRegistry::isNamingGlobal(const Expr *expr, GlobalName global) const {
  auto name = GlobalNameTable::getName(global);
  const auto *named = dyn_cast<NameExpr>(expr);
  if (named == nullptr || named->name != name) {
    return false;
  }

  return this->isUnshadowedGlobal(named);
}

DeclarationNamesRegistry::ResultEntryDeclared
DeclarationNamesRegistry::declare(llvm::StringRef name, ScopeEntry entry) {
  auto allowed = this->checkNameAllowed(name);
  if (allowed.isErr()) {
    return err(allowed.error());
  }
  auto function = this->checkFunctionOnce(name, entry);
  if (function.isErr()) {
    return err(function.error());
  }
  auto unique = this->checkNotDeclared(name);
  if (unique.isErr()) {
    return err(unique.error());
  }
  this->state.getScopeStack().addToCurrentScope(name, entry);

  return ok(entry);
}

DeclarationNamesRegistry::ResultNameIsAllowed
DeclarationNamesRegistry::checkNameAllowed(llvm::StringRef name) const {
  if (TypeNameTable::isName(name, TypeName::UNDEFINED)) {
    auto message = ErrorTextDeclareName::undefined();

    return err(DeclarationNamesRegistryDeclareError{
        .code = DeclarationNamesRegistryDeclareErrorCode::UNDEFINED_AS_NAME,
        .message = message,
    });
  }

  return ok();
}

DeclarationNamesRegistry::ResultNameIsAllowed
DeclarationNamesRegistry::checkFunctionOnce(llvm::StringRef name, const ScopeEntry &entry) const {
  auto previous = this->state.getScopeStack().findInCurrentScope(name);
  bool isSecondFunction =
      previous.has_value() && entry.func() != nullptr && previous->func() != nullptr;
  if (isSecondFunction) {
    auto message = ErrorTextDeclareName::function(name);

    return err(DeclarationNamesRegistryDeclareError{
        .code = DeclarationNamesRegistryDeclareErrorCode::DUPLICATE_FUNCTION,
        .message = message,
    });
  }

  return ok();
}

DeclarationNamesRegistry::ResultNameIsAllowed
DeclarationNamesRegistry::checkNotDeclared(llvm::StringRef name) const {
  auto previous = this->state.getScopeStack().findInCurrentScope(name);
  if (previous.has_value()) {
    auto message = ErrorTextDeclareName::duplicate(name);

    return err(DeclarationNamesRegistryDeclareError{
        .code = DeclarationNamesRegistryDeclareErrorCode::DUPLICATE_DECLARATION,
        .message = message,
    });
  }

  return ok();
}

bool DeclarationNamesRegistry::isGlobalMember(const MemberExpr *member, GlobalMember which) const {
  auto owner = GlobalMemberTable::getOwner(which);
  auto found = GlobalMemberTable::find(owner, member->name);
  if (found != which) {
    return false;
  }

  return this->isGlobalName(member->object, owner);
}

VarSymbol *DeclarationNamesRegistry::declareLet(llvm::StringRef name, llvm::SMLoc loc,
                                                const Type *type) {
  bool isConst = false;

  return this->declareVar(name, loc, type, isConst);
}

VarSymbol *DeclarationNamesRegistry::declareConst(llvm::StringRef name, llvm::SMLoc loc,
                                                  const Type *type) {
  bool isConst = true;

  return this->declareVar(name, loc, type, isConst);
}

DeclarationNamesRegistry::ResultNameIsVisible
DeclarationNamesRegistry::checkVisible(NameExpr *name, const VarSymbol *variable) const {
  if (this->state.getBlockPosition().isOtherCaseVar(variable)) {
    auto message = ErrorTextCheckVisible::otherCase(name->name);

    return err(DeclarationNamesRegistryCheckVisibleError{
        .code = DeclarationNamesRegistryCheckVisibleErrorCode::DECLARED_IN_OTHER_CASE,
        .message = message,
    });
  }

  return ok(variable);
}

const ClassField *DeclarationNamesRegistry::findField(const ClassDecl *classDecl,
                                                      llvm::StringRef name) const {
  for (const ClassDecl *current = classDecl; current != nullptr; current = current->parent) {
    const auto *field = current->field(name);
    if (field != nullptr) {
      return field;
    }
  }

  return nullptr;
}

std::vector<FunctionDecl *>
DeclarationNamesRegistry::collectMemberFunctions(const ClassDecl *classDecl) const {
  std::vector<FunctionDecl *> methods = classDecl->methods;
  methods.insert(methods.end(), classDecl->constructors.begin(), classDecl->constructors.end());
  if (classDecl->init != nullptr) {
    methods.push_back(classDecl->init);
  }
  for (const ClassField &field : classDecl->fields) {
    if (field.method() != nullptr) {
      methods.push_back(field.method());
    }
  }

  return methods;
}

const ClassDecl *DeclarationNamesRegistry::findFieldOwner(const ClassDecl *classDecl,
                                                          llvm::StringRef name) const {
  for (const ClassDecl *current = classDecl; current != nullptr; current = current->parent) {
    const auto *field = current->field(name);
    if (field != nullptr) {
      return current;
    }
  }

  return nullptr;
}

VarSymbol *DeclarationNamesRegistry::declareVar(llvm::StringRef name, llvm::SMLoc loc,
                                                const Type *type, bool isConst) {
  auto *variable = this->state.getAst().newVar();
  variable->name = name;
  variable->loc = loc;
  variable->type = type;
  variable->isConst = isConst;
  this->placeVariable(variable);
  auto entry = ScopeEntry{.form = VarEntry{.var = variable}};
  auto added = this->declare(name, entry);
  if (added.isErr()) {
    this->reportDeclareError(loc, name, added.error());
  }

  return variable;
}

void DeclarationNamesRegistry::placeVariable(VarSymbol *variable) {
  if (this->state.getScopeStack().isTopLevel()) {
    bool isUsedInFunction = this->state.getScopeStack().isUsedInFunction(variable->name);
    variable->place = GlobalVar{.usedInFunction = isUsedInFunction};
    return;
  }
  auto sequenceNumber = this->state.getReturnInference().takeSequenceNumber();
  auto *currentDecl = this->state.getFunctionStack().getCurrentDecl();
  variable->place = VarSymbol::localPlace(currentDecl, sequenceNumber);
}

void DeclarationNamesRegistry::reportDeclareError(
    llvm::SMLoc loc, llvm::StringRef name, const DeclarationNamesRegistryDeclareError &error) {
  this->state.getDiagnostics().error(loc, error.message);
  switch (error.code) {
    case DeclarationNamesRegistryDeclareErrorCode::UNDEFINED_AS_NAME:
      return;
    case DeclarationNamesRegistryDeclareErrorCode::DUPLICATE_DECLARATION:
    case DeclarationNamesRegistryDeclareErrorCode::DUPLICATE_FUNCTION:
      return this->notePreviousDeclaration(name);
  }
}

void DeclarationNamesRegistry::notePreviousDeclaration(llvm::StringRef name) {
  auto previous = this->findName(name);
  auto message = ErrorTextDeclareName::previous();
  this->state.getDiagnostics().note(previous.loc(), message);
}

std::vector<FunctionDecl *> DeclarationNamesRegistry::findMethodsNamed(const ClassDecl *classDecl,
                                                                       llvm::StringRef name) const {
  auto found = std::vector<FunctionDecl *>();
  for (const ClassDecl *current = classDecl; current != nullptr; current = current->parent) {
    auto own = this->ownMethodsNamed(current, name);
    this->addNotOverridden(found, own);
  }

  return found;
}

std::vector<FunctionDecl *> DeclarationNamesRegistry::ownMethodsNamed(const ClassDecl *classDecl,
                                                                      llvm::StringRef name) const {
  auto own = std::vector<FunctionDecl *>();
  for (FunctionDecl *method : classDecl->methods) {
    if (method->name == name && !method->isStatic()) {
      own.push_back(method);
    }
  }
  for (const ClassField &field : classDecl->fields) {
    if (field.method() != nullptr && field.name == name) {
      own.push_back(field.method());
    }
  }

  return own;
}

void DeclarationNamesRegistry::addNotOverridden(std::vector<FunctionDecl *> &found,
                                                const std::vector<FunctionDecl *> &own) const {
  for (FunctionDecl *method : own) {
    if (!this->isOverriddenByAny(found, method)) {
      found.push_back(method);
    }
  }
}

bool DeclarationNamesRegistry::isOverriddenByAny(const std::vector<FunctionDecl *> &found,
                                                 const FunctionDecl *method) const {
  for (const FunctionDecl *lower : found) {
    if (this->isOverriding(lower, method)) {
      return true;
    }
  }

  return false;
}

bool DeclarationNamesRegistry::isOverriding(const FunctionDecl *method,
                                            const FunctionDecl *base) const {
  for (const FunctionDecl *current = method; current != nullptr; current = current->overrides()) {
    if (current == base) {
      return true;
    }
  }

  return false;
}

std::vector<FunctionDecl *>
DeclarationNamesRegistry::findStaticMethodsNamed(const ClassDecl *classDecl,
                                                 llvm::StringRef name) const {
  auto found = std::vector<FunctionDecl *>();
  for (const ClassDecl *current = classDecl; current != nullptr; current = current->parent) {
    auto own = this->ownStaticMethodsNamed(current, name);
    this->addNotHidden(found, own);
  }

  return found;
}

std::vector<FunctionDecl *>
DeclarationNamesRegistry::ownStaticMethodsNamed(const ClassDecl *classDecl,
                                                llvm::StringRef name) const {
  auto own = std::vector<FunctionDecl *>();
  for (FunctionDecl *method : classDecl->staticOwner()->methods) {
    if (method->name == name && method->isStatic()) {
      own.push_back(method);
    }
  }

  return own;
}

void DeclarationNamesRegistry::addNotHidden(std::vector<FunctionDecl *> &found,
                                            const std::vector<FunctionDecl *> &own) const {
  for (FunctionDecl *method : own) {
    if (!this->isHiddenByAny(found, method)) {
      found.push_back(method);
    }
  }
}

bool DeclarationNamesRegistry::isHiddenByAny(const std::vector<FunctionDecl *> &found,
                                             const FunctionDecl *method) const {
  for (const FunctionDecl *lower : found) {
    if (this->hasSameSignature(lower->sym, method->sym)) {
      return true;
    }
  }

  return false;
}

bool DeclarationNamesRegistry::hasSameSignature(const FuncSymbol *first,
                                                const FuncSymbol *second) const {
  if (first == nullptr || second == nullptr) {
    return false;
  }

  return first->params == second->params && first->minArgs == second->minArgs &&
         first->hasRest == second->hasRest;
}

}
