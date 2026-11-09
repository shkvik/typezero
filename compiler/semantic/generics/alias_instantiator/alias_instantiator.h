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

  static ResultDefaultExists defaultExists(const TypeAliasDecl *alias, size_t count,
                                           const Type *fallback);
  static ResultArgumentCount argumentCount(const TypeAliasDecl *alias, size_t count, size_t got);
  static ResultAliasDistinct notResolving(const TypeAliasDecl *alias, const AliasInstance &entry);
  ResultDepthInBounds depthInBounds(const TypeAliasDecl *alias, unsigned calls,
                                    bool conditional) const;
  ResultArgumentMatch argumentMatch(const TypeAliasDecl *alias, llvm::ArrayRef<const Type *> params,
                                    llvm::ArrayRef<const Type *> args, size_t k);
  static std::string typeList(llvm::ArrayRef<const Type *> types);

  SemanticState &state;
  ClassBases *classBases = nullptr;
  TypeParams *typeParams = nullptr;
  TypeResolver *typeResolver = nullptr;
};

}
