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
  void finishSentEmits();
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

  struct EmitterCall {
    CallExpr *call;
    MemberExpr *member;
    ClassDecl *cls;
    const Type *receiver;
    const Type *events;
    bool super;
  };

  struct EmitShape {
    const Type *args;
    std::string event;
    std::string first;
  };

  enum class ListenerForm { Free, Receiver, Captures, Unknown };

  struct ListenerShape {
    ListenerForm form;
    const Type *type;
    const VarSymbol *var;
    FunctionDecl *envFn;
    std::vector<const VarSymbol *> captured;
  };

  static ListenerShape listenerShape(Expr *e);
  static ListenerShape closureShape(FunctionDecl *fn);
  static ListenerShape functionShape(FunctionDecl *fn);
  bool addListenerReceiver(const ListenerShape &shape, bool topLevel, int &bits);
  bool reportsUnknownListener(const Expr *arg, bool timer);
  bool reportsCapturesKept(const Expr *arg, const std::vector<const VarSymbol *> &captured,
                           bool timer);
  static EmitterMethod emitterMethod(llvm::StringRef name);
  static std::optional<std::string> chainText(const Expr *expr);
  static std::optional<std::string> rootText(const Expr *expr);

  void noteListener(MemberExpr *member, const Type *events, llvm::StringRef event);
  void checkLooseArgs(CallExpr *call, size_t from);
  bool isOwnEmit(const EventSite *site, const SentEmit &emit) const;
  const Type *inferredEmitArgs(EventSite *site, const std::string &event, llvm::SMLoc loc,
                               std::string &first);
  Expr *collectEmitterCall(const SentEmit &emit, EventSite *site, const std::string &event,
                           bool library);
  std::optional<const Type *> emittedValueType(CallExpr *call, const EventRecord *record,
                                               const std::string &event);
  void finishEmit(const SentEmit &emit, const Type *args);
  bool hasWrongArity(const EmitterCall &emitter, unsigned min, unsigned max);
  Expr *checkEmit(const EmitterCall &emitter, bool library);
  Expr *checkSiteEmit(const EmitterCall &emitter, EventSite *site, bool library);
  Expr *checkEmitShape(const EmitterCall &emitter, const EmitShape &shape, bool library);
  bool emitValuesFit(CallExpr *call, const EmitShape &shape, std::vector<bool> &given);
  Expr *checkListenCall(const EmitterCall &emitter, EmitterMethod kind);
  bool addListenerBits(const EmitterCall &emitter, llvm::StringRef event, ListenerKind listener);
  Expr *checkListenersCall(const EmitterCall &emitter, EmitterMethod kind);
  Expr *checkCountCall(const EmitterCall &emitter);
  Expr *checkRemoveAllCall(const EmitterCall &emitter);
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
  MethodCallChecker *methodCallChecker = nullptr;
  GenericInstantiator *genericInstantiator = nullptr;
  GenericClassInstantiator *genericClassInstantiator = nullptr;
};

}
