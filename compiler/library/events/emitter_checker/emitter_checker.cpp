#include "compiler/library/events/emitter_checker/emitter_checker.h"
#include "compiler/check/classes/class_links/class_links.h"
#include "compiler/check/classes/method_call_checker/method_call_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/flow/field_facts/field_facts.h"
#include "compiler/check/generics/generic_class_instantiator/generic_class_instantiator.h"
#include "compiler/check/generics/generic_instantiator/generic_instantiator.h"
#include "compiler/check/generics/type_params/type_params.h"
#include "compiler/check/objects/keyed_field_checker/keyed_field_checker.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/library/events/event_map_inference/event_map_inference.h"
#include "compiler/library/events/event_maps/event_maps.h"
#include "compiler/library/tz_modules/library_calls/library_calls.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/Path.h"
#include <cassert>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

enum class EmitterMethod {
  None,
  Listen,
  Remove,
  Emit,
  Listeners,
  RawListeners,
  Count,
  RemoveAll,
  Chain
};

EmitterMethod emitterMethod(llvm::StringRef name) {
  if (name == "on" || name == "addListener" || name == "prependListener" || name == "once" ||
      name == "prependOnceListener") {
    return EmitterMethod::Listen;
  }
  if (name == "off" || name == "removeListener") {
    return EmitterMethod::Remove;
  }
  if (name == "emit") {
    return EmitterMethod::Emit;
  }
  if (name == "listeners") {
    return EmitterMethod::Listeners;
  }
  if (name == "rawListeners") {
    return EmitterMethod::RawListeners;
  }
  if (name == "listenerCount") {
    return EmitterMethod::Count;
  }
  if (name == "removeAllListeners") {
    return EmitterMethod::RemoveAll;
  }
  if (name == "setMaxListeners") {
    return EmitterMethod::Chain;
  }

  return EmitterMethod::None;
}

std::string chainText(const Expr *expr) {
  if (auto *name = dyn_cast<NameExpr>(expr)) {
    return name->name.str();
  }
  if (auto *call = dyn_cast<CallExpr>(expr); call && isa<NameExpr>(call->callee)) {
    llvm::StringRef fn = cast<NameExpr>(call->callee)->name;
    auto *fd = call->args.size() == 1 ? dyn_cast<IntLitExpr>(call->args[0]) : nullptr;
    if (fn == "processStdin" && call->args.empty()) {
      return "process.stdin";
    }
    if (fn == "processStdout" && fd) {
      return fd->value == 2 ? "process.stderr" : "process.stdout";
    }
    return "";
  }
  if (auto *member = dyn_cast<MemberExpr>(expr); member && !member->optional) {
    std::string object = chainText(member->object);
    return object.empty() ? "" : object + "." + member->name.str();
  }

  return "";
}

bool isServiceEvent(llvm::StringRef name) {
  return name == "newListener" || name == "removeListener";
}

std::string fileOfPlace(const std::string &place) {
  size_t column = place.rfind(':');
  size_t line =
      column == std::string::npos || column == 0 ? std::string::npos : place.rfind(':', column - 1);

  return line == std::string::npos ? place : place.substr(0, line);
}

std::string shortPlace(const std::string &place) {
  std::string file = fileOfPlace(place);

  return llvm::sys::path::filename(file).str() + place.substr(file.size());
}

}

EmitterChecker::EmitterChecker(Checker &checker) : checker_(checker) {}

void EmitterChecker::noteListener(MemberExpr *member, const Type *events, llvm::StringRef event) {
  if (event == "error") {
    checker_.errorListenedMaps_.insert(events);
  }
  if (events->isRecord() || (event != "error" && !events->field("error"))) {
    return;
  }
  const Expr *root = member->object;
  while (true) {
    while (auto *conv = dyn_cast<CastExpr>(root)) {
      if (!conv->isImplicit()) {
        break;
      }
      root = conv->operand;
    }
    auto *inner = dyn_cast<CallExpr>(root);
    auto *chained = inner ? dyn_cast<MemberExpr>(inner->callee) : nullptr;
    if (!chained || emitterMethod(chained->name) != EmitterMethod::Listen) {
      break;
    }
    root = chained->object;
  }
  Checker::EmitterKey key = keyOf(root);
  Checker::Flow &flow = checker_.flow_;
  if (event == "error") {
    if (!llvm::is_contained(flow.errorListened, key)) {
      flow.errorListened.push_back(key);
    }
    llvm::erase_if(flow.errorPending, [&](const auto &p) { return p.first == key; });
    return;
  }
  if (!llvm::is_contained(flow.errorListened, key) &&
      llvm::none_of(flow.errorPending, [&](const auto &p) { return p.first == key; })) {
    flow.errorPending.push_back({key, member->loc()});
  }
}

Checker::EmitterKey EmitterChecker::keyOf(const Expr *root) const {
  while (auto *conv = dyn_cast<CastExpr>(root)) {
    if (!conv->isImplicit()) {
      break;
    }
    root = conv->operand;
  }
  Checker::EmitterKey key;
  if (auto *name = dyn_cast<NameExpr>(root)) {
    const VarSymbol *var = name->var ? name->var : checker_.scopes().lookup(name->name).var;
    if (var) {
      key.path.var = var;
      return key;
    }
  }
  if (std::optional<Checker::FieldPath> path = checker_.fieldFacts().fieldPath(root)) {
    key.path = *path;
  } else if (std::string text = chainText(root); !text.empty()) {
    key.text = text;
  } else {
    key.root = root;
  }

  return key;
}

void EmitterChecker::noteErrorsHandled(const Expr *emitter) {
  if (emitter->type) {
    if (const Type *events = checker_.eventMaps().eventMapOf(emitter->type)) {
      checker_.errorListenedMaps_.insert(events);
    }
  }
  Checker::EmitterKey key = keyOf(emitter);
  Checker::Flow &flow = checker_.flow_;
  if (!llvm::is_contained(flow.errorListened, key)) {
    flow.errorListened.push_back(key);
  }
  llvm::erase_if(flow.errorPending, [&](const auto &p) { return p.first == key; });
}

std::string EmitterChecker::emitterName(const Checker::EmitterKey &key) const {
  if (!key.text.empty()) {
    return "'" + key.text + "'";
  }
  if (!key.path.var) {
    return "this emitter";
  }
  std::string name = key.path.var->name.str();
  for (llvm::StringRef part : key.path.names) {
    name += "." + part.str();
  }

  return "'" + name + "'";
}

void EmitterChecker::reportUnlistenedErrors() {
  for (const auto &[key, loc] : checker_.flow_.errorPending) {
    if (!checker_.reportedErrorSubscriptions_.insert(loc.getPointer()).second) {
      continue;
    }
    std::string name = emitterName(key);
    std::string shown = key.path.var || !key.text.empty() ? name.substr(1, name.size() - 2) : "s";
    checker_.diags_.error(loc, name +
                                   " may emit 'error', and nothing listens for it on every path "
                                   "here, so the error would be lost (node would end the "
                                   "program); listen in this function: '" +
                                   shown + ".on(\"error\", (e) => ...)'");
  }
  checker_.flow_.errorPending.clear();
}

void EmitterChecker::checkErrorListeners() {
  for (const auto &[events, loc] : checker_.errorEmits_) {
    if (!checker_.errorListenedMaps_.contains(events)) {
      checker_.diags_.error(loc, "'error' is emitted here, but nothing in the program listens for "
                                 "'error' on this emitter, so the error would be lost (node "
                                 "would end the program); listen for it, e.g. "
                                 "'bus.on(\"error\", (e) => ...)'");
    }
  }
}

Expr *EmitterChecker::checkEmitterNew(NewExpr *expr, ClassDecl *generic, const Type *expected) {
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : expr->args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }

    return expr;
  };
  const Type *events = nullptr;
  if (!expr->typeArgs.empty()) {
    if (expr->typeArgs.size() == 1) {
      events = checker_.typeResolver().resolveType(expr->typeArgs[0], false);
    }
  } else if (const Type *want = expected ? expected->nonOptional() : nullptr;
             want && ClassDeclarer::classOf(want) &&
             ClassDeclarer::classOf(want)->generic == generic) {
    events = ClassDeclarer::classOf(want)->typeArgs[0];
  } else {
    events = checker_.eventMapInference().inferredEventMap(
        "new " + checker_.eventMapInference().placeOf(expr->loc()));
    if (!events) {
      checker_.diags_.error(expr->loc(), "'new EventEmitter()' needs its event map here, e.g. 'new "
                                         "EventEmitter<{ data: [chunk: string]; end: [] }>()'");
      return checkArgsLoosely();
    }
  }
  if (!events) {
    return nullptr;
  }
  if (events->isError()) {
    return checkArgsLoosely();
  }
  ClassDecl *inst =
      checker_.genericClassInstantiator().instantiateClass(generic, {events}, expr->loc());
  if (!inst) {
    return checkArgsLoosely();
  }
  expr->typeArgs.clear();
  return checker_.methodCallChecker().checkClassNew(expr, inst, expected);
}

Expr *EmitterChecker::collectEmitterCall(CallExpr *call, MemberExpr *member,
                                         const std::string &site) {
  call->type = Type::boolTy();
  member->type = Type::error();
  llvm::SMLoc loc = call->args.empty() ? member->loc() : call->args[0]->loc();
  std::vector<const Type *> elements;
  bool known = true;
  for (size_t i = 0; i < call->args.size(); ++i) {
    call->args[i] = i == 0 ? checker_.argumentChecker().checkArg(call->args[i], Type::string())
                           : checker_.exprChecker().checkExpr(call->args[i]);
    const Type *type = call->args[i]->type;
    if (type->isError() || type->isVoid() || mentionsParam(type)) {
      known = false;
    }
    if (i > 0) {
      elements.push_back(type->isLiteral() ? type->literalBase() : type);
    }
  }
  std::optional<std::string> key =
      call->args.empty() ? std::nullopt : checker_.keyedFieldChecker().constantKey(call->args[0]);
  if (!key || !known || isServiceEvent(*key)) {
    return call;
  }
  const Type *found = Type::tuple(elements);
  std::vector<EventInference::Event> &events = checker_.inference_->maps[site];
  for (EventInference::Event &e : events) {
    if (e.name != *key) {
      continue;
    }
    const Type *before = checker_.eventMapInference().fromPortable(e.args, loc);
    if (!before || before == found || before->fields().size() != elements.size()) {
      return call;
    }
    std::vector<const Type *> wider;
    for (size_t i = 0; i < elements.size(); ++i) {
      const Type *both = TypeRelations::unify(before->fields()[i].type, elements[i]);
      if (!both) {
        return call;
      }
      wider.push_back(both);
    }
    if (const Type *now = Type::tuple(wider); now != before) {
      llvm::SmallPtrSet<const Type *, 8> seen;
      e.args = checker_.eventMapInference().portable(now, seen);
      checker_.inference_->changed = true;
    }
    return call;
  }
  llvm::SmallPtrSet<const Type *, 8> seen;
  events.push_back({*key, checker_.eventMapInference().portable(found, seen),
                    shortPlace(checker_.eventMapInference().placeOf(loc))});
  checker_.inference_->changed = true;

  return call;
}

Expr *EmitterChecker::callLibraryMethod(CallExpr *call, MemberExpr *member, ClassDecl *cls,
                                        llvm::StringRef name, llvm::ArrayRef<const Type *> bound,
                                        const std::vector<bool> *given, bool super) {
  std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(cls, name, false);
  assert(!methods.empty() && "a method of the emitter's code");
  FuncSymbol *sym = methods[0]->sym;
  checker_.returnInferrer().resolveParams(sym);
  llvm::SMLoc loc = member->loc();
  std::vector<const Type *> params = sym->params;
  const Type *ret = checker_.returnInferrer().returnType(sym);
  FuncSymbol *callee = sym;
  if (sym->isGeneric()) {
    llvm::SMLoc outerLoc = std::exchange(checker_.instanceLoc_, loc);
    for (const Type *&p : params) {
      p = checker_.typeParams().substitute(p, sym->typeParams, bound);
    }
    ret = ret ? checker_.typeParams().substitute(ret, sym->typeParams, bound) : Type::error();
    checker_.instanceLoc_ = outerLoc;
    if (!checker_.trial_ && llvm::none_of(bound, [](const Type *t) { return mentionsParam(t); })) {
      if (FuncSymbol *instance = checker_.genericInstantiator().instantiate(sym, bound, loc)) {
        callee = instance;
      }
    }
  }
  call->superCall = super;
  if (sym->isGeneric() || super) {
    call->method = callee;
  } else if (FunctionDecl *impl = ClassLinks::soleImplementation(cls, sym->decl)) {
    call->method = impl->sym;
  } else {
    call->method = sym;
    call->virtualCall = true;
  }
  member->type = checker_.returnInferrer().functionType(callee);
  call->type = ret ? ret : Type::error();
  if (callee != sym) {
    checker_.returnInferrer().resolveParams(callee);
    return checker_.argumentChecker().checkArguments(call, callee->params, callee->minArgs,
                                                     callee->hasRest, loc, given);
  }

  return checker_.argumentChecker().checkArguments(call, params, sym->minArgs, sym->hasRest, loc,
                                                   given);
}

Expr *EmitterChecker::checkEmitterCall(CallExpr *call, MemberExpr *member, ClassDecl *cls,
                                       bool super) {
  EmitterMethod kind = emitterMethod(member->name);
  if (kind == EmitterMethod::None || checker_.libraryCalls().inLibrary(call->loc())) {
    return nullptr;
  }
  std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(cls, member->name, false);
  if (!methods.empty() && !methods[0]->cls->library) {
    return nullptr;
  }
  const Type *receiver = member->object->type;
  const Type *events = checker_.eventMaps().eventMapOf(receiver);
  if (!events) {
    return nullptr;
  }
  call->type = Type::error();
  member->type = Type::error();
  if (!member->typeArgs.empty()) {
    checker_.diags_.error(member->loc(), "'" + member->name +
                                             "' takes no type arguments: the event map of "
                                             "the emitter gives the types");
    member->typeArgs.clear();
  }
  if (checker_.callChecker().rejectSpread(call)) {
    return call;
  }
  auto checkArgsLoosely = [&](size_t from) {
    for (size_t i = from; i < call->args.size(); ++i) {
      call->args[i] = checker_.exprChecker().checkExpr(call->args[i]);
    }
  };
  auto flag = [&](bool value) {
    return checker_.exprChecker().checkExpr(checker_.ctx_.make<BoolLitExpr>(call->loc(), value));
  };
  llvm::StringRef name = member->name;
  std::string event;
  switch (kind) {
  case EmitterMethod::Emit: {
    if (call->args.empty()) {
      checker_.diags_.error(member->loc(),
                            "'emit' needs the name of the event, e.g. 'emit(\"data\", ...)'");
      return call;
    }
    if (auto open = checker_.openMaps_.find(events); open != checker_.openMaps_.end()) {
      return collectEmitterCall(call, member, open->second);
    }
    const Type *args = checker_.eventMaps().eventArgs(events, call->args[0], name, &event);
    if (!args) {
      checkArgsLoosely(1);
      return call;
    }
    size_t count = call->args.size() - 1;
    size_t min = args->tupleMin();
    size_t max = args->tupleRest() ? ~size_t(0) : args->fields().size();
    std::string first;
    if (auto inferred = checker_.inferredMaps_.find(events);
        inferred != checker_.inferredMaps_.end()) {
      for (const EventInference::Event &e : checker_.inference_->maps[inferred->second]) {
        if (e.name == event) {
          first = e.where;
        }
      }
    }
    if (count < min || count > max) {
      if (!first.empty()) {
        checker_.diags_.error(member->loc(), "'" + event + "' is emitted with " +
                                                 args->name().str() + " at " + first +
                                                 ", but with " + std::to_string(count) +
                                                 " argument" + (count == 1 ? "" : "s") + " here");
      } else {
        checker_.diags_.error(member->loc(),
                              "the listeners of '" + event + "' get '" + args->name().str() +
                                  "': " + argumentCount(unsigned(min), unsigned(max)) +
                                  ", but got " + std::to_string(count));
      }
      checkArgsLoosely(1);
      return call;
    }
    if (event == "error") {
      checker_.errorEmits_.push_back({events, member->loc()});
    }
    std::vector<bool> given(call->args.size(), false);
    given[0] = true;
    if (!first.empty()) {
      for (size_t i = 1; i < call->args.size() && i - 1 < args->fields().size(); ++i) {
        const Type *wanted = args->fields()[i - 1].type;
        call->args[i] = checker_.exprChecker().checkExpr(call->args[i], wanted);
        const Type *type = call->args[i]->type;
        if (!type->isError() && !TypeRelations::isImplicitlyConvertible(type, wanted)) {
          checker_.diags_.error(call->args[i]->loc(),
                                "'" + event + "' is emitted with " + args->name().str() + " at " +
                                    first + ", but with '" + type->name().str() +
                                    "' here; an event has one type of arguments");
          checkArgsLoosely(i + 1);
          return call;
        }
        call->args[i] = checker_.typeRelations().coerce(call->args[i], wanted);
        given[i] = true;
      }
    }
    return callLibraryMethod(call, member, cls, "emit", {args}, &given, super);
  }
  case EmitterMethod::Listen:
  case EmitterMethod::Remove: {
    if (!checker_.argumentChecker().checkArity(call, member->loc(), 2, 2)) {
      return call;
    }
    const Type *args = checker_.eventMaps().eventArgs(events, call->args[0], name, &event);
    if (!args) {
      checkArgsLoosely(1);
      return call;
    }
    Checker::ListenerKind listener = Checker::ListenerKind::Plain;
    call->args[1] =
        checker_.eventMaps().checkListener(call->args[1], args, event, receiver, listener);
    if (call->args[1]->type->isError()) {
      return call;
    }
    if (kind == EmitterMethod::Listen) {
      noteListener(member, events, event);
      call->args.push_back(checker_.exprChecker().checkExpr(
          checker_.ctx_.make<IntLitExpr>(call->loc(), uint64_t(listener)), Type::intTy()));
    } else {
      checker_.file_->comparesFunctions = true;
    }
    std::vector<bool> given(call->args.size(), true);
    callLibraryMethod(call, member, cls, name, {}, &given, super);
    call->type = receiver;
    return call;
  }
  case EmitterMethod::Listeners:
  case EmitterMethod::RawListeners: {
    if (!checker_.argumentChecker().checkArity(call, member->loc(), 1, 1)) {
      return call;
    }
    const Type *args = checker_.eventMaps().eventArgs(events, call->args[0], name, &event);
    if (!args) {
      return call;
    }
    call->args.push_back(flag(kind == EmitterMethod::RawListeners));
    std::vector<bool> given(call->args.size(), true);
    callLibraryMethod(call, member, cls, "#listenersOf", {args}, &given, super);
    call->type = Type::array(EventMaps::listenerType(args, Type::voidTy()));
    return call;
  }
  case EmitterMethod::Count: {
    if (!checker_.argumentChecker().checkArity(call, member->loc(), 1, 2)) {
      return call;
    }
    const Type *args = checker_.eventMaps().eventArgs(events, call->args[0], name, &event);
    if (!args) {
      checkArgsLoosely(1);
      return call;
    }
    if (call->args.size() == 2) {
      Checker::ListenerKind listener = Checker::ListenerKind::Plain;
      call->args[1] =
          checker_.eventMaps().checkListener(call->args[1], args, event, receiver, listener);
      if (call->args[1]->type->isError()) {
        return call;
      }
      checker_.file_->comparesFunctions = true;
    }
    std::vector<bool> given(call->args.size(), true);
    return callLibraryMethod(call, member, cls, name, {}, &given, super);
  }
  case EmitterMethod::RemoveAll: {
    if (!checker_.argumentChecker().checkArity(call, member->loc(), 0, 1)) {
      return call;
    }
    if (!call->args.empty() &&
        !checker_.eventMaps().eventArgs(events, call->args[0], name, &event)) {
      return call;
    }
    std::vector<bool> given(call->args.size(), true);
    callLibraryMethod(call, member, cls, name, {}, &given, super);
    call->type = receiver;
    return call;
  }
  case EmitterMethod::Chain:
    callLibraryMethod(call, member, cls, name, {}, nullptr, super);
    if (!call->type->isError()) {
      call->type = receiver;
    }
    return call;
  case EmitterMethod::None:
    break;
  }

  return nullptr;
}

std::string EmitterChecker::libraryExportHint(llvm::StringRef name) {
  if (name == "errorMonitor" || name == "captureRejectionSymbol") {
    return ": in node it is a symbol, and tz has no symbols of its own; the names of events are "
           "strings (docs/events.md, section 4)";
  }
  if (name == "defaultMaxListeners") {
    return "; it is 'EventEmitter.defaultMaxListeners'";
  }
  if (name == "addAbortListener") {
    return " yet; listen to the signal: 'signal.addEventListener(\"abort\", listener, { once: "
           "true })'";
  }
  if (name == "EventEmitterAsyncResource" || name == "usingDomains") {
    return ": tz has neither async_hooks nor domains";
  }
  if (name == "EventTarget" || name == "Event" || name == "CustomEvent") {
    return "; '" + name.str() + "' is global, as in node: use it without an import";
  }

  return "";
}

void EmitterChecker::reportEmitterMethodValue(MemberExpr *member) {
  checker_.diags_.error(member->loc(),
                        "'" + member->name +
                            "' of an emitter can only be called: the event "
                            "map checks each call by the name of its event; wrap it in an "
                            "arrow, e.g. '(listener) => bus." +
                            member->name + "(\"data\", listener)'");
}

}
