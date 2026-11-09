#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/regex/match_checker/match_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class ExprChecker;
class MemberChecker;
class ObjectChecker;
class TypeRelations;
class ConstantFolder;

class MatchChecker {
public:
  MatchChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkMatchMember(MemberExpr *member);
  Expr *checkMatchIndex(IndexExpr *index);
  Expr *matchItems(Expr *match);

private:
  using ResultGroupIndex = Result<int64_t, MatchCheckerGroupIndexError>;

  static ResultGroupIndex groupIndex(int64_t index, int groups);
  static const Type *itemsType();
  Expr *itemsMember(Expr *match);
  Expr *optionalItems(Expr *match, Expr *items);

  SemanticState &state;
  ExprChecker *exprChecker = nullptr;
  MemberChecker *memberChecker = nullptr;
  ObjectChecker *objectChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  ConstantFolder *constantFolder = nullptr;
};

}
