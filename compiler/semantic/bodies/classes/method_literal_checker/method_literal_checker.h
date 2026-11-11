#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/classes/method_literal_checker/method_literal_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class CallCycles;
class ClassBases;
class ExprChecker;
class ClassDeclarer;
class TypeRelations;
class ReturnInferrer;
class FunctionChecker;
class PredicateChecker;

class MethodLiteralChecker {
public:
  MethodLiteralChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkMethodLiteral(ObjectLitExpr *lit, const Type *target);

private:
  using ResultSpreadAbsent = Result<const ObjectLitExpr *, MethodLiteralCheckerNoSpreadError>;
  using ResultMethodSimple = Result<const FunctionDecl *, MethodLiteralCheckerPlainMethodError>;
  using ResultValueIsTyped = Result<const Type *, MethodLiteralCheckerFieldValueError>;
  using ResultPropertyFits =
      Result<const ObjectLitExpr::Property *, MethodLiteralCheckerPropertyFitsError>;
  using ResultMembersMatch = Result<const ObjectLitExpr *, MethodLiteralCheckerMembersMatchError>;

  static ResultSpreadAbsent noSpread(const ObjectLitExpr *lit);
  static ResultMethodSimple plainMethod(const FunctionDecl *fn);
  static ResultValueIsTyped fieldValue(const Type *valueType, llvm::StringRef label);
  static ResultMembersMatch membersMatch(const ObjectLitExpr *lit, const Type *target);
  ResultPropertyFits propertyFits(const ObjectLitExpr::Property &prop, const Type *target) const;
  Type *makeLiteralClass(ObjectLitExpr *lit);
  void addLiteralMethod(ClassDecl *cls, const ObjectLitExpr::Property &prop);
  bool addLiteralField(ClassDecl *cls, ObjectLitExpr::Property &prop, const Type *target,
                       std::vector<Field> &fields);
  static std::string literalTypeName(const std::vector<Field> &fields,
                                     llvm::ArrayRef<FunctionDecl *> methods);
  void declareLiteralMethods(ClassDecl *cls, const Type *target);
  void declareLiteralMethod(FunctionDecl *fn, const Type *target);
  void checkLiteralMethods(ClassDecl *cls);
  Expr *matchLiteralTarget(ObjectLitExpr *lit, const Type *target, const Type *type);

  SemanticState &state;
  CallCycles *callCycles = nullptr;
  ClassBases *classBases = nullptr;
  ExprChecker *exprChecker = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  TypeRelations *typeRelations = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
  FunctionChecker *functionChecker = nullptr;
  PredicateChecker *predicateChecker = nullptr;
};

}
