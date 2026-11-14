#pragma once

#include "compiler/program/library/builtin_names/type_name_table/type_name_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/code/iterators/code_iterators_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <string>
#include <vector>

namespace tz {

class IteratorSignatures {
public:
  explicit IteratorSignatures(SemanticState &owner);

  void afterSoleIteratorReturn();
  void nextValueOf(const NextValueOf &task);
  void elementTypeOf(const ElementTypeOf &task);
  void afterNextMethods(const AfterNextMethods &task);
  void walkIterableParam(const WalkIterableParam &task);
  void finishIteratorArgs(const FinishIteratorArgs &task);
  void resolveIteratorType(const ResolveIteratorType &task);
  void finishIterableParam(const FinishIterableParam &task);
  void walkGeneratorReturn(const WalkGeneratorReturn &task);
  void finishIteratorResult(const FinishIteratorResult &task);

private:
  enum class IteratorForm {
    RESULT,
    SYNC,
    ASYNC,
  };

  using ResultIterableParam = Result<const Type *, CodeIteratorsRegistryIterableParamError>;
  using ResultIterableArgs = Result<const TypeRef *, CodeIteratorsRegistryIterableParamError>;
  using ResultIteratorForm = Result<IteratorForm, CodeIteratorsRegistryResolveIteratorTypeError>;
  using ResultGeneratorReturns =
      Result<const Type *, CodeIteratorsRegistryGeneratorReturnTypeError>;

  void pushNextValue(const Type *next);
  void pushIteratorArguments(const TypeRef &ref);


  bool isNamingNoValue(const TypeRef &arg) const;
  bool isFinishedResult(const Type *member) const;
  bool isIteratorResultShape(const Type *member) const;
  bool isPlainNext(const FunctionDecl *functionDecl) const;
  bool isResolvedArgument(const TypeRef &ref, size_t index) const;


  const Type *nextFieldType(const Type *object) const;
  const Type *knownElementType(const Type *type) const;
  const Type *nextResultValue(const Type *result) const;
  const Type *iteratorResultType(const Type *value, const Type *returned) const;
  const Type *takeTypeArgument(const TypeRef &ref, size_t index, const Type *fallback);
  const Type *addIterableTypeParam(FunctionDecl *functionDecl, FuncSymbol *function,
                                   const Type *element, const TypeRef &ref);


  ResultIteratorForm checkIteratorForm(const TypeRef &ref) const;
  const FunctionDecl *soleIteratorMethod(const Type *type) const;
  ResultIterableArgs checkIterableArguments(const TypeRef &ref) const;


  ClassDecl *classOf(const Type *type) const;
  std::string shownElementName(const Type *element) const;
  std::vector<FunctionDecl *> plainNextMethods(const Type *object) const;
  ResultGeneratorReturns checkGeneratorReturnType(const FunctionDecl *functionDecl,
                                                  const Type *declared);
  ResultIterableParam checkIterableParam(FunctionDecl *functionDecl, FuncSymbol *function,
                                         const Type *element, const TypeRef &ref);

  SemanticState &state;
};

}
