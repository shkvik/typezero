#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/generics/alias_instantiator/alias_instantiator_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class AliasInstantiator {
public:
  explicit AliasInstantiator(CheckState &owner);

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

  CheckState &state;
};

}
