#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/classes/class_facts/class_facts.h"
#include "compiler/semantic/code/classes/code_classes_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <string>
#include <vector>

namespace tz {

class ClassBodyWalks {
public:
  ClassBodyWalks(SemanticState &owner, ClassFacts &sharedFacts);

  void declareThis(const DeclareThis &task);
  void checkFieldsSet(const CheckFieldsSet &task);
  void walkClassBodies(const WalkClassBodies &task);
  void walkClassMethod(const WalkClassMethod &task);
  void walkFieldStarts(const WalkFieldStarts &task);
  void finishClassBodies(const FinishClassBodies &task);
  void walkClassFunction(const WalkClassFunction &task);
  void bindOuterTypeParams(const BindOuterTypeParams &task);
  void afterClassMethodReturn(const AfterClassMethodReturn &task);

private:
  using ResultReplaceableField = Result<const Field *, CodeClassesRegistryReplaceableError>;
  using ResultFieldTypeMatches = Result<const Field *, CodeClassesRegistryFieldMatchesError>;
  using ResultReturnTypeAgrees = Result<const Type *, CodeClassesRegistryReturnMatchesError>;
  using ResultVariantForParams = Result<const Field *, CodeClassesRegistryVariantFoundError>;
  using ResultMemberIsDeclared = Result<const Field *, CodeClassesRegistryMemberDeclaredError>;
  using ResultMemberIsFunction = Result<const Field *, CodeClassesRegistryMethodForFieldError>;
  using ResultSignatureVariant = Result<const Type *, CodeClassesRegistrySignatureVariantError>;
  using ResultFieldHasValue = Result<const ClassField *, CodeClassesRegistryFieldStartValueError>;
  using ResultMethodTakesThis = Result<const FunctionDecl *, CodeClassesRegistryDeclareThisError>;
  using ResultEveryFieldSet = Result<const FunctionDecl *, CodeClassesRegistryCheckFieldsSetError>;
  using ResultReturnMatches = Result<const FunctionDecl *, CodeClassesRegistryOverrideReturnError>;
  using ResultAbstractHasClass =
      Result<const FunctionDecl *, CodeClassesRegistryAbstractNeedsClassError>;
  using ResultAbstractOverride =
      Result<const FunctionDecl *, CodeClassesRegistryAbstractImplementedError>;

  void walkAbstract(ClassDecl *classDecl);
  void walkEmptySlot(const ClassDecl *classDecl, size_t slot);
  void walkImplements(ClassDecl *classDecl, KnownReturns &returns);
  void walkOverrideReturns(const ClassDecl *classDecl, KnownReturns &returns);
  void interfacesOf(const ClassDecl *classDecl, std::vector<const Type *> &out) const;
  void walkImplementedInterface(ClassDecl *classDecl, const Type *interfaceType,
                                KnownReturns &returns);
  void addInterface(const Type *interfaceType, std::vector<const Type *> &queue,
                    std::vector<const Type *> &out) const;
  void walkImplementedMember(ClassDecl *classDecl, const Type *interfaceType, const Field &member,
                             const std::string &prefix, KnownReturns &returns);
  void walkImplementedField(const ClassDecl *classDecl, const Field &member,
                            const ClassField *field, const std::string &prefix);
  void walkImplementedOverload(ClassDecl *classDecl, const Type *interfaceType,
                               const Overload &overload, const std::string &prefix,
                               KnownReturns &returns);
  void walkVariantReturn(const ClassDecl *classDecl, const Type *interfaceType, const Field &member,
                         const FunctionDecl *functionDecl, KnownReturns &returns);
  void walkVariantReturns(const ClassDecl *classDecl, const Type *interfaceType,
                          const Field &member, llvm::ArrayRef<FunctionDecl *> methods,
                          KnownReturns &returns);
  void walkOverloadSignature(ClassDecl *classDecl, const Type *interfaceType,
                             const Overload &overload, llvm::ArrayRef<FunctionDecl *> methods,
                             const Type *signature, const std::string &prefix,
                             KnownReturns &returns);


  bool isTakingParams(const Type *functionType, const FuncSymbol *function) const;
  bool isTakingMember(const Field &member, const FunctionDecl *functionDecl) const;
  bool hasMatchingVariant(const Field &member, llvm::ArrayRef<FunctionDecl *> methods) const;


  FunctionDecl *ownSlotMethod(const ClassDecl *owner, size_t slot) const;
  FunctionDecl *slotMethod(const ClassDecl *classDecl, size_t slot) const;
  FunctionDecl *methodTaking(llvm::ArrayRef<FunctionDecl *> methods, const Type *signature) const;


  ResultEveryFieldSet checkEveryFieldSet() const;
  const Type *knownReturn(FuncSymbol *function, KnownReturns &returns) const;
  ResultReturnMatches checkOverrideReturn(const ClassDecl *classDecl, const FunctionDecl *method,
                                          KnownReturns &returns);
  ResultFieldHasValue checkFieldStartValue(const ClassDecl *classDecl,
                                           const ClassField &field) const;


  ResultAbstractOverride checkAbstractImplemented(const ClassDecl *classDecl,
                                                  const FunctionDecl *found) const;
  ResultAbstractHasClass checkAbstractNeedsClass(const ClassDecl *classDecl,
                                                 const FunctionDecl *functionDecl) const;
  ResultMemberIsFunction checkMethodForField(const Field &member, const Type *interfaceType,
                                             const ClassDecl *classDecl) const;
  ResultMemberIsDeclared checkMemberDeclared(llvm::StringRef prefix, const Field &member,
                                             llvm::ArrayRef<FunctionDecl *> methods) const;
  ResultSignatureVariant checkSignatureVariant(llvm::StringRef prefix, llvm::StringRef name,
                                               const Type *signature, bool isFound) const;
  ResultFieldTypeMatches checkFieldMatches(llvm::StringRef prefix, const Field &member,
                                           const Field *actual, const ClassDecl *classDecl) const;
  ResultReplaceableField checkReplaceable(const Field &member,
                                          llvm::ArrayRef<FunctionDecl *> methods,
                                          const Type *interfaceType,
                                          const ClassDecl *classDecl) const;
  ResultReturnTypeAgrees checkReturnMatches(llvm::StringRef name, const Type *returnType,
                                            const Type *expected, const Type *interfaceType,
                                            const ClassDecl *classDecl) const;
  ResultVariantForParams checkVariantFound(llvm::StringRef prefix, const Field &member,
                                           llvm::ArrayRef<FunctionDecl *> methods,
                                           const ClassDecl *classDecl, bool isFound) const;


  std::vector<FunctionDecl *> classMethods(const ClassDecl *classDecl) const;
  ResultMethodTakesThis checkMethodTakesThis(const FunctionDecl *functionDecl) const;

  ClassFacts &facts;
  SemanticState &state;
};

}
