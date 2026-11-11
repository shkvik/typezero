#pragma once

#include "compiler/program/library/builtin_names/global_member_table/global_member_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include <optional>

namespace tz {

class SemanticParts;
struct ScopeEntry;
struct ArrayMethodInfo;
class Scopes;
class NewChecker;
class UrlChecker;
class DateChecker;
class DictMethods;
class ExprChecker;
class HttpChecker;
class IntlChecker;
class JsonChecker;
class MathChecker;
class NameChecker;
class ArrayMethods;
class BytesChecker;
class LibraryCalls;
class MatchChecker;
class RegexChecker;
class TimerChecker;
class TupleChecker;
class UnionChecker;
class ConcatChecker;
class CryptoChecker;
class NumberMethods;
class ObjectChecker;
class ResultChecker;
class StaticChecker;
class StringMethods;
class TypeRelations;
class ArgListChecker;
class ConsoleChecker;
class DecimalChecker;
class GenericChecker;
class InheritChecker;
class ProcessChecker;
class PromiseChecker;
class StreamPipeline;
class UnknownChecker;
class WeakRefChecker;
class ArgumentChecker;
class IteratorMethods;
class NestedFunctions;
class TopLevelChecker;
class TypeArgInferrer;
class TzModulesChecker;
class WebCryptoChecker;
class DateFormatChecker;
class IntlMethodChecker;
class JsonStaticChecker;
class MethodCallChecker;
class SubtleCallChecker;
class EventTargetChecker;
class ReflectCallChecker;
class BufferStaticChecker;
class CryptoMethodChecker;
class ResultMethodChecker;
class BuiltinModuleChecker;
class PromiseStaticChecker;
class RuntimeObjectChecker;
class FunctionMethodChecker;
class GlobalFunctionChecker;

class CallChecker {
public:
  using ResultValueCallTyped = Result<Expr *, CallCheckerCheckValueCallError>;

  CallChecker(SemanticState &owner, SemanticParts &all);

  bool rejectSpread(CallExpr *call);
  Expr *checkCall(CallExpr *call, const Type *expected);
  bool isUnshadowedGlobal(const NameExpr *name) const;
  bool isGlobalName(const Expr *expr, GlobalName global) const;
  bool isGlobalMember(const MemberExpr *member, GlobalMember which) const;
  ResultValueCallTyped checkValueCall(CallExpr *call, const Type *fnType);
  Expr *checkDirectCall(CallExpr *call, FuncSymbol *func, llvm::SMLoc loc,
                        const Type *expected = nullptr);

private:
  using ResultNoSpreadPassed = Result<const CallExpr *, CallCheckerSpreadFreeError>;
  using ResultCalleeCallable = Result<const Type *, CallCheckerCalleeCallableError>;
  using ResultTypeArgsAbsent = Result<const MemberExpr *, CallCheckerNoTypeArgsError>;
  using ResultTypeArgsUsable = Result<const FuncSymbol *, CallCheckerDirectTypeArgsError>;

  bool namesGlobal(const Expr *expr, GlobalName global) const;
  bool isGlobalThisMember(const Expr *expr, GlobalName global) const;
  static ResultTypeArgsAbsent noTypeArgs(const MemberExpr *member);
  static ResultNoSpreadPassed spreadFree(const CallExpr *call, size_t i);
  static ResultCalleeCallable calleeCallable(const Type *type, const NameExpr *calleeName);
  static ResultTypeArgsUsable directTypeArgs(const FuncSymbol *func,
                                             llvm::ArrayRef<TypeRef> typeArgs);

  void rewriteSymbolCallee(CallExpr *call);
  void rejectTypeArgs(MemberExpr *member);
  void rejectGlobalTypeArgs(MemberExpr *member);
  Expr *checkCalleeValueCall(CallExpr *call);
  Expr *checkMapStaticCall(CallExpr *call, MemberExpr *member);
  Expr *checkListParamMethod(CallExpr *call, MemberExpr *member);
  Expr *checkIteratorFromCall(CallExpr *call, MemberExpr *member);
  Expr *checkNumberStaticCall(CallExpr *call, MemberExpr *member);
  Expr *checkLocaleMethod(CallExpr *call, MemberExpr *member);
  Expr *checkDictReceiverCall(CallExpr *call, MemberExpr *member);
  Expr *checkUnionReceiverCall(CallExpr *call, MemberExpr *member);
  Expr *checkTupleReceiverCall(CallExpr *call, MemberExpr *member);
  Expr *checkCollectionStaticCall(CallExpr *call, MemberExpr *member);
  Expr *tupleArrayCopy(MemberExpr *member, const ArrayMethodInfo *arrayMethod);
  std::optional<Expr *> checkTimerCall(CallExpr *call, NameExpr *name);
  std::optional<Expr *> checkGlobalFunctionCall(CallExpr *call, NameExpr *name);
  Expr *reportNoMethod(CallExpr *call, MemberExpr *member, const Type *receiver);
  Expr *checkNamedCall(CallExpr *call, NameExpr *name, const Type *expected);
  Expr *checkScopedCall(CallExpr *call, NameExpr *name, const Type *expected);
  Expr *reportNotCallable(CallExpr *call, NameExpr *name, const ScopeEntry &entry);
  Expr *checkMemberCall(CallExpr *call, MemberExpr *member, const Type *expected);
  Expr *checkReceiverCall(CallExpr *call, MemberExpr *member, const Type *expected);
  Expr *checkObjectStaticCall(CallExpr *call, MemberExpr *member, const Type *expected);
  Expr *checkClassReceiverCall(CallExpr *call, MemberExpr *member, const Type *expected);
  Expr *checkLanguageStaticCall(CallExpr *call, MemberExpr *member, const Type *expected);
  Expr *checkLibraryStaticCall(CallExpr *call, MemberExpr *member, const Type *expected);
  Expr *checkShapedReceiverCall(CallExpr *call, MemberExpr *member, const Type *expected);
  Expr *checkLibraryReceiverCall(CallExpr *call, MemberExpr *member, const Type *expected);
  Expr *checkPromiseReceiverCall(CallExpr *call, MemberExpr *member, const Type *expected);
  Expr *checkCallableReceiverCall(CallExpr *call, MemberExpr *member, const Type *expected);
  Expr *checkFunctionNameCall(CallExpr *call, NameExpr *name, FuncSymbol *func,
                              const Type *expected);
  Expr *checkValueReceiverCall(CallExpr *call, MemberExpr *member, const Type *receiver,
                               const Type *expected);
  Expr *checkPlainValueMethod(CallExpr *call, MemberExpr *member, const Type *receiver,
                              const Type *expected);

  SemanticState &state;
  Scopes *scopes = nullptr;
  NewChecker *newChecker = nullptr;
  UrlChecker *urlChecker = nullptr;
  DateChecker *dateChecker = nullptr;
  DictMethods *dictMethods = nullptr;
  ExprChecker *exprChecker = nullptr;
  HttpChecker *httpChecker = nullptr;
  IntlChecker *intlChecker = nullptr;
  JsonChecker *jsonChecker = nullptr;
  MathChecker *mathChecker = nullptr;
  NameChecker *nameChecker = nullptr;
  ArrayMethods *arrayMethods = nullptr;
  BytesChecker *bytesChecker = nullptr;
  LibraryCalls *libraryCalls = nullptr;
  MatchChecker *matchChecker = nullptr;
  RegexChecker *regexChecker = nullptr;
  TimerChecker *timerChecker = nullptr;
  TupleChecker *tupleChecker = nullptr;
  UnionChecker *unionChecker = nullptr;
  ConcatChecker *concatChecker = nullptr;
  CryptoChecker *cryptoChecker = nullptr;
  NumberMethods *numberMethods = nullptr;
  ObjectChecker *objectChecker = nullptr;
  ResultChecker *resultChecker = nullptr;
  StaticChecker *staticChecker = nullptr;
  StringMethods *stringMethods = nullptr;
  TypeRelations *typeRelations = nullptr;
  ArgListChecker *argListChecker = nullptr;
  ConsoleChecker *consoleChecker = nullptr;
  DecimalChecker *decimalChecker = nullptr;
  GenericChecker *genericChecker = nullptr;
  InheritChecker *inheritChecker = nullptr;
  ProcessChecker *processChecker = nullptr;
  PromiseChecker *promiseChecker = nullptr;
  StreamPipeline *streamPipeline = nullptr;
  UnknownChecker *unknownChecker = nullptr;
  WeakRefChecker *weakRefChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  IteratorMethods *iteratorMethods = nullptr;
  NestedFunctions *nestedFunctions = nullptr;
  TopLevelChecker *topLevelChecker = nullptr;
  TypeArgInferrer *typeArgInferrer = nullptr;
  TzModulesChecker *tzModulesChecker = nullptr;
  WebCryptoChecker *webCryptoChecker = nullptr;
  DateFormatChecker *dateFormatChecker = nullptr;
  IntlMethodChecker *intlMethodChecker = nullptr;
  JsonStaticChecker *jsonStaticChecker = nullptr;
  MethodCallChecker *methodCallChecker = nullptr;
  SubtleCallChecker *subtleCallChecker = nullptr;
  EventTargetChecker *eventTargetChecker = nullptr;
  ReflectCallChecker *reflectCallChecker = nullptr;
  BufferStaticChecker *bufferStaticChecker = nullptr;
  CryptoMethodChecker *cryptoMethodChecker = nullptr;
  ResultMethodChecker *resultMethodChecker = nullptr;
  BuiltinModuleChecker *builtinModuleChecker = nullptr;
  PromiseStaticChecker *promiseStaticChecker = nullptr;
  RuntimeObjectChecker *runtimeObjectChecker = nullptr;
  FunctionMethodChecker *functionMethodChecker = nullptr;
  GlobalFunctionChecker *globalFunctionChecker = nullptr;
};

}
