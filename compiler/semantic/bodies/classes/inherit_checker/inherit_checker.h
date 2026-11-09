#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/classes/inherit_checker/inherit_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include <vector>

namespace tz {

class SemanticParts;
class EventMaps;
class FlowChecker;
class ClassChecker;
class EmitterChecker;
class ReturnInferrer;
class ArgumentChecker;
class TypeArgInferrer;
class OverloadResolver;
class ClassMemberChecker;

class InheritChecker {
public:
  InheritChecker(SemanticState &owner, SemanticParts &all);

  void checkAbstract(ClassDecl *cls);
  void checkImplements(ClassDecl *cls);
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

  static bool isArrowMethod(const FunctionDecl *fn);
  static ResultParentClassFound parentClass(ClassDecl *cls);
  ResultSuperIsStatement rootSuper(const CallExpr *call) const;
  ResultSuperNotRepeated superOnce(const ClassDecl *cls) const;
  static ResultConstructorScope superAllowed(FunctionDecl *ctor);
  static ResultSuperMethodScope superContext(FunctionDecl *method);
  static bool takesParams(const Type *fnType, const FuncSymbol *sym);
  ResultObjectIsComplete objectComplete(const MemberExpr *member) const;
  static void interfacesOf(const ClassDecl *cls, std::vector<const Type *> &out);
  static ResultMethodHasItsBody methodBody(FuncSymbol *sym, llvm::StringRef name);
  static ResultParentHasMethods parentMethods(const ClassDecl *parent, llvm::StringRef name);
  static ResultNoSuperArguments superArguments(const CallExpr *call, const ClassDecl *parent);
  static ResultAbstractHasClass abstractNeedsClass(const ClassDecl *cls, const FunctionDecl *fn);
  static ResultAbstractOverride abstractImplemented(const ClassDecl *cls,
                                                    const FunctionDecl *found);
  static ResultMemberIsFunction methodForField(const Field &member, const Type *iface,
                                               const ClassDecl *cls);
  static ResultSignatureVariant signatureVariant(llvm::StringRef prefix, llvm::StringRef name,
                                                 const Type *signature, bool found);
  static ResultMemberIsDeclared memberDeclared(llvm::StringRef prefix, const Field &member,
                                               llvm::ArrayRef<FunctionDecl *> methods);
  static ResultFieldTypeMatches fieldMatches(llvm::StringRef prefix, const Field &member,
                                             const Field *actual, const ClassDecl *cls);
  static ResultReplaceableField replaceable(const Field &member,
                                            llvm::ArrayRef<FunctionDecl *> methods,
                                            const Type *iface, const ClassDecl *cls);
  static ResultReturnTypeAgrees returnMatches(llvm::StringRef name, const Type *ret,
                                              const Type *expected, const Type *iface,
                                              const ClassDecl *cls);
  static ResultVariantForParams variantFound(llvm::StringRef prefix, const Field &member,
                                             llvm::ArrayRef<FunctionDecl *> methods,
                                             const ClassDecl *cls, bool found);

  SemanticState &state;
  EventMaps *eventMaps = nullptr;
  FlowChecker *flowChecker = nullptr;
  ClassChecker *classChecker = nullptr;
  EmitterChecker *emitterChecker = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  TypeArgInferrer *typeArgInferrer = nullptr;
  OverloadResolver *overloadResolver = nullptr;
  ClassMemberChecker *classMemberChecker = nullptr;
};

}
