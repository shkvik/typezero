#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/generics_v2/aliases/generics_aliases_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <string>

namespace tz {

class GenericsAliasesRegistry {
public:
  explicit GenericsAliasesRegistry(SemanticState &owner);

  void dropOwnAliasAnswer();
  void closeAliasBody(const CloseAliasBody &task);
  void afterAliasInner(const AfterAliasInner &task);
  void afterAliasMatch(const AfterAliasMatch &task);
  void afterAliasObject(const AfterAliasObject &task);
  void afterAliasParams(const AfterAliasParams &task);
  void instantiateAlias(const InstantiateAlias &task);
  void afterAliasDefault(const AfterAliasDefault &task);
  void afterOwnAliasParams(const AfterOwnAliasParams &task);
  void instantiateOwnAlias(const InstantiateOwnAlias &task);

private:
  using ResultArgumentCount = Result<size_t, GenericsAliasesRegistryArgumentCountError>;
  using ResultArgumentMatch = Result<const Type *, GenericsAliasesRegistryArgumentMatchError>;
  using ResultDepthInBounds = Result<unsigned, GenericsAliasesRegistryDepthInBoundsError>;
  using ResultDefaultExists = Result<const Type *, GenericsAliasesRegistryDefaultExistsError>;
  using ResultAliasDistinct =
      Result<const TypeAliasDecl *, GenericsAliasesRegistryNotResolvingError>;

  void nextAliasMatch(AliasDraft draft);
  void startAliasBody(AliasDraft draft);
  void startAliasStep(AliasDraft draft);
  void nextAliasDefault(AliasDraft draft);
  void finishAlias(const AliasDraft &draft, const Type *type);
  void rememberAliasType(const AliasDraft &draft, const Type *type);
  void bindAliasParams(const TypeAliasDecl *alias, llvm::ArrayRef<const Type *> types);


  bool isAliasDepthFitting(const TypeAliasDecl *alias, const AliasInstance &entry, unsigned calls);


  const Type *reusedInstance(AliasInstance &entry) const;
  const Type *definedObject(const AliasDraft &draft, std::vector<Field> fields,
                            std::vector<Overload> overloads) const;


  std::string aliasDisplay(const AliasDraft &draft) const;
  ResultArgumentCount argumentCount(const TypeAliasDecl *alias, size_t count, size_t got) const;
  ResultAliasDistinct notResolving(const TypeAliasDecl *alias, const AliasInstance &entry) const;
  ResultDefaultExists defaultExists(const TypeAliasDecl *alias, size_t count,
                                    const Type *fallback) const;
  ResultDepthInBounds depthInBounds(const TypeAliasDecl *alias, unsigned calls,
                                    bool isConditional) const;

  ResultArgumentMatch argumentMatch(const AliasDraft &draft, bool isSatisfied) const;

  SemanticState &state;
};

}
