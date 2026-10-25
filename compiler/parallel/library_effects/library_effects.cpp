#include "compiler/parallel/library_effects/library_effects.h"
#include "compiler/parallel/code_walk/code_walk.h"
#include "compiler/parallel/expr_walk/expr_walk.h"
#include "compiler/parallel/call_walk/call_walk.h"
#include "compiler/parallel/alias_analysis/alias_analysis.h"
#include "compiler/parallel/call_resolver/call_resolver.h"
#include "compiler/parallel/pieces/pieces.h"
#include "compiler/parallel/conflicts/conflicts.h"
#include "compiler/parallel/explainer/explainer.h"
#include "compiler/parallel/parallel_helpers/parallel_helpers.h"
#include "compiler/library/crypto/crypto_api/crypto_api.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSet.h"
#include <algorithm>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include "compiler/parallel/parallel_model/parallel_model.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz::parallel {

namespace {

// The emitters of node:events, the event targets and the abort signals.
bool isEmitterClass(llvm::StringRef cls) {
  return cls == "EventEmitter" || cls == "EventEmitterBase";
}

bool isTargetClass(llvm::StringRef cls) { return cls == "EventTarget" || cls == "AbortSignal"; }

bool isSignalClass(llvm::StringRef cls) { return cls == "AbortSignal" || cls == "AbortController"; }

} // namespace

LibraryEffects::LibraryEffects(Analysis &analysis) : analysis_(analysis) {}

bool LibraryEffects::inLibrary(llvm::SMLoc loc) const {
  const char *at = loc.getPointer();

  return llvm::any_of(analysis_.libRanges_, [&](const std::pair<const char *, const char *> &range) {
    return at >= range.first && at < range.second;
  });
}

const FunctionDecl *LibraryEffects::libraryTarget(const CallExpr *c) const {
  if (c->func && analysis_.isLibrary(c->func->decl)) {
    return c->func->decl;
  }
  if (c->method && analysis_.isLibrary(c->method->decl)) {
    return c->method->decl;
  }

  return nullptr;
}

void LibraryEffects::libraryCall(const CallExpr *c, const FunctionDecl *decl, const Use &use) {
  auto *m = dyn_cast<MemberExpr>(c->callee);
  bool superCtor = c->superCall && !m;
  std::optional<Ref> self;
  if (superCtor) {
    if (analysis_.fn_->thisOwner) {
      self = Ref{RefKind::This, analysis_.fn_->thisOwner};
    }
  } else if (m && c->method) {
    analysis_.exprWalk().expr(m->object);
    self = analysis_.aliasAnalysis().receiverRef(m->object);
  }
  llvm::StringRef cls = libraryClassName(decl->cls);
  llvm::StringRef name = decl->name;
  llvm::SMLoc at = c->loc();
  for (const Expr *arg : c->args) {
    libraryArg(c, cls, name, arg, at);
  }
  if (!decl->isAsync) {
    analysis_.codeWalk().throwPoint();
  }
  CallRec &rec = analysis_.codeWalk().record(c, CallKind::Call, use, at);
  rec.call = c;
  if (superCtor) {
    // The initializers of the fields of the program's classes that extend it.
    for (const ClassDecl *k = analysis_.fn_->decl ? analysis_.fn_->decl->cls : nullptr; k; k = k->parent) {
      if (k->init) {
        analysis_.callWalk().addCallee(rec, k->init, self);
      }
    }
  } else if (c->method && c->virtualCall) {
    // The methods of the program's classes that override it.
    for (const FunctionDecl *fn : analysis_.callWalk().methodTargets(decl, true)) {
      analysis_.callWalk().addCallee(rec, fn, self);
    }
  }
  if (self && !superCtor) {
    libraryState(cls, name, self, decl->accessor != FunctionDecl::Accessor::Getter, at);
  }
  if (isEmitterClass(cls) && name == "emit") {
    dispatch(c, CallKind::Inline, EmitterListeners, use);
  } else if ((isTargetClass(cls) && name == "dispatchEvent") ||
             (cls == "AbortController" && name == "abort")) {
    dispatch(c, CallKind::Inline, TargetListeners, use);
  } else if (cls == "AbortSignal" && name == "timeout") {
    dispatch(c, CallKind::Later, TargetListeners, use);
  }
}

void LibraryEffects::libraryNew(const NewExpr *n) {
  llvm::SMLoc at = n->loc();
  for (const Expr *arg : n->ctor ? n->ctor->args : n->args) {
    libraryArg(n->ctor, libraryClassName(n->cls), "constructor", arg, at);
  }
  analysis_.codeWalk().throwPoint();
  // A new object, which only this expression has.
}

// What a function of a built-in module does with one of its arguments.
void LibraryEffects::libraryArg(const CallExpr *c, llvm::StringRef cls, llvm::StringRef name,
                          const Expr *arg, llvm::SMLoc at) {
  const Expr *f = strip(arg);
  // A listener is given as a value of any type: `__tz_listener(f)`.
  if (auto *wrap = dyn_cast<CallExpr>(f);
      wrap && wrap->builtin == Builtin::LibListener && !wrap->args.empty()) {
    f = strip(wrap->args[0]);
  }
  bool fn = f->type && f->type->nonOptional()->isFunction();
  if (fn) {
    if (isEmitterClass(cls) && (name == "on" || name == "addListener" || name == "once" ||
                                name == "prependListener" || name == "prependOnceListener")) {
      listen(f, EmitterListeners);
    } else if (isTargetClass(cls) && (name == "addEventListener" || name == "addFunctionListener" ||
                                      name == "setOnabort")) {
      listen(f, TargetListeners);
    } else if (cls.empty() && name == "serve") {
      libraryCallback(c, f, CallKind::Later, /*interval=*/true, /*handled=*/true);
    } else {
      libraryCallback(c, f, CallKind::Inline);
      libraryCallback(c, f, CallKind::Later);
    }
    return;
  }
  // The functions of an object literal given: the source of a stream, the
  // options of a server.
  if (auto *lit = dyn_cast<ObjectLitExpr>(f)) {
    for (const ObjectLitExpr::Property &p : lit->props) {
      const Expr *v = strip(p.value);
      if (p.spread || !v || !v->type || !v->type->nonOptional()->isFunction()) {
        analysis_.exprWalk().expr(p.value);
        libraryReads(v, at);
        continue;
      }
      if (cls == "ReadableStream" && p.name == "start") {
        libraryCallback(c, v, CallKind::Inline);
      } else if (cls == "ReadableStream" && p.name == "pull") {
        libraryCallback(c, v, CallKind::Later, /*interval=*/true);
      } else if (cls == "ReadableStream" && p.name == "cancel") {
        libraryCallback(c, v, CallKind::Later);
      } else if (cls.empty() && name == "serve" && p.name == "onListen") {
        libraryCallback(c, v, CallKind::Inline);
      } else if (isTargetClass(cls) &&
                 (name == "addEventListener" || name == "addObjectListener") &&
                 p.name == "handleEvent") {
        listen(v, TargetListeners);
      } else {
        libraryCallback(c, v, CallKind::Inline);
        libraryCallback(c, v, CallKind::Later);
      }
    }
    return;
  }
  analysis_.exprWalk().expr(arg);
  libraryReads(f, at);
}

// What a function of a built-in module reads of a value it is given: the
// state of an object of one of their classes, or else all it holds.
void LibraryEffects::libraryReads(const Expr *e, llvm::SMLoc at) {
  const Type *type = e && e->type ? e->type->nonOptional() : nullptr;
  if (const ClassDecl *cls = type ? type->classDecl() : nullptr; cls && cls->library) {
    libraryState(libraryClassName(cls), "", analysis_.aliasAnalysis().refOf(e), false, at);
    return;
  }
  analysis_.exprWalk().contents(e, false, at);
}

void LibraryEffects::libraryCallback(const CallExpr *c, const Expr *f, CallKind kind, bool interval,
                               bool handled) {
  CallRec &rec = analysis_.codeWalk().record(f, kind, {}, f->loc());
  rec.call = c;
  rec.interval |= interval;
  rec.handled |= handled;
  if (kind == CallKind::Inline) {
    rec.repeated = true;
  }
  analysis_.callWalk().addFunctionCallees(f, f, kind);
}

// A listener kept, which a call of `channel` runs.
void LibraryEffects::listen(const Expr *f, Channel channel) {
  f = strip(f);
  analysis_.exprWalk().expr(f); // a function of the program as a value
  const FunctionDecl *decl = nullptr;
  if (auto *fe = dyn_cast<FunctionExpr>(f)) {
    decl = fe->fn;
  } else if (auto *n = dyn_cast<NameExpr>(f); n && n->func) {
    decl = n->func->decl;
  } else if (n && n->var && (n->var->function || n->var->lambda)) {
    decl = n->var->function ? n->var->function : n->var->lambda;
  }
  if (decl && decl->body && !analysis_.isLibrary(decl)) {
    unsigned fi = analysis_.codeWalk().discover(decl);
    if (!llvm::is_contained(analysis_.listeners_[channel], fi)) {
      analysis_.listeners_[channel].push_back(fi);
    }
    return;
  }
  if (f->type && f->type->nonOptional()->isFunction() &&
      !llvm::is_contained(analysis_.listenerTypes_[channel], f->type->nonOptional())) {
    analysis_.listenerTypes_[channel].push_back(f->type->nonOptional());
  }
}

void LibraryEffects::dispatch(const CallExpr *c, CallKind kind, Channel channel, const Use &use) {
  (void)use;
  CallRec &rec = analysis_.codeWalk().record(c, kind, {}, c->loc());
  rec.call = c;
  rec.channel = channel;
}

// What a call of the method `name` of a class of a built-in module touches
// of the object it is called on.
void LibraryEffects::libraryState(llvm::StringRef cls, llvm::StringRef name, std::optional<Ref> self,
                            bool write, llvm::SMLoc at) {
  if (isSignalClass(cls)) {
    analysis_.resource(Res::Signals, write, at);
    return;
  }
  if (!self) {
    return;
  }
  if (isEmitterClass(cls) || isTargetClass(cls)) {
    bool reads = name == "emit" || name == "dispatchEvent" || name == "listenerCount" ||
                 name == "listeners" || name == "rawListeners" || name == "eventNames" ||
                 name == "getMaxListeners";
    analysis_.codeWalk().touch(LocKind::Field, *self, analysis_.listenersKey_, write && !reads, at);
    return;
  }
  analysis_.codeWalk().touch(LocKind::Field, *self, analysis_.state_, write, at);
}

// `o.x = v` of a setter of a class of a built-in module: its call. A
// function assigned to `onabort` (or another `on...`) is a listener.
bool LibraryEffects::librarySetter(const MemberExpr *m, const Expr *value) {
  const Type *type = m->object->type ? m->object->type->nonOptional() : nullptr;
  const ClassDecl *cls = type ? type->classDecl() : nullptr;
  if (!cls || !cls->library) {
    return false;
  }
  bool setter = false;
  for (const ClassDecl *k = cls; k && !setter; k = k->parent) {
    for (const FunctionDecl *fn : k->methods) {
      setter |= fn->name == m->name && fn->accessor == FunctionDecl::Accessor::Setter;
    }
  }
  if (!setter) {
    return false;
  }
  if (llvm::StringRef(m->name).starts_with("on") && value && value->type &&
      value->type->nonOptional()->isFunction()) {
    listen(value, TargetListeners);
  }
  libraryState(libraryClassName(cls), m->name, analysis_.aliasAnalysis().receiverRef(m->object), true, m->loc());

  return true;
}

// What a built-in reads and writes, once its arguments are evaluated.
void LibraryEffects::builtinEffects(const CallExpr *c, const Expr *recv) {
  llvm::SMLoc at = c->loc();
  Builtin b = c->builtin;
  auto arg = [&](size_t i) -> const Expr * { return i < c->args.size() ? c->args[i] : nullptr; };
  auto readArgs = [&] {
    for (const Expr *a : c->args) {
      analysis_.exprWalk().contents(a, false, at);
    }
  };
  auto field = [&](const Expr *object, llvm::StringRef name, bool write) {
    if (object && holdsReferences(object->type)) {
      analysis_.codeWalk().touch(LocKind::Field, analysis_.aliasAnalysis().refOf(object), analysis_.intern(name), write, at);
    }
  };
  switch (b) {
  case Builtin::ConsoleLog:
  case Builtin::ProcessStdoutWrite:
    readArgs();
    analysis_.resource(Res::Stdout, true, at);
    return;
  case Builtin::ConsoleError:
  case Builtin::ProcessStderrWrite:
    readArgs();
    analysis_.resource(Res::Stderr, true, at);
    return;
  case Builtin::ProcessExit:
    // Ends the program: after what was printed, before what was not.
    analysis_.resource(Res::Process, true, at);
    analysis_.resource(Res::Stdout, true, at);
    analysis_.resource(Res::Stderr, true, at);
    return;
  case Builtin::ProcessExitCode:
  case Builtin::ProcessEnv:
  case Builtin::EnvMap:
    analysis_.resource(Res::Process, false, at);
    return;
  case Builtin::ProcessSetExitCode:
    analysis_.resource(Res::Process, true, at);
    return;
  case Builtin::ReadLine:
  case Builtin::RlQuestion:
  case Builtin::RlQuestionCallback:
    // Takes the next line of input, after writing the question.
    analysis_.resource(Res::Stdin, true, at);
    analysis_.resource(Res::Stdout, true, at);
    return;
  case Builtin::RlClose:
    analysis_.resource(Res::Stdin, true, at);
    return;
  case Builtin::StringOf:
  case Builtin::JsonStringify:
    readArgs();
    return;
  case Builtin::TupleJoin:
    field(recv, "*", false);
    return;
  // Arrays.
  case Builtin::ArrPush:
  case Builtin::ArrPop:
  case Builtin::ArrShift:
  case Builtin::ArrUnshift:
  case Builtin::ArrSplice:
    analysis_.exprWalk().elements(recv, true, at);
    if (recv && holdsReferences(recv->type)) {
      analysis_.codeWalk().touch(LocKind::Field, analysis_.aliasAnalysis().refOf(recv), analysis_.length_, true, at);
    }
    return;
  case Builtin::ArrReverse:
  case Builtin::ArrFill:
  case Builtin::ArrSort:
  case Builtin::ArrSortWith:
    analysis_.exprWalk().elements(recv, true, at);
    return;
  case Builtin::ArrJoin:
  case Builtin::ArrToString:
    analysis_.exprWalk().contents(recv, false, at);
    return;
  case Builtin::ArrConcat:
    analysis_.exprWalk().elements(recv, false, at);
    for (const Expr *a : c->args) {
      analysis_.exprWalk().elements(a, false, at);
    }
    return;
  case Builtin::ArrAt:
  case Builtin::ArrSlice:
  case Builtin::ArrIndexOf:
  case Builtin::ArrLastIndexOf:
  case Builtin::ArrIncludes:
  case Builtin::ArrForEach:
  case Builtin::ArrMap:
  case Builtin::ArrFilter:
  case Builtin::ArrFlatMap:
  case Builtin::ArrReduce:
  case Builtin::ArrReduceRight:
  case Builtin::ArrFind:
  case Builtin::ArrFindIndex:
  case Builtin::ArrFindLast:
  case Builtin::ArrFindLastIndex:
  case Builtin::ArrSome:
  case Builtin::ArrEvery:
  case Builtin::ArrKeys:
  case Builtin::ArrValues:
  case Builtin::ArrEntries:
    analysis_.exprWalk().elements(recv, false, at);
    return;
  case Builtin::ObjectKeys:
  case Builtin::ObjectValues:
  case Builtin::ObjectEntries:
    if (const Expr *a = arg(0); a && a->type && a->type->nonOptional()->isObject()) {
      field(a, "*", false);
    } else {
      analysis_.exprWalk().elements(a, false, at);
    }
    return;
  // Iterators: a step moves an iterator on, and a generator runs.
  case Builtin::IterNext:
    if (recv) {
      analysis_.codeWalk().iterate(recv, c);
    }
    return;
  case Builtin::IterFrom:
  case Builtin::ArrayFrom:
    if (const Expr *a = arg(0);
        a && a->type &&
        (a->type->nonOptional()->isIterator() || a->type->nonOptional()->isAsyncIterator())) {
      analysis_.codeWalk().iterate(a, c);
    } else {
      analysis_.exprWalk().contents(arg(0), false, at);
    }
    return;
  case Builtin::IterMap:
  case Builtin::IterFilter:
  case Builtin::IterTake:
  case Builtin::IterDrop:
  case Builtin::IterFlatMap:
  case Builtin::IterToArray:
  case Builtin::IterForEach:
  case Builtin::IterReduce:
  case Builtin::IterSome:
  case Builtin::IterEvery:
  case Builtin::IterFind:
    if (recv) {
      analysis_.codeWalk().iterate(recv, c);
    }
    return;
  // Dictionaries.
  case Builtin::MapGet:
  case Builtin::MapHas:
  case Builtin::MapSize:
  case Builtin::MapForEach:
  case Builtin::MapKeys:
  case Builtin::MapValues:
  case Builtin::MapEntries:
  case Builtin::SetValues:
  case Builtin::SetEntries:
    analysis_.exprWalk().elements(recv, false, at);
    return;
  case Builtin::MapSet:
  case Builtin::MapDelete:
  case Builtin::MapClear:
  case Builtin::SetAdd:
  case Builtin::RecordSet:
    analysis_.exprWalk().elements(recv, true, at);
    return;
  case Builtin::RecordNullishSet:
    analysis_.exprWalk().elements(recv, false, at);
    analysis_.exprWalk().elements(recv, true, at);
    return;
  case Builtin::SetUnion:
  case Builtin::SetIntersection:
  case Builtin::SetDifference:
  case Builtin::SetSymmetricDifference:
  case Builtin::SetIsSubsetOf:
  case Builtin::SetIsSupersetOf:
  case Builtin::SetIsDisjointFrom:
    analysis_.exprWalk().elements(recv, false, at);
    analysis_.exprWalk().elements(arg(0), false, at);
    return;
  case Builtin::DictNew:
  case Builtin::DictGroupBy:
    analysis_.exprWalk().contents(arg(0), false, at);
    return;
  // The files, with fd 0 for input and 1 and 2 for output.
  case Builtin::FsReadFile:
    if (const Expr *a = arg(0); a && a->type && a->type->isInteger()) {
      analysis_.resource(Res::Stdin, true, at);
    }
    analysis_.resource(Res::Files, false, at);
    return;
  case Builtin::FsWriteFile:
    if (const Expr *a = arg(0); a && a->type && a->type->isInteger()) {
      analysis_.resource(Res::Stdout, true, at);
      analysis_.resource(Res::Stderr, true, at);
    }
    analysis_.exprWalk().contents(arg(1), false, at);
    analysis_.resource(Res::Files, true, at);
    return;
  case Builtin::FsExists:
  case Builtin::FsAccess:
  case Builtin::FsStat:
  case Builtin::FsReaddir:
  case Builtin::FsRealpath:
    analysis_.resource(Res::Files, false, at);
    return;
  case Builtin::FsChmod:
  case Builtin::FsTruncate:
  case Builtin::FsMkdir:
  case Builtin::FsMkdtemp:
  case Builtin::FsRmdir:
  case Builtin::FsUnlink:
  case Builtin::FsRm:
  case Builtin::FsRename:
  case Builtin::FsCopyFile:
    analysis_.resource(Res::Files, true, at);
    return;
  // Regular expressions keep their lastIndex.
  case Builtin::RegExpTest:
  case Builtin::RegExpExec:
    field(recv, "lastIndex", true);
    return;
  case Builtin::StrMatch:
  case Builtin::StrMatchGlobal:
  case Builtin::StrMatchAll:
  case Builtin::StrSearch:
  case Builtin::StrReplaceRegex:
  case Builtin::StrReplaceRegexFn:
  case Builtin::StrSplitRegex:
    field(arg(0), "lastIndex", true);
    return;
  // Dates hold their time.
  case Builtin::DateSetTime:
  case Builtin::DateSetUTC:
  case Builtin::DateSetLocal:
    field(recv, "%time", true);
    return;
  case Builtin::DateGetTime:
  case Builtin::DateGetUTC:
  case Builtin::DateGetLocal:
  case Builtin::DateToISOString:
  case Builtin::DateToJSON:
  case Builtin::DateToUTCString:
  case Builtin::DateToLocalString:
  case Builtin::DateToLocale:
    field(recv, "%time", false);
    return;
  case Builtin::DateCopy:
  case Builtin::DtfFormat:
  case Builtin::DtfFormatToParts:
    field(arg(0), "%time", false);
    return;
  // Binary data.
  case Builtin::BytesFromArray:
  case Builtin::BytesFromBytes:
  case Builtin::BytesOf:
  case Builtin::BufferConcat:
    analysis_.exprWalk().contents(arg(0), false, at);
    return;
  case Builtin::BytesAt:
  case Builtin::BytesIndexOf:
  case Builtin::BytesJoin:
  case Builtin::BytesSlice:
  case Builtin::BytesToReversed:
  case Builtin::BytesToSorted:
  case Builtin::BytesWith:
  case Builtin::ArrayBufferSlice:
  case Builtin::DataViewGet:
  case Builtin::BufferToString:
  case Builtin::BufferIndexOf:
  case Builtin::BufferRead:
  case Builtin::BufferReadVar:
  case Builtin::BufferToJSON:
  case Builtin::BufferEquals:
  case Builtin::BufferCompareTo:
    analysis_.exprWalk().elements(recv, false, at);
    for (const Expr *a : c->args) {
      analysis_.exprWalk().elements(a, false, at);
    }
    return;
  case Builtin::BufferCompare:
    analysis_.exprWalk().elements(arg(0), false, at);
    analysis_.exprWalk().elements(arg(1), false, at);
    return;
  case Builtin::BytesReverse:
  case Builtin::BytesSort:
  case Builtin::BytesFill:
  case Builtin::BytesCopyWithin:
  case Builtin::DataViewSet:
  case Builtin::BufferWrite:
  case Builtin::BufferFill:
  case Builtin::BufferWriteNumber:
  case Builtin::BufferWriteVar:
  case Builtin::BufferSwap:
    analysis_.exprWalk().elements(recv, true, at);
    return;
  case Builtin::BytesSet:
    analysis_.exprWalk().elements(recv, true, at);
    analysis_.exprWalk().contents(arg(0), false, at);
    return;
  case Builtin::BufferCopy:
    analysis_.exprWalk().elements(recv, false, at);
    analysis_.exprWalk().elements(arg(0), true, at);
    return;
  case Builtin::TextEncodeInto:
    analysis_.exprWalk().elements(arg(1), true, at);
    return;
  case Builtin::TextDecode:
    analysis_.exprWalk().elements(arg(0), false, at);
    return;
  // Metadata.
  case Builtin::MetaDefine:
  case Builtin::MetaDelete:
    analysis_.resource(Res::Metadata, true, at);
    return;
  // A child process of child_process: what it does, the program cannot see
  // but in the files, the output it shares and the environment.
  case Builtin::ChildSpawnSync:
  case Builtin::ChildExecFileSync:
  case Builtin::ChildExecSync:
    readArgs();
    analysis_.resource(Res::Files, true, at);
    analysis_.resource(Res::Stdout, true, at);
    analysis_.resource(Res::Stderr, true, at);
    analysis_.resource(Res::Process, false, at);
    return;
  // node:crypto, by the kinds of the arguments of its function (sema/
  // crypto_api.h): an object of node:crypto changes as it is used (update,
  // final, digest), the bytes that randomFill and getRandomValues fill are
  // written, and the data given read.
  case Builtin::Crypto: {
    auto *number = dyn_cast_or_null<IntLitExpr>(strip(arg(0)));
    if (!number || number->value >= std::size(kCryptoFunctions)) {
      readArgs();
      return;
    }
    auto fn = CryptoFn(unsigned(number->value));
    llvm::StringRef kinds = kCryptoFunctions[size_t(number->value)].params;
    bool fills = fn == CryptoFn::RandomFill || fn == CryptoFn::GetRandomValues;
    for (size_t i = 1; i < c->args.size(); ++i) {
      const Expr *a = c->args[i];
      char kind = i - 1 < kinds.size() ? kinds[i - 1] : 'd';
      if (!a || !holdsReferences(a->type)) {
        continue;
      }
      bool bytes = a->type->nonOptional()->isBytes();
      if (kind == 'o' && bytes) {
        analysis_.exprWalk().elements(a, fills, at);
      } else if (kind == 'o') {
        analysis_.codeWalk().touch(LocKind::Field, analysis_.aliasAnalysis().refOf(a), analysis_.state_, true, at);
      } else if (bytes) {
        analysis_.exprWalk().elements(a, false, at);
      } else {
        analysis_.exprWalk().contents(a, false, at);
      }
    }
    return;
  }
  case Builtin::CryptoKeyPair:
    readArgs();
    return;
  // Reflection: the fields read by a name known only when the program runs,
  // and the method or the constructor it runs (Analysis::resolve).
  case Builtin::ReflectGet:
    if (const Expr *a = arg(0); a && holdsReferences(a->type)) {
      analysis_.codeWalk().touch(LocKind::Field, analysis_.aliasAnalysis().refOf(a), analysis_.star_, false, at);
    }
    return;
  case Builtin::ReflectApply: {
    for (size_t i = 2; i < c->args.size(); ++i) {
      analysis_.exprWalk().contents(c->args[i], false, at);
    }
    CallRec &rec = analysis_.codeWalk().record(c, CallKind::Call, {}, at);
    rec.call = c;
    const Expr *fn = strip(arg(0));
    if (fn && fn->type && fn->type->nonOptional()->isFunction()) {
      rec.byType = fn->type; // a function the program has as a value
      return;
    }
    // A method that Reflect.get found by name: one of the classes of the
    // object it is applied to.
    rec.reflect = CallRec::Reflect::Apply;
    const Expr *self = strip(arg(1));
    rec.bySelf = self ? std::optional<Ref>(analysis_.aliasAnalysis().receiverRef(self)) : std::nullopt;
    rec.byObject = self ? self->type : nullptr;
    return;
  }
  case Builtin::ReflectConstruct:
  case Builtin::FromJsonClass: {
    for (size_t i = 1; i < c->args.size(); ++i) {
      analysis_.exprWalk().contents(c->args[i], false, at);
    }
    CallRec &rec = analysis_.codeWalk().record(c, CallKind::Call, {}, at);
    rec.call = c;
    rec.reflect = CallRec::Reflect::Construct;
    return;
  }
  case Builtin::MetaGet:
  case Builtin::MetaGetOwn:
  case Builtin::MetaHas:
  case Builtin::MetaHasOwn:
    analysis_.resource(Res::Metadata, false, at);
    return;
  default:
    break;
  }
  if (knownBuiltin(b)) {
    return; // numbers, strings, paths, promises, timers...: nothing shared
  }
  // A built-in the pass does not know may change what it is given.
  analysis_.exprWalk().contents(recv, true, at);
  for (const Expr *a : c->args) {
    analysis_.exprWalk().contents(a, true, at);
  }
  analysis_.resource(Res::Runtime, true, at);
}

} // namespace tz::parallel
