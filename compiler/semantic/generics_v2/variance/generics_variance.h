#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace tz {

class GenericsVariance {
public:
  bool isWidening(const Type *from, const Type *to);
  bool isCovariant(const ClassDecl *generic, size_t index);


  std::string whyNot(const Type *from, const Type *to);
  const ClassDecl *instanceOf(const ClassDecl *classDecl, const ClassDecl *generic);
  ClassDecl *sharedGeneric(const ClassDecl *first, llvm::ArrayRef<const Type *> members);
  std::vector<const ClassDecl *> instancesOf(llvm::ArrayRef<const Type *> members,
                                             const ClassDecl *generic);
  std::optional<std::vector<const Type *>> joinedArgs(const ClassDecl *generic,
                                                      llvm::ArrayRef<const ClassDecl *> instances);

private:
  enum class Variance {
    SAFE,
    UNSAFE,
    WAITING,
  };

  struct VarianceFrame {
    const ClassDecl *generic;
    size_t index;
    const ClassDecl *baseGeneric;
    size_t nextArg;
  };

  void varianceStep(std::vector<VarianceFrame> &frames);
  void finishVariance(std::vector<VarianceFrame> &frames, Variance variance);
  void startVariance(std::vector<VarianceFrame> &frames, const ClassDecl *generic, size_t index);


  bool isReferenceFitting(const Type *own, const Type *wide);
  bool isMentioning(const TypeRef &ref, llvm::StringRef name);
  bool isTaking(const FunctionDecl *method, llvm::StringRef name);
  bool isParamMentioning(const Param &param, llvm::StringRef name);
  bool hasOpenPropertyParam(const ClassDecl *generic, llvm::StringRef name);
  bool isWrittenFromOutside(const ClassDecl *generic, llvm::StringRef name);


  std::string invariantText(const ClassDecl *generic, size_t index);
  std::string writerOf(const ClassDecl *generic, llvm::StringRef name);
  std::string argumentWhyNot(const ClassDecl *generic, const Type *own, const Type *wide,
                             size_t index);


  const ClassDecl *genericTarget(const Type *from, const Type *to);
  const ClassDecl *memberInstance(const Type *member, const ClassDecl *generic);


  std::optional<bool> readyVariance(const ClassDecl *generic, size_t index);
  const ClassField *settableField(const ClassDecl *generic, llvm::StringRef name);
  const FunctionDecl *publicTaker(const ClassDecl *generic, llvm::StringRef name);
  std::vector<const Type *> distinctArgs(llvm::ArrayRef<const ClassDecl *> instances, size_t index);
  Variance argumentVariance(const ClassDecl *baseGeneric, const TypeRef &arg, llvm::StringRef name,
                            size_t index);

  std::map<std::pair<const ClassDecl *, size_t>, bool> knownVariances;
};

}
