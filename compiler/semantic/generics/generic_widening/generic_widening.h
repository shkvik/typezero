#pragma once

#include "compiler/semantic/generics/generic_widening/generic_widening_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class TypeRelations;
class GenericClassInstantiator;

class GenericWidening {
public:
  GenericWidening(SemanticState &owner, SemanticParts &all);

  static bool widens(const Type *from, const Type *to);
  static bool covariant(const ClassDecl *generic, size_t index);
  static const ClassDecl *instanceOf(const ClassDecl *cls, const ClassDecl *generic);
  static std::string whyNot(const Type *from, const Type *to);
  Expr *joined(Expr *expr);

private:
  static bool mentions(const TypeRef &ref, llvm::StringRef name);
  static std::map<std::pair<const ClassDecl *, size_t>, bool> &knownVariance();
  static std::string writerOf(const ClassDecl *generic, llvm::StringRef name);
  static bool writtenFromOutside(const ClassDecl *generic, llvm::StringRef name);
  static bool passedOnSafely(const ClassDecl *generic, llvm::StringRef name);
  static bool referenceFits(const Type *own, const Type *wide);
  static bool fitsWide(const Type *own, const Type *wide);

  SemanticState &state;
  TypeRelations *typeRelations = nullptr;
  GenericClassInstantiator *genericClassInstantiator = nullptr;
};

}
