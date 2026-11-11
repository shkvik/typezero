#pragma once

#include "compiler/program/library/reflect/reflect_function_table/reflect_function_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library/reflect/reflect_call_checker/reflect_call_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
struct MetadataType;
class CallChecker;
class ExprChecker;
class TypeRelations;
class ReflectChecker;
class ReturnInferrer;
class ArgumentChecker;
class MetadataChecker;

class ReflectCallChecker {
public:
  ReflectCallChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkReflectArgs(Expr *args);
  FunctionDecl *knownReflectMethod(ClassDecl *cls, llvm::StringRef name, bool &several);
  const FunctionDecl *heldReflectMethod(const Expr *f);
  Expr *directReflectApply(CallExpr *call, const FunctionDecl *fn);
  Expr *checkReflectCall(CallExpr *call, MemberExpr *member, const Type *expected);
  static bool reflectAccepts(const Type *arg, const Type *param);

private:
  using ResultKeyOfRecords = Result<const Type *, ReflectCallCheckerKeyOfRecordsError>;
  using ResultTargetObject = Result<const Type *, ReflectCallCheckerTargetObjectError>;
  using ResultArgumentList = Result<const Type *, ReflectCallCheckerArgumentListError>;

  static ResultKeyOfRecords keyOfRecords(const Type *type);
  static ResultTargetObject targetObject(const Type *type);
  static ResultArgumentList argumentList(const Type *type);
  static const Type *designType(const Expr *key);
  static bool isDesignKey(const Expr *key);
  static Expr *stripImplicit(Expr *expr);
  static bool declaredBelow(const ClassDecl *cls, llvm::StringRef name);
  static bool acceptsPlain(const Type *arg, const Type *param);

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
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  ReflectChecker *reflectChecker = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  MetadataChecker *metadataChecker = nullptr;
};

}
