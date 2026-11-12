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
  FunctionDecl *knownReflectMethod(ClassDecl *classDecl, llvm::StringRef name, bool &several);
  const FunctionDecl *heldReflectMethod(const Expr *f);
  Expr *directReflectApply(CallExpr *call, const FunctionDecl *functionDecl);
  Expr *checkReflectCall(CallExpr *call, MemberExpr *member, const Type *expected);
  bool reflectAccepts(const Type *arg, const Type *param) const;

private:
  using ResultKeyOfRecords = Result<const Type *, ReflectCallCheckerKeyOfRecordsError>;
  using ResultTargetObject = Result<const Type *, ReflectCallCheckerTargetObjectError>;
  using ResultArgumentList = Result<const Type *, ReflectCallCheckerArgumentListError>;

  ResultKeyOfRecords keyOfRecords(const Type *type) const;
  ResultTargetObject targetObject(const Type *type) const;
  ResultArgumentList argumentList(const Type *type) const;
  const Type *designType(const Expr *key) const;
  bool isDesignKey(const Expr *key) const;
  Expr *stripImplicit(Expr *expr) const;
  bool declaredBelow(const ClassDecl *classDecl, llvm::StringRef name) const;
  bool acceptsPlain(const Type *arg, const Type *param) const;

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
