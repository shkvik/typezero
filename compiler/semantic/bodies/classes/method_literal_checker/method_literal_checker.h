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

  Expr *checkMethodLiteral(ObjectLitExpr *literal, const Type *target);

private:
  using ResultSpreadAbsent = Result<const ObjectLitExpr *, MethodLiteralCheckerNoSpreadError>;
  using ResultMethodSimple = Result<const FunctionDecl *, MethodLiteralCheckerPlainMethodError>;
  using ResultValueIsTyped = Result<const Type *, MethodLiteralCheckerFieldValueError>;
  using ResultPropertyFits =
      Result<const ObjectLitExpr::Property *, MethodLiteralCheckerPropertyFitsError>;
  using ResultMembersMatch = Result<const ObjectLitExpr *, MethodLiteralCheckerMembersMatchError>;

  ResultSpreadAbsent checkNoSpread(const ObjectLitExpr *literal) const;
  ResultMethodSimple checkPlainMethod(const FunctionDecl *functionDecl) const;
  ResultValueIsTyped checkFieldValue(const Type *valueType, llvm::StringRef label) const;
  ResultMembersMatch checkMembersMatch(const ObjectLitExpr *literal, const Type *target) const;
  bool hasMethodNamed(const ClassDecl *classDecl, llvm::StringRef name) const;
  ResultPropertyFits checkPropertyFits(const ObjectLitExpr::Property &property,
                                       const Type *target) const;
  Expr *failMethodLiteral(ObjectLitExpr *literal);
  Type *makeLiteralClass(ObjectLitExpr *literal);
  void addLiteralMethod(ClassDecl *classDecl, const ObjectLitExpr::Property &property);
  bool addLiteralField(ClassDecl *classDecl, ObjectLitExpr::Property &property, const Type *target,
                       std::vector<Field> &fields);
  std::string literalTypeName(const std::vector<Field> &fields,
                              llvm::ArrayRef<FunctionDecl *> methods) const;
  void declareLiteralMethods(ClassDecl *classDecl, const Type *target);
  void declareLiteralMethod(FunctionDecl *functionDecl, const Type *target);
  void walkLiteralMethods(ClassDecl *classDecl);
  void walkLiteralMethod(FunctionDecl *functionDecl);
  Expr *matchLiteralTarget(ObjectLitExpr *literal, const Type *target, const Type *type);

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
