#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "compiler/semantic/types/type_operators/type_operators_errors.h"

namespace tz {

class SemanticParts;
class EventMaps;
class TypeParams;
class TypeResolver;
class UtilityTypes;
class ClassDeclarer;
class TemplateTypes;
class ArgListChecker;
class ReturnInferrer;
class ExtendsRelation;
class TopLevelChecker;
class ConditionalTypes;
class ClassValueChecker;

class TypeOperators {
public:
  using ResultTypeOpResolved = Result<const Type *, TypeOperatorsResolveTypeOpError>;

  TypeOperators(SemanticState &owner, SemanticParts &all);

  const Type *keysOf(const Type *type);
  ResultTypeOpResolved resolveTypeOp(const TypeRef &ref);
  const Type *evalTypeOp(const Type *op, llvm::ArrayRef<const Type *> operands);

private:
  const Type *ownKeys(const Type *type);
  static std::u16string utf16(llvm::StringRef text);
  static llvm::ArrayRef<const Type *> membersOf(const Type *&type);

  SemanticState &state;
  EventMaps *eventMaps = nullptr;
  TypeParams *typeParams = nullptr;
  TypeResolver *typeResolver = nullptr;
  UtilityTypes *utilityTypes = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  TemplateTypes *templateTypes = nullptr;
  ArgListChecker *argListChecker = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
  ExtendsRelation *extendsRelation = nullptr;
  TopLevelChecker *topLevelChecker = nullptr;
  ConditionalTypes *conditionalTypes = nullptr;
  ClassValueChecker *classValueChecker = nullptr;
};

}
