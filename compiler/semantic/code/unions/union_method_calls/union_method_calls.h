#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/classes/code_classes_registry_errors.h"
#include "compiler/semantic/code/unions/code_unions_registry_errors.h"
#include "compiler/semantic/code/unions/union_rules/union_rules.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <vector>

namespace tz {

class UnionMethodCalls {
public:
  UnionMethodCalls(SemanticState &owner, UnionRules &sharedUnionRules);

  void checkUnionMethodCall(const CheckUnionMethodCall &task);
  void reportUnionMethodCall(const ReportUnionMethodCall &task);
  void resolveUnionMethodCall(const ResolveUnionMethodCall &task);

private:
  struct UnionMethodPart {
    const Type *type;
    UnionMethod entry;
  };
  struct UnionMethods {
    const Type *type;
    std::vector<UnionMethod> methods;
  };

  using ResultMethodAllowed = Result<void, CodeUnionsRegistryCheckUnionMethodCallError>;
  using ResultUnionMethodList = Result<UnionMethods, CodeUnionsRegistryCheckUnionMethodCallError>;
  using ResultUnionMethodPart =
      Result<UnionMethodPart, CodeUnionsRegistryCheckUnionMethodCallError>;

  bool isAllObjects(const Type *type) const;


  UnionMethodPart missingPart(const Type *memberType) const;
  UnionMethodPart fieldPart(const Type *memberType, llvm::StringRef name) const;


  ResultMethodAllowed checkNarrowedObjects(const MemberExpr *member, const Type *receiver) const;
  ResultMethodAllowed checkSameMethodType(const MemberExpr *member, const Type *common,
                                          const Type *partType) const;
  ResultMethodAllowed checkCallablePart(const MemberExpr *member, const Type *memberType,
                                        const Type *partType) const;


  ResultUnionMethodPart memberMethod(const MemberExpr *member, const Type *memberType);
  ResultUnionMethodList resolveUnionMethods(const MemberExpr *member, const Type *receiver);
  ResultUnionMethodPart classMemberMethod(const MemberExpr *member, const Type *memberType,
                                          ClassDecl *classDecl);
  ResultUnionMethodPart classFieldPart(const MemberExpr *member, const Type *memberType,
                                       const ClassDecl *classDecl);
  ResultUnionMethodPart classVariantMethod(const MemberExpr *member, const Type *memberType,
                                           ClassDecl *classDecl, FunctionDecl *variant);


  const Type *knownFunctionType(const FuncSymbol *function) const;
  std::vector<FuncSymbol *> neededFunctions(const MemberExpr *member) const;
  UnionMethod variantEntry(const Type *memberType, ClassDecl *classDecl,
                           FuncSymbol *function) const;

  SemanticState &state;
  UnionRules &unionRules;
};

}
