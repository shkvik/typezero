#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/classes/inherit_checker/inherit_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include <vector>

namespace tz {

class ClassLinks;
class ClassBases;
class SemanticParts;
class EventMaps;
class FlowChecker;
class ClassChecker;
class ClassDeclarer;
class EmitterChecker;
class ReturnInferrer;
class ArgumentChecker;
class TypeArgInferrer;
class OverloadResolver;
class ClassMemberChecker;

class InheritChecker {
public:
  InheritChecker(SemanticState &owner, SemanticParts &all);

  void walkAbstract(ClassDecl *classDecl);
  void walkImplements(ClassDecl *classDecl);
  Expr *checkSuperCall(CallExpr *call);
  Expr *checkSuperMethodCall(CallExpr *call, MemberExpr *member);

private:
  using ResultParentClassFound = Result<ClassDecl *, InheritCheckerParentClassError>;
  using ResultMethodHasItsBody = Result<FuncSymbol *, InheritCheckerMethodBodyError>;
  using ResultReplaceableField = Result<const Field *, InheritCheckerReplaceableError>;
  using ResultFieldTypeMatches = Result<const Field *, InheritCheckerFieldMatchesError>;
  using ResultReturnTypeAgrees = Result<const Type *, InheritCheckerReturnMatchesError>;
  using ResultVariantForParams = Result<const Field *, InheritCheckerVariantFoundError>;
  using ResultSuperIsStatement = Result<const CallExpr *, InheritCheckerRootSuperError>;
  using ResultConstructorScope = Result<FunctionDecl *, InheritCheckerSuperAllowedError>;
  using ResultSuperNotRepeated = Result<const ClassDecl *, InheritCheckerSuperOnceError>;
  using ResultSuperMethodScope = Result<FunctionDecl *, InheritCheckerSuperContextError>;
  using ResultMemberIsDeclared = Result<const Field *, InheritCheckerMemberDeclaredError>;
  using ResultMemberIsFunction = Result<const Field *, InheritCheckerMethodForFieldError>;
  using ResultSignatureVariant = Result<const Type *, InheritCheckerSignatureVariantError>;
  using ResultNoSuperArguments = Result<const CallExpr *, InheritCheckerSuperArgumentsError>;
  using ResultObjectIsComplete = Result<const MemberExpr *, InheritCheckerObjectCompleteError>;
  using ResultAbstractHasClass =
      Result<const FunctionDecl *, InheritCheckerAbstractNeedsClassError>;
  using ResultAbstractOverride =
      Result<const FunctionDecl *, InheritCheckerAbstractImplementedError>;
  using ResultParentHasMethods =
      Result<std::vector<FunctionDecl *>, InheritCheckerParentMethodsError>;

  ResultAbstractHasClass checkAbstractNeedsClass(const ClassDecl *classDecl,
                                                 const FunctionDecl *functionDecl) const;
  ResultAbstractOverride checkAbstractImplemented(const ClassDecl *classDecl,
                                                  const FunctionDecl *found) const;
  void walkEmptySlot(const ClassDecl *classDecl, size_t slot);
  FunctionDecl *slotMethod(const ClassDecl *classDecl, size_t slot) const;
  FunctionDecl *ownSlotMethod(const ClassDecl *owner, size_t slot) const;
  ResultFieldTypeMatches checkFieldMatches(llvm::StringRef prefix, const Field &member,
                                           const Field *actual, const ClassDecl *classDecl) const;
  ResultMemberIsDeclared checkMemberDeclared(llvm::StringRef prefix, const Field &member,
                                             llvm::ArrayRef<FunctionDecl *> methods) const;
  ResultMemberIsFunction checkMethodForField(const Field &member, const Type *interfaceType,
                                             const ClassDecl *classDecl) const;
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
  ResultSignatureVariant checkSignatureVariant(llvm::StringRef prefix, llvm::StringRef name,
                                               const Type *signature, bool isFound) const;
  void walkImplementedInterface(ClassDecl *classDecl, const Type *interfaceType);
  void walkImplementedMember(ClassDecl *classDecl, const Type *interfaceType, const Field &member,
                             const std::string &prefix);
  void walkImplementedField(const ClassDecl *classDecl, const Field &member,
                            const ClassField *field, const std::string &prefix);
  void walkVariantReturns(const ClassDecl *classDecl, const Type *interfaceType,
                          const Field &member, llvm::ArrayRef<FunctionDecl *> methods);
  void walkVariantReturn(const ClassDecl *classDecl, const Type *interfaceType, const Field &member,
                         const FunctionDecl *functionDecl);
  bool hasMatchingVariant(const Field &member, llvm::ArrayRef<FunctionDecl *> methods) const;
  bool isTakingMember(const Field &member, const FunctionDecl *functionDecl) const;
  void walkImplementedOverload(ClassDecl *classDecl, const Type *interfaceType,
                               const Overload &overload, const std::string &prefix);
  void walkOverloadSignature(ClassDecl *classDecl, const Type *interfaceType,
                             const Overload &overload, llvm::ArrayRef<FunctionDecl *> methods,
                             const Type *signature, const std::string &prefix);
  FunctionDecl *methodTaking(llvm::ArrayRef<FunctionDecl *> methods, const Type *signature) const;
  ResultConstructorScope checkSuperAllowed(FunctionDecl *constructor) const;
  ResultParentClassFound checkParentClass(ClassDecl *classDecl) const;
  ResultSuperIsStatement checkRootSuper(const CallExpr *call) const;
  ResultSuperNotRepeated checkSuperOnce(const ClassDecl *classDecl) const;
  ResultNoSuperArguments checkSuperArguments(const CallExpr *call, const ClassDecl *parent) const;
  void walkSuperWithoutConstructor(CallExpr *call, const ClassDecl *parent);
  ClassDecl *superCallClass(const CallExpr *call);
  void walkParentConstructor(CallExpr *call, llvm::ArrayRef<FunctionDecl *> constructors);
  ResultSuperMethodScope checkSuperContext(FunctionDecl *method) const;
  ResultObjectIsComplete checkObjectComplete(const MemberExpr *member) const;
  ResultParentHasMethods checkParentMethods(const ClassDecl *parent, llvm::StringRef name) const;
  ResultMethodHasItsBody checkMethodBody(FuncSymbol *function, llvm::StringRef name) const;
  FunctionDecl *enclosingMethod();
  FunctionDecl *methodWithThis(FunctionDecl *functionDecl) const;
  bool prepareSuperObject(MemberExpr *member);
  Expr *callSuperMethod(CallExpr *call, MemberExpr *member, llvm::ArrayRef<FunctionDecl *> methods);
  Expr *checkGenericSuperCall(CallExpr *call, MemberExpr *member, FuncSymbol *function);

  SemanticState &state;
  EventMaps *eventMaps = nullptr;
  ClassLinks *classLinks = nullptr;
  ClassBases *classBases = nullptr;
  FlowChecker *flowChecker = nullptr;
  ClassChecker *classChecker = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  EmitterChecker *emitterChecker = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  TypeArgInferrer *typeArgInferrer = nullptr;
  OverloadResolver *overloadResolver = nullptr;
  ClassMemberChecker *classMemberChecker = nullptr;
};

}
