#pragma once

#include "compiler/semantic/generics/generic_class_instantiator/generic_class_instantiator_errors.h"
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

  static bool isWidening(const Type *from, const Type *to);
  static bool isCovariant(const ClassDecl *generic, size_t index);
  static const ClassDecl *instanceOf(const ClassDecl *classDecl, const ClassDecl *generic);
  std::string whyNot(const Type *from, const Type *to) const;
  Expr *joined(Expr *expr);

private:
  std::vector<const ClassDecl *> instancesOf(llvm::ArrayRef<const Type *> members,
                                             const ClassDecl *generic) const;
  Expr *widenToGeneric(Expr *expr, ClassDecl *generic, llvm::ArrayRef<const ClassDecl *> instances);
  std::optional<std::vector<const Type *>>
  joinedArgs(const ClassDecl *generic, llvm::ArrayRef<const ClassDecl *> instances) const;
  enum class Variance {
    SAFE,
    UNSAFE,
    WAITING,
  };

  static bool isMentioning(const TypeRef &ref, llvm::StringRef name);
  static std::map<std::pair<const ClassDecl *, size_t>, bool> &knownVariance();
  std::string writerOf(const ClassDecl *generic, llvm::StringRef name) const;
  static bool isWrittenFromOutside(const ClassDecl *generic, llvm::StringRef name);
  static std::optional<bool> readyVariance(const ClassDecl *generic, size_t index);
  static void startVariance(std::vector<VarianceFrame> &frames, const ClassDecl *generic,
                            size_t index);
  static void varianceStep(std::vector<VarianceFrame> &frames);
  static void finishVariance(std::vector<VarianceFrame> &frames, Variance variance);
  static Variance argumentVariance(const ClassDecl *baseGeneric, const TypeRef &arg,
                                   llvm::StringRef name, size_t index);
  static bool isParamMentioning(const Param &param, llvm::StringRef name);
  static bool isTaking(const FunctionDecl *method, llvm::StringRef name);
  static bool hasOpenPropertyParam(const ClassDecl *generic, llvm::StringRef name);
  static const ClassField *settableField(const ClassDecl *generic, llvm::StringRef name);
  static const FunctionDecl *publicTaker(const ClassDecl *generic, llvm::StringRef name);
  bool isReferenceFitting(const Type *own, const Type *wide) const;
  const ClassDecl *genericTarget(const Type *from, const Type *to) const;
  ClassDecl *sharedGeneric(const ClassDecl *first, llvm::ArrayRef<const Type *> members) const;
  std::string invariantText(const ClassDecl *generic, size_t index) const;
  const ClassDecl *memberInstance(const Type *member, const ClassDecl *generic) const;
  std::vector<const Type *> distinctArgs(llvm::ArrayRef<const ClassDecl *> instances,
                                         size_t index) const;
  std::string argumentWhyNot(const ClassDecl *generic, const Type *own, const Type *wide,
                             size_t index) const;
  void reportInstantiation(const GenericClassInstantiatorInstantiateClassError &error,
                           const ClassDecl *generic, llvm::SMLoc use);

  SemanticState &state;
  TypeRelations *typeRelations = nullptr;
  GenericClassInstantiator *genericClassInstantiator = nullptr;
};

}
