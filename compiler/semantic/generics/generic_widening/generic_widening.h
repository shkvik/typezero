#pragma once

#include "compiler/semantic/generics/generic_widening/generic_widening_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class TypeRelations;
class GenericClassInstantiator;

struct VarianceFrame {
  const ClassDecl *generic;
  size_t index;
  const ClassDecl *baseGeneric;
  size_t nextArg;
};

class GenericWidening {
public:
  GenericWidening(SemanticState &owner, SemanticParts &all);

  static bool widens(const Type *from, const Type *to);
  static bool covariant(const ClassDecl *generic, size_t index);
  static const ClassDecl *instanceOf(const ClassDecl *cls, const ClassDecl *generic);
  static std::string whyNot(const Type *from, const Type *to);
  Expr *joined(Expr *expr);

private:
  static std::vector<const ClassDecl *> instancesOf(llvm::ArrayRef<const Type *> members,
                                                    const ClassDecl *generic);
  Expr *widenToGeneric(Expr *expr, ClassDecl *generic, llvm::ArrayRef<const ClassDecl *> instances);
  static std::optional<std::vector<const Type *>>
  joinedArgs(const ClassDecl *generic, llvm::ArrayRef<const ClassDecl *> instances);
  static bool mentions(const TypeRef &ref, llvm::StringRef name);
  static std::map<std::pair<const ClassDecl *, size_t>, bool> &knownVariance();
  static std::string writerOf(const ClassDecl *generic, llvm::StringRef name);
  static bool writtenFromOutside(const ClassDecl *generic, llvm::StringRef name);
  static std::optional<bool> readyVariance(const ClassDecl *generic, size_t index);
  static void startVariance(std::vector<VarianceFrame> &frames, const ClassDecl *generic,
                            size_t index);
  static void varianceStep(std::vector<VarianceFrame> &frames);
  static void finishVariance(std::vector<VarianceFrame> &frames, bool safe);
  static bool referenceFits(const Type *own, const Type *wide);

  SemanticState &state;
  TypeRelations *typeRelations = nullptr;
  GenericClassInstantiator *genericClassInstantiator = nullptr;
};

}
