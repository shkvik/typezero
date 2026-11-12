#pragma once

#include "compiler/program/library/reflect/reflect_function_table/reflect_function_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library/reflect/reflect_call_checker/reflect_call_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class ClassLinks;
class SemanticParts;
struct MetadataType;
class CallChecker;
class ExprChecker;
class TypeRelations;
class ClassDeclarer;
class ReflectChecker;
class ReturnInferrer;
class ArgumentChecker;
class MetadataChecker;

class ReflectCallChecker {
public:
  ReflectCallChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkReflectArgs(Expr *args);
  struct KnownMethod {
    FunctionDecl *method;
    bool hasSeveral;
  };

  std::optional<KnownMethod> knownReflectMethod(ClassDecl *classDecl, llvm::StringRef name);
  const FunctionDecl *heldReflectMethod(const Expr *expr);
  Expr *directReflectApply(CallExpr *call, const FunctionDecl *functionDecl);
  Expr *checkReflectCall(CallExpr *call, MemberExpr *member, const Type *expected);
  bool isReflectAccepting(const Type *arg, const Type *param) const;

private:
  using ResultKeyOfRecords = Result<const Type *, ReflectCallCheckerKeyOfRecordsError>;
  using ResultTargetObject = Result<const Type *, ReflectCallCheckerTargetObjectError>;
  using ResultArgumentList = Result<const Type *, ReflectCallCheckerArgumentListError>;

  ResultKeyOfRecords checkKeyOfRecords(const Type *type) const;
  ResultTargetObject checkTargetObject(const Type *type) const;
  ResultArgumentList checkArgumentList(const Type *type) const;
  const Type *designType(const Expr *key) const;
  bool isDesignKey(const Expr *key) const;
  Expr *stripImplicit(Expr *expr) const;
  bool isDeclaredBelow(const ClassDecl *classDecl, llvm::StringRef name) const;
  bool isPlainAccepting(const Type *arg, const Type *param) const;
  bool isClassRefAccepting(const Type *arg, const Type *param) const;
  bool isObjectAccepting(const Type *arg, const Type *param) const;
  bool isGenericMethod(const FunctionDecl *functionDecl) const;
  bool isReflectCallable(ClassDecl *classDecl, FunctionDecl *functionDecl) const;
  bool hasOwnMethodNamed(const ClassDecl *owner, llvm::StringRef name) const;

  struct MetadataFunction {
    Builtin builtin = Builtin::None;
    unsigned min = 0;
    unsigned max = 0;
  };

  struct MetadataKey {
    const void *key = nullptr;
    std::string label;
    llvm::SMLoc loc;
    const Type *design = nullptr;
    bool failed = false;
  };

  Expr *checkArgsLoosely(CallExpr *call);
  void walkMetadataTarget(CallExpr *call, size_t at);
  MetadataKey readMetadataKey(CallExpr *call, bool define);
  void walkMetadataValue(CallExpr *call, const MetadataKey &key);
  void rememberMetadataType(const Expr *value, const MetadataKey &key);
  Expr *reportReflectName(CallExpr *call, MemberExpr *member, const std::string &message);
  Expr *coerceKnownMetadata(Expr *value, const MetadataType *known, const std::string &label);
  const Type *metadataResultType(Builtin builtin, const MetadataKey &key, const Type *expected);
  Expr *checkMetadataCall(CallExpr *call, MemberExpr *member, const Type *expected,
                          MetadataFunction function);

  SemanticState &state;
  ClassLinks *classLinks = nullptr;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  ReflectChecker *reflectChecker = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  MetadataChecker *metadataChecker = nullptr;
};

}
