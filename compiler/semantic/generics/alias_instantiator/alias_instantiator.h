#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/generics/alias_instantiator/alias_instantiator_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class ClassBases;
class TypeParams;
class TypeResolver;

class AliasInstantiator {
public:
  AliasInstantiator(SemanticState &owner, SemanticParts &all);

  const Type *instantiateAlias(TypeAliasDecl *alias, std::vector<const Type *> args,
                               llvm::SMLoc loc);
  std::vector<const Type *> aliasTypeParams(TypeAliasDecl *alias);

private:
  using ResultArgumentCount = Result<size_t, AliasInstantiatorArgumentCountError>;
  using ResultDefaultExists = Result<const Type *, AliasInstantiatorDefaultExistsError>;
  using ResultArgumentMatch = Result<const Type *, AliasInstantiatorArgumentMatchError>;
  using ResultAliasDistinct = Result<const TypeAliasDecl *, AliasInstantiatorNotResolvingError>;
  using ResultDepthInBounds = Result<unsigned, AliasInstantiatorDepthInBoundsError>;

  ResultDefaultExists defaultExists(const TypeAliasDecl *alias, size_t count,
                                    const Type *fallback) const;
  ResultArgumentCount argumentCount(const TypeAliasDecl *alias, size_t count, size_t got) const;
  ResultAliasDistinct notResolving(const TypeAliasDecl *alias, const AliasInstance &entry) const;
  ResultDepthInBounds depthInBounds(const TypeAliasDecl *alias, unsigned calls,
                                    bool isConditional) const;
  ResultArgumentMatch argumentMatch(const TypeAliasDecl *alias, llvm::ArrayRef<const Type *> params,
                                    llvm::ArrayRef<const Type *> args, size_t index);

  struct AliasRun {
    TypeAliasDecl *alias = nullptr;
    std::vector<const Type *> args;
    llvm::SMLoc loc;
    std::vector<const Type *> defaults;
    std::vector<AliasInstance *> chain;
  };

  const Type *aliasStep(AliasRun &run, unsigned calls);
  const Type *failedAliasArgs(AliasRun &run);
  const Type *reusedInstance(AliasInstance &entry) const;
  bool isAliasDepthFitting(const TypeAliasDecl *alias, const AliasInstance &entry, unsigned calls);
  std::string aliasDisplay(const AliasRun &run) const;
  const Type *resolveAliasBody(const AliasRun &run, AliasInstance &entry,
                               const std::string &display);
  const Type *objectAliasType(TypeAliasDecl *alias, AliasInstance &entry,
                              const std::string &display);
  const Type *aliasTypeInner(const TypeAliasDecl *alias);
  const Type *definedObject(const AliasInstance &entry, std::vector<Field> fields,
                            std::vector<Overload> overloads, const std::string &display) const;
  void rememberAliasType(const AliasRun &run, const Type *type, const std::string &display);

  SemanticState &state;
  ClassBases *classBases = nullptr;
  TypeParams *typeParams = nullptr;
  TypeResolver *typeResolver = nullptr;
};

}
