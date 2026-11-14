#include "compiler/semantic/declaration/names/declaration_names_registry.h"
#include "compiler/program/ast/ast.h"
#include "compiler/program/ast_walker/ast_walker.h"
#include "compiler/program/library/library_module_names/library_module_names.h"
#include "llvm/ADT/StringSet.h"

using llvm::isa;

namespace tz {

DeclarationNamesRegistry::DeclarationNamesRegistry(SemanticState &owner)
    : state(owner) {}

void DeclarationNamesRegistry::openScope() {
  this->state.walk.scopes.openScope();
}

void DeclarationNamesRegistry::closeScope() {
  this->state.walk.scopes.closeScope();
}

void DeclarationNamesRegistry::rememberNamesInFunctions(SourceFile &program) {
  auto names = llvm::StringSet<>();
  for (Stmt *item : program.items) {
    bool isFunction = isa<FunctionDecl>(item);
    AstWalker::collectNames(item, names, isFunction);
  }
  for (const auto &name : names) {
    this->state.declaration.mentions.markUsedInFunction(name.getKey());
  }
  this->state.declaration.mentions.markUsedInFunction(LibraryModuleNames::kSubtleName);
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
  if (this->state.walk.block.isOtherCaseVar(variable)) {
    auto message = ErrorTextCheckVisible::otherCase(name->name);

    return err(DeclarationNamesRegistryCheckVisibleError{
        .code = DeclarationNamesRegistryCheckVisibleErrorCode::DECLARED_IN_OTHER_CASE,
        .message = message,
    });
  }

  return ok(variable);
}

VarSymbol *DeclarationNamesRegistry::declareVar(llvm::StringRef name, llvm::SMLoc loc,
                                                const Type *type, bool isConst) {
  auto *variable = this->state.ast.newVar();
  variable->name = name;
  variable->loc = loc;
  variable->type = type;
  variable->isConst = isConst;
  this->placeVariable(variable);
  auto entry = ScopeEntry{.form = VarEntry{.var = variable}};
  auto added = this->state.walk.scopes.declare(name, entry);
  if (added.isErr()) {
    this->reportDeclareError(loc, name, added.error());
  }

  return variable;
}

void DeclarationNamesRegistry::placeVariable(VarSymbol *variable) {
  if (this->state.walk.scopes.isTopLevel()) {
    bool isUsedInFunction = this->state.declaration.mentions.isUsedInFunction(variable->name);
    variable->place = GlobalVar{.usedInFunction = isUsedInFunction};
    return;
  }
  auto sequenceNumber = this->state.declaration.order.takeSequenceNumber();
  auto *currentDecl = this->state.walk.functions.getCurrentDecl();
  variable->place = VarSymbol::localPlace(currentDecl, sequenceNumber);
}

void DeclarationNamesRegistry::reportDeclareError(llvm::SMLoc loc, llvm::StringRef name,
                                                  const ScopeStackContextDeclareError &error) {
  this->state.diagnostics.error(loc, error.message);
  switch (error.code) {
    case ScopeStackContextDeclareErrorCode::UNDEFINED_AS_NAME:
      return;
    case ScopeStackContextDeclareErrorCode::DUPLICATE_DECLARATION:
    case ScopeStackContextDeclareErrorCode::DUPLICATE_FUNCTION:
      return this->notePreviousDeclaration(name);
  }
}

void DeclarationNamesRegistry::notePreviousDeclaration(llvm::StringRef name) {
  auto previous = this->state.walk.scopes.findName(name);
  auto message = ErrorTextDeclareName::previous();
  this->state.diagnostics.note(previous.loc(), message);
}

}
