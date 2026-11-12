#pragma once

#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/values/global_function_checker/global_function_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class FlowChecker;
class SemanticParts;
class Prelude;
class CallChecker;
class DateChecker;
class ExprChecker;
class MathChecker;
class RegexChecker;
class TruthChecker;
class BinaryChecker;
class ConcatChecker;
class TypeRelations;
class DecimalChecker;
class ArgumentChecker;

class GlobalFunctionChecker {
public:
  using ResultGlobalCallMade = Result<Expr *, GlobalFunctionCheckerCheckGlobalCallError>;
  using ResultFunctionCalled = Result<Expr *, GlobalFunctionCheckerCheckGlobalFunctionError>;

  GlobalFunctionChecker(SemanticState &owner, SemanticParts &all);

  ResultGlobalCallMade checkGlobalCall(CallExpr *call, NameExpr *name);
  ResultFunctionCalled checkGlobalFunction(CallExpr *call, llvm::StringRef functionName,
                                           llvm::SMLoc loc);

private:
  using ResultNumberSource = Result<const Type *, GlobalFunctionCheckerCheckGlobalFunctionError>;

  Expr *regExpCall(CallExpr *call, const NameExpr *name);
  bool isGlobalFunction(const NameExpr *name) const;
  GlobalFunctionCheckerCheckGlobalCallErrorCode
  globalCallCode(GlobalFunctionCheckerCheckGlobalFunctionErrorCode code) const;
  CallExpr *soleCallArg(const CallExpr *call) const;
  ResultFunctionCalled checkNotLegacyEscape(CallExpr *call, std::optional<GlobalName> global);
  std::optional<Builtin> uriBuiltin(std::optional<GlobalName> global) const;
  ResultFunctionCalled checkUriCall(CallExpr *call, std::optional<GlobalName> global,
                                    Builtin builtin);
  ResultFunctionCalled rejectDecodedEscape(CallExpr *escaped);
  const Type *uriResultType(Builtin builtin);
  ResultFunctionCalled reportBigInt(CallExpr *call, llvm::StringRef functionName, llvm::SMLoc loc);
  ResultFunctionCalled checkBooleanCall(CallExpr *call);
  ResultFunctionCalled checkStringCall(CallExpr *call);
  ResultFunctionCalled checkNumberCall(CallExpr *call);
  ResultNumberSource checkNumberSource(const Type *type) const;
  ResultFunctionCalled numberOf(CallExpr *call);
  ResultFunctionCalled checkParseCall(CallExpr *call, std::optional<GlobalName> global);

  SemanticState &state;
  Prelude *prelude = nullptr;
  CallChecker *callChecker = nullptr;
  DateChecker *dateChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  MathChecker *mathChecker = nullptr;
  FlowChecker *flowChecker = nullptr;
  RegexChecker *regexChecker = nullptr;
  TruthChecker *truthChecker = nullptr;
  BinaryChecker *binaryChecker = nullptr;
  ConcatChecker *concatChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  DecimalChecker *decimalChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
};

}
