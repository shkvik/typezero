#pragma once

#include "compiler/semantic/library/events/emitter_checker/emitter_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class Scopes;
class EventMaps;
class FieldFacts;
class TypeParams;
class CallChecker;
class ExprChecker;
class LibraryCalls;
class TypeResolver;
class TypeRelations;
class ReturnInferrer;
class ArgumentChecker;
class EventMapInference;
class KeyedFieldChecker;
class MethodCallChecker;
class GenericInstantiator;
class GenericClassInstantiator;

class EmitterChecker {
public:
  EmitterChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkEmitterCall(CallExpr *call, MemberExpr *member, ClassDecl *cls, bool super);
  Expr *callLibraryMethod(CallExpr *call, MemberExpr *member, ClassDecl *cls, llvm::StringRef name,
                          llvm::ArrayRef<const Type *> bound, const std::vector<bool> *given,
                          bool super);
  void reportEmitterMethodValue(MemberExpr *member);
  Expr *checkEmitterNew(NewExpr *expr, ClassDecl *generic, const Type *expected);
  Expr *collectEmitterCall(CallExpr *call, MemberExpr *member, const std::string &site);
  void reportUnlistenedErrors();
  void checkErrorListeners();
  void checkDroppedListeners();
  void noteErrorsHandled(const Expr *emitter);
  bool classifyListener(Expr *arg, const Expr *emitter, int &bits, bool timer = false);
  bool ownsEmitter(const Expr *emitter) const;
  static bool canReceive(const Type *type);

private:
  enum class EmitterMethod {
    None,
    Listen,
    Remove,
    Emit,
    Listeners,
    RawListeners,
    Count,
    RemoveAll
  };

  static EmitterMethod emitterMethod(llvm::StringRef name);
  static std::string chainText(const Expr *expr);
  static bool isServiceEvent(llvm::StringRef name);
  static std::string fileOfPlace(const std::string &place);
  static std::string shortPlace(const std::string &place);

  void noteListener(MemberExpr *member, const Type *events, llvm::StringRef event);
  EmitterKey keyOf(const Expr *root) const;
  std::string emitterPath(const EmitterKey &key) const;

  SemanticState &state;
  Scopes *scopes = nullptr;
  EventMaps *eventMaps = nullptr;
  FieldFacts *fieldFacts = nullptr;
  TypeParams *typeParams = nullptr;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  LibraryCalls *libraryCalls = nullptr;
  TypeResolver *typeResolver = nullptr;
  TypeRelations *typeRelations = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  EventMapInference *eventMapInference = nullptr;
  KeyedFieldChecker *keyedFieldChecker = nullptr;
  MethodCallChecker *methodCallChecker = nullptr;
  GenericInstantiator *genericInstantiator = nullptr;
  GenericClassInstantiator *genericClassInstantiator = nullptr;
};

}
