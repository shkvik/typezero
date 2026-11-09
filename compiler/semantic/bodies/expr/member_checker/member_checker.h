#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/expr/member_checker/member_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class Scopes;
class CallChecker;
class DateChecker;
class DictChecker;
class ExprChecker;
class IndexProofs;
class IntlChecker;
class JsonChecker;
class BytesChecker;
class MatchChecker;
class RegexChecker;
class TupleChecker;
class TypeDeclarer;
class UnionChecker;
class UtilityTypes;
class CryptoChecker;
class ObjectChecker;
class ResultChecker;
class StaticChecker;
class TypeRelations;
class ArgListChecker;
class ConstantFolder;
class GenericChecker;
class ProcessChecker;
class UnknownChecker;
class OverloadResolver;
class KeyedFieldChecker;
class ClassMemberChecker;
class BuiltinModuleChecker;
class RuntimeObjectChecker;

class MemberChecker {
public:
  using ResultIndexedElement = Result<Expr *, MemberCheckerCheckIndexError>;
  using ResultMemberReadable = Result<Expr *, MemberCheckerCheckMemberError>;

  MemberChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkPosition(Expr *expr);
  ResultIndexedElement checkIndex(IndexExpr *index);
  ResultMemberReadable checkMember(MemberExpr *member, const Type *expected);

private:
  using ResultIndexIsInteger = Result<const Type *, MemberCheckerPositionTypeError>;

  static ResultIndexIsInteger positionType(const Type *type);

  SemanticState &state;
  Scopes *scopes = nullptr;
  CallChecker *callChecker = nullptr;
  DateChecker *dateChecker = nullptr;
  DictChecker *dictChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  IndexProofs *indexProofs = nullptr;
  IntlChecker *intlChecker = nullptr;
  JsonChecker *jsonChecker = nullptr;
  BytesChecker *bytesChecker = nullptr;
  MatchChecker *matchChecker = nullptr;
  RegexChecker *regexChecker = nullptr;
  TupleChecker *tupleChecker = nullptr;
  TypeDeclarer *typeDeclarer = nullptr;
  UnionChecker *unionChecker = nullptr;
  UtilityTypes *utilityTypes = nullptr;
  CryptoChecker *cryptoChecker = nullptr;
  ObjectChecker *objectChecker = nullptr;
  ResultChecker *resultChecker = nullptr;
  StaticChecker *staticChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  ArgListChecker *argListChecker = nullptr;
  ConstantFolder *constantFolder = nullptr;
  GenericChecker *genericChecker = nullptr;
  ProcessChecker *processChecker = nullptr;
  UnknownChecker *unknownChecker = nullptr;
  OverloadResolver *overloadResolver = nullptr;
  KeyedFieldChecker *keyedFieldChecker = nullptr;
  ClassMemberChecker *classMemberChecker = nullptr;
  BuiltinModuleChecker *builtinModuleChecker = nullptr;
  RuntimeObjectChecker *runtimeObjectChecker = nullptr;
};

}
