#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/objects/tuple_checker/tuple_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class CallChecker;
class ExprChecker;
class TypeRelations;
class ArgumentChecker;

class TupleChecker {
public:
  using ResultTupleMember = Result<Expr *, TupleCheckerCheckTupleMemberError>;
  using ResultTupleMethod = Result<Expr *, TupleCheckerCheckTupleMethodError>;
  using ResultTupleFilled = Result<Expr *, TupleCheckerCheckTupleLitError>;

  TupleChecker(SemanticState &owner, SemanticParts &all);

  Expr *arrayCopy(Expr *tuple);
  const Type *tupleElement(const Type *tuple) const;
  ResultTupleMember checkTupleMember(MemberExpr *member);
  ResultTupleFilled checkTupleLit(ArrayLitExpr *literal, const Type *tuple);
  ResultTupleMethod checkTupleMethod(CallExpr *call, MemberExpr *member);

private:
  using ResultTupleJoined = Result<Expr *, TupleCheckerCheckTupleJoinError>;

  ResultTupleJoined checkTupleJoin(CallExpr *call, MemberExpr *member);
  bool isJoinableElement(const Type *element) const;
  ResultTupleMethod checkJoinMethod(CallExpr *call, MemberExpr *member);
  TupleCheckerCheckTupleMethodErrorCode
  joinMethodCode(TupleCheckerCheckTupleJoinErrorCode code) const;
  std::string tupleText(const MemberExpr *member) const;
  bool isSpreadElement(const ArrayLitExpr::Element &element) const;
  void walkLooseElements(ArrayLitExpr *literal);

  SemanticState &state;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
};

}
