#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/classes/code_classes_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <string>
#include <vector>

namespace tz {

class ClassFacts {
public:
  using ResultFieldUnsetYet = Result<std::string, CodeClassesRegistryFirstUnsetError>;
  using ResultObjectIsReady = Result<const Flow *, CodeClassesRegistryThisReadyError>;
  using ResultInitThisFound = Result<const NameExpr *, CodeClassesRegistryIsInitThisError>;
  using ResultMemberAccessible = Result<const ClassDecl *, CodeClassesRegistryCheckAccessError>;

  explicit ClassFacts(SemanticState &owner);

  bool isOverridingTransitively(const FunctionDecl *functionDecl, const FunctionDecl *base) const;


  ClassDecl *classOf(const Type *type) const;
  ClassDecl *finalClass(ClassDecl *classDecl) const;


  const Type *valueTypeOf(ClassDecl *classDecl) const;
  const Type *classRefType(const ClassDecl *classDecl) const;


  ResultInitThisFound isInitThis(const Expr *expr) const;
  ResultFieldUnsetYet firstUnset(const Flow &flow) const;
  ResultObjectIsReady checkThisReady(const Flow &flow) const;


  std::vector<FunctionDecl *> ownMethods(const ClassDecl *classDecl) const;
  std::vector<FunctionDecl *> constructorsOf(const ClassDecl *classDecl) const;
  std::vector<FunctionDecl *> signatureVariants(const Type *interfaceType,
                                                const Overload &overload);


  FunctionDecl *soleImplementation(const ClassDecl *classDecl, FunctionDecl *variant) const;
  int signatureIndex(llvm::ArrayRef<FunctionDecl *> variants, const FuncSymbol *function) const;
  ResultMemberAccessible checkAccess(const ClassDecl *classDecl, Access access,
                                     llvm::StringRef name, const Expr *receiver = nullptr);

private:
  struct OwnedMethod {
    FunctionDecl *method;
    const ClassDecl *owner;
  };

  void addImplementations(const ClassDecl *child, const FunctionDecl *variant,
                          std::vector<OwnedMethod> &implementations) const;


  bool isUnsetField(const Flow &flow, const ClassField &field) const;
  bool isDerivedFrom(const ClassDecl *from, const ClassDecl *base) const;
  bool isSameClass(const ClassDecl *first, const ClassDecl *second) const;


  Param signatureParam(const Type *signature, size_t index);
  FunctionDecl *signatureVariant(const Overload &overload, const Type *signature);
  std::vector<OwnedMethod> implementationsBelow(const ClassDecl *classDecl,
                                                const FunctionDecl *variant) const;
  ResultMemberAccessible checkProtectedReceiver(const ClassDecl *classDecl, llvm::StringRef name,
                                                const Expr *receiver) const;

  SemanticState &state;
};

}
