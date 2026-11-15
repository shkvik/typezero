#include "compiler/semantic/library_v2/reflect/library_reflect.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>

namespace tz {

LibraryReflect::LibraryReflect(SemanticState &owner)
    : state(owner) {}

bool LibraryReflect::isOnlyChecked() const {
  auto isCheckOnly = std::bind_front(&LibraryReflect::isCheckOnlyFunction, this);

  return this->state.walk.functions.isInsideTrial() ||
         llvm::any_of(this->state.walk.functions.getFunctions(), isCheckOnly);
}

bool LibraryReflect::isCheckOnlyFunction(const FunctionDecl *functionDecl) const {
  const ClassDecl *owner = functionDecl->cls();

  return (!functionDecl->typeParams.empty() && !functionDecl->isInstance) ||
         (owner != nullptr && (owner->isGeneric() || owner->checkOnly));
}

ClassDecl *LibraryReflect::builtinConstructorNamed(llvm::StringRef name) {
  auto found = BuiltinConstructorTable::find(name);
  if (!found.has_value()) {
    return nullptr;
  }

  return this->builtinConstructor(*found);
}

ClassDecl *LibraryReflect::builtinConstructor(BuiltinConstructor which) {
  llvm::StringRef name = BuiltinConstructorTable::getName(which);
  auto &reflection = this->state.library.reflection;
  auto *known = reflection.findBuiltinConstructor(name);
  if (known != nullptr) {
    return known;
  }
  auto *classDecl = this->state.ast.make<ClassDecl>(llvm::SMLoc(), name);
  classDecl->origin = ClassOrigin::BUILTIN;
  classDecl->stage = ClassStage::RESOLVED;
  classDecl->declared = true;
  reflection.rememberBuiltinConstructor(name, classDecl);

  return classDecl;
}

}
