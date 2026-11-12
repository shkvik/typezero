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

  static bool isArrowMethod(const FunctionDecl *functionDecl);
  ResultParentClassFound parentClass(ClassDecl *classDecl) const;
  ResultSuperIsStatement rootSuper(const CallExpr *call) const;
  ResultSuperNotRepeated superOnce(const ClassDecl *classDecl) const;
  ResultConstructorScope superAllowed(FunctionDecl *constructor) const;
  ResultSuperMethodScope superContext(FunctionDecl *method) const;
  bool takesParams(const Type *functionType, const FuncSymbol *function) const;
  ResultObjectIsComplete objectComplete(const MemberExpr *member) const;
  void interfacesOf(const ClassDecl *classDecl, std::vector<const Type *> &out) const;
  ResultMethodHasItsBody methodBody(FuncSymbol *function, llvm::StringRef name) const;
  ResultParentHasMethods parentMethods(const ClassDecl *parent, llvm::StringRef name) const;
  ResultNoSuperArguments superArguments(const CallExpr *call, const ClassDecl *parent) const;
  ResultAbstractHasClass abstractNeedsClass(const ClassDecl *classDecl,
                                            const FunctionDecl *functionDecl) const;
  ResultAbstractOverride abstractImplemented(const ClassDecl *classDecl,
                                             const FunctionDecl *found) const;
  ResultMemberIsFunction methodForField(const Field &member, const Type *interfaceType,
                                        const ClassDecl *classDecl) const;
  ResultSignatureVariant signatureVariant(llvm::StringRef prefix, llvm::StringRef name,
                                          const Type *signature, bool found) const;
  ResultMemberIsDeclared memberDeclared(llvm::StringRef prefix, const Field &member,
                                        llvm::ArrayRef<FunctionDecl *> methods) const;
  ResultFieldTypeMatches fieldMatches(llvm::StringRef prefix, const Field &member,
                                      const Field *actual, const ClassDecl *classDecl) const;
  ResultReplaceableField replaceable(const Field &member, llvm::ArrayRef<FunctionDecl *> methods,
                                     const Type *interfaceType, const ClassDecl *classDecl) const;
  ResultReturnTypeAgrees returnMatches(llvm::StringRef name, const Type *returnType,
                                       const Type *expected, const Type *interfaceType,
                                       const ClassDecl *classDecl) const;
  ResultVariantForParams variantFound(llvm::StringRef prefix, const Field &member,
                                      llvm::ArrayRef<FunctionDecl *> methods,
                                      const ClassDecl *classDecl, bool found) const;
  FunctionDecl *enclosingMethod();
  bool superObjectReady(MemberExpr *member);
  Expr *callSuperMethod(CallExpr *call, MemberExpr *member, llvm::ArrayRef<FunctionDecl *> methods);
  Expr *checkGenericSuperCall(CallExpr *call, MemberExpr *member, FuncSymbol *function);
  ClassDecl *superCallClass(const CallExpr *call);
  void walkParentConstructor(CallExpr *call, llvm::ArrayRef<FunctionDecl *> constructors);
  void walkImplementedMember(ClassDecl *classDecl, const Type *interfaceType, const Field &member,
                             const std::string &prefix);
  void walkImplementedField(const ClassDecl *classDecl, const Field &member,
                            const ClassField *field, const std::string &prefix);
  bool hasMatchingVariant(const ClassDecl *classDecl, const Type *interfaceType,
                          const Field &member, llvm::ArrayRef<FunctionDecl *> methods);
  void walkImplementedOverload(ClassDecl *classDecl, const Type *interfaceType,
                               const Overload &overload, const std::string &prefix);
  FunctionDecl *methodTaking(llvm::ArrayRef<FunctionDecl *> methods, const Type *signature) const;

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
