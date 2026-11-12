#pragma once

#include "compiler/semantic/library/events/emitter_checker/emitter_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class ClassLinks;
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
class ClassDeclarer;
class ReturnInferrer;
class ArgumentChecker;
class EventMapInference;
class MethodCallChecker;
class GenericInstantiator;
class GenericClassInstantiator;

class EmitterChecker {
public:
  EmitterChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkEmitterCall(CallExpr *call, MemberExpr *member, ClassDecl *classDecl, bool super);
  Expr *callLibraryMethod(CallExpr *call, MemberExpr *member, ClassDecl *classDecl,
                          llvm::StringRef name, llvm::ArrayRef<const Type *> bound,
                          const std::vector<bool> *given, bool super);
  void reportEmitterMethodValue(MemberExpr *member);
  Expr *checkEmitterNew(NewExpr *expr, ClassDecl *generic, const Type *expected);
  void finishSentEmits();
  void reportUnlistenedErrors();
  void walkErrorListeners();
  void walkDroppedListeners();
  void noteErrorsHandled(const Expr *emitter);
  bool classifyListener(Expr *arg, const Expr *emitter, int &bits, bool timer = false);
  bool ownsEmitter(const Expr *emitter) const;
  bool canReceive(const Type *type) const;

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

  ListenerShape listenerShape(Expr *expr) const;
  ListenerShape closureShape(FunctionDecl *functionDecl) const;
  ListenerShape functionShape(FunctionDecl *functionDecl) const;
  bool addListenerReceiver(const ListenerShape &shape, bool topLevel, int &bits);
  bool reportsUnknownListener(const Expr *arg, bool timer);
  bool reportsCapturesKept(const Expr *arg, const std::vector<const VarSymbol *> &captured,
                           bool timer);
  EmitterMethod emitterMethod(llvm::StringRef name) const;
  std::optional<std::string> chainText(const Expr *expr) const;
  std::optional<std::string> rootText(const Expr *expr) const;

  void noteListener(MemberExpr *member, const Type *events, llvm::StringRef event);
  void walkLooseArgs(CallExpr *call, size_t from);
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
  ClassLinks *classLinks = nullptr;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  LibraryCalls *libraryCalls = nullptr;
  TypeResolver *typeResolver = nullptr;
  TypeRelations *typeRelations = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  EventMapInference *eventMapInference = nullptr;
  MethodCallChecker *methodCallChecker = nullptr;
  GenericInstantiator *genericInstantiator = nullptr;
  GenericClassInstantiator *genericClassInstantiator = nullptr;
};

}
