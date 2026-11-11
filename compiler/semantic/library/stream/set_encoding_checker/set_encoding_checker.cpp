#include "compiler/semantic/library/stream/set_encoding_checker/set_encoding_checker.h"
#include "compiler/program/ast_walker/ast_walker.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/flow/field_facts/field_facts.h"
#include "compiler/semantic/generics/generic_class_instantiator/generic_class_instantiator.h"
#include "compiler/semantic/library/events/emitter_checker/emitter_checker.h"
#include "compiler/semantic/library/process/process_checker/process_checker.h"
#include "compiler/semantic/library/stream/chunk_types/chunk_types.h"
#include "compiler/semantic/library/stream/stream_pipeline/stream_pipeline.h"
#include "compiler/semantic/library/stream/stream_sources/stream_sources.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

SetEncodingChecker::SetEncodingChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->chunkTypes = &all.getChunkTypes();
  this->fieldFacts = &all.getFieldFacts();
  this->streamSources = &all.getStreamSources();
  this->emitterChecker = &all.getEmitterChecker();
  this->processChecker = &all.getProcessChecker();
  this->genericClassInstantiator = &all.getGenericClassInstantiator();
}

const Type *SetEncodingChecker::encodedStreamType(ClassDecl *cls, llvm::SMLoc loc) {
  for (ClassDecl *c = cls; c; c = c->parent) {
    if (!c->library() || !c->generic() || c->typeArgs.empty()) {
      continue;
    }
    llvm::StringRef name = c->generic()->name;
    std::vector<const Type *> args;
    ClassDecl *generic = c->generic();
    if (name == "Readable") {
      args = {Type::string()};
    } else if (name == "Duplex" || name == "Transform") {
      args = {c->typeArgs[0], Type::string()};
    } else if (name == "PassThrough") {
      args = {c->typeArgs[0], Type::string()};
    } else {
      continue;
    }
    if (!generic) {
      return nullptr;
    }
    GenericClassInstantiator::ResultClassMade made =
        this->genericClassInstantiator->instantiateClass(generic, args, loc);
    if (made.isErr() &&
        made.error().code != GenericClassInstantiatorInstantiateClassErrorCode::ALREADY_REPORTED) {
      llvm::SMLoc at =
          made.error().code == GenericClassInstantiatorInstantiateClassErrorCode::ENDLESS_INSTANCES
              ? generic->loc()
              : (loc.isValid() ? loc : generic->loc());
      this->state.getDiagnostics().error(at, made.error().message);
    }
    return made.isOk() ? made.value()->type : nullptr;
  }

  return nullptr;
}

SetEncodingChecker::ResultDecodedChunks SetEncodingChecker::decodedChunks(const Type *readable,
                                                                          const Type *receiver) {
  if (!readable) {
    std::string message = ErrorTextSetEncoding::unreadable(receiver->name());

    return err(SetEncodingCheckerDecodedChunksError{
        .code = SetEncodingCheckerDecodedChunksErrorCode::NOT_READABLE,
        .message = message,
    });
  }
  if (!readable->isString() && !readable->isBuffer()) {
    std::string message = ErrorTextSetEncoding::objectMode(readable->name());

    return err(SetEncodingCheckerDecodedChunksError{
        .code = SetEncodingCheckerDecodedChunksErrorCode::OBJECT_MODE,
        .message = message,
    });
  }

  return ok(readable);
}

SetEncodingChecker::ResultStdinIsPlaced SetEncodingChecker::stdinPlace() const {
  const Module *module = this->state.getModuleStates().getCurrentModule();
  bool topLevel = module && module->isEntry && !this->state.getFunctionStack().isInsideFunction() &&
                  this->state.getScopeStack().isTopLevel();
  if (!topLevel) {
    std::string message = ErrorTextSetEncoding::stdinPlace();

    return err(SetEncodingCheckerStdinPlaceError{
        .code = SetEncodingCheckerStdinPlaceErrorCode::NOT_TOP_LEVEL,
        .message = message,
    });
  }

  return ok(module);
}

Expr *SetEncodingChecker::checkSetEncoding(CallExpr *call, MemberExpr *member, ClassDecl *cls) {
  const Type *receiver = member->object->type;
  StreamChunks chunks = this->chunkTypes->streamChunksOf(cls);
  llvm::SMLoc loc = member->loc();
  ResultDecodedChunks decoded = SetEncodingChecker::decodedChunks(chunks.readable, receiver);
  if (decoded.isErr()) {
    this->state.getDiagnostics().error(loc, decoded.error().message);
    return call;
  }
  if (chunks.readable->isString()) {
    return this->plainSetEncoding(call, member, cls, receiver);
  }
  const Expr *object = SetEncodingChecker::withoutImplicit(member->object);
  bool stdinStream = SetEncodingChecker::isStdinCall(object);
  if (stdinStream) {
    ResultStdinIsPlaced placed = this->stdinPlace();
    if (placed.isErr()) {
      this->state.getDiagnostics().error(loc, placed.error().message);
      return call;
    }
  }
  std::optional<FieldPath> path = this->encodingPath(object, stdinStream);
  std::optional<std::string> why = this->encodingPathProblem(path, stdinStream);
  if (why.has_value()) {
    std::string message = ErrorTextSetEncoding::changes(*why);
    this->state.getDiagnostics().error(loc, message);
    return call;
  }
  EncodingRun run{
      .member = member,
      .stdinStream = stdinStream,
      .path = *path,
      .here = nullptr,
      .start = nullptr,
      .escapes = {},
      .safe = {},
  };
  SetEncodingChecker::markEscapeWindow(run);
  this->collectEscapes(run);
  if (!run.escapes.empty()) {
    this->reportFirstEscape(run, loc);
    return call;
  }
  const Type *view = this->encodedStreamType(cls, loc);
  this->plainSetEncoding(call, member, cls, receiver);
  if (view == nullptr || call->type->isError()) {
    return call;
  }
  call->type = view;
  llvm::SMLoc here = llvm::SMLoc::getFromPointer(run.here);
  this->state.getStreamStore().rememberStreamView(run.path, here, view);

  return call;
}

Expr *SetEncodingChecker::plainSetEncoding(CallExpr *call, MemberExpr *member, ClassDecl *cls,
                                           const Type *receiver) {
  this->emitterChecker->callLibraryMethod(call, member, cls, "setEncoding", {}, nullptr, false);
  if (!call->type->isError()) {
    call->type = receiver;
  }

  return call;
}

const Expr *SetEncodingChecker::withoutImplicit(const Expr *expr) {
  const Expr *bare = expr;
  auto *conv = dyn_cast<CastExpr>(bare);
  while (conv != nullptr && conv->isImplicit()) {
    bare = conv->operand;
    conv = dyn_cast<CastExpr>(bare);
  }

  return bare;
}

bool SetEncodingChecker::isStdinCall(const Expr *expr) {
  auto *c = dyn_cast<CallExpr>(expr);

  return c != nullptr && c->func() != nullptr && c->func()->name == "processStdin";
}

std::optional<FieldPath> SetEncodingChecker::encodingPath(const Expr *object, bool stdinStream) {
  if (stdinStream) {
    FieldPath stdinPath;
    stdinPath.var = this->streamSources->processStdinVar();
    return stdinPath;
  }
  auto *name = dyn_cast<NameExpr>(object);
  if (name != nullptr && name->var() != nullptr) {
    FieldPath namePath;
    namePath.var = name->var();
    return namePath;
  }

  return this->fieldFacts->fieldPath(object);
}

std::optional<std::string>
SetEncodingChecker::encodingPathProblem(const std::optional<FieldPath> &path,
                                        bool stdinStream) const {
  if (!path.has_value() || path->var == nullptr) {
    return ErrorTextEncodingWhy::noConstant();
  }
  const VarSymbol *root = path->var;
  if (!root->isConst) {
    return ErrorTextEncodingWhy::notConst(root->name);
  }
  std::optional<llvm::StringRef> field = SetEncodingChecker::writableField(*path);
  if (field.has_value()) {
    return ErrorTextEncodingWhy::fieldSet(*field);
  }
  if (!this->isOwnBlock(root, stdinStream)) {
    return ErrorTextEncodingWhy::ownBlock(root->name);
  }

  return std::nullopt;
}

std::optional<llvm::StringRef> SetEncodingChecker::writableField(const FieldPath &path) {
  const Type *at = path.var->type;
  for (llvm::StringRef field : path.names) {
    const Field *f = at != nullptr ? at->nonOptional()->field(field) : nullptr;
    if (f == nullptr || !f->readonly) {
      return field;
    }
    at = f->type;
  }

  return std::nullopt;
}

bool SetEncodingChecker::isOwnBlock(const VarSymbol *root, bool stdinStream) const {
  ScopeStack &scopes = this->state.getScopeStack();
  size_t declared = stdinStream ? 0 : scopes.getScopeDepth();
  if (!stdinStream) {
    std::optional<size_t> found = scopes.findDeclaringScope(root);
    if (found.has_value()) {
      declared = *found;
    }
  }
  bool branched = stdinStream ? scopes.isInsideBranch() : scopes.isBranchedSince(declared);

  return declared + 1 == scopes.getScopeDepth() && !branched;
}

void SetEncodingChecker::markEscapeWindow(EncodingRun &run) {
  const Expr *base = run.member->object;
  for (;;) {
    auto *conv = dyn_cast<CastExpr>(base);
    auto *m = dyn_cast<MemberExpr>(base);
    if (conv != nullptr) {
      base = conv->operand;
    } else if (m != nullptr) {
      base = m->object;
    } else {
      break;
    }
  }
  run.here = base->loc().getPointer();
  run.start = run.stdinStream ? nullptr : run.path.var->loc.getPointer();
}

bool SetEncodingChecker::isBefore(const EncodingRun &run, const Expr *e) {
  const char *at = e->loc().getPointer();

  return at > run.start && at < run.here;
}

bool SetEncodingChecker::isEncodingPath(const EncodingRun &run, const Expr *e) const {
  const Expr *bare = SetEncodingChecker::withoutImplicit(e);
  if (run.stdinStream) {
    if (isa<CallExpr>(bare)) {
      return SetEncodingChecker::isStdinCall(bare);
    }
    return this->processChecker->isProcessMember(bare, "stdin");
  }
  const VarSymbol *root = run.path.var;
  if (run.path.names.empty()) {
    auto *name = dyn_cast<NameExpr>(bare);
    return name != nullptr &&
           (name->var() == root || (name->var() == nullptr && name->name == root->name));
  }
  std::optional<FieldPath> p = this->fieldFacts->fieldPath(bare);

  return p.has_value() && *p == run.path;
}

void SetEncodingChecker::collectEscapes(EncodingRun &run) {
  std::vector<const Stmt *> body = this->encodingBody(run);
  for (const Stmt *stmt : body) {
    for (const Expr *e : AstWalker::exprsIn(stmt, true)) {
      this->classifyUse(run, e);
    }
  }
  llvm::SmallPtrSet<const Expr *, 32> direct;
  for (const Stmt *stmt : body) {
    std::vector<const Expr *> exprs = AstWalker::exprsIn(stmt, false);
    direct.insert(exprs.begin(), exprs.end());
  }
  for (const Stmt *stmt : body) {
    for (const Expr *e : AstWalker::exprsIn(stmt, true)) {
      this->noteEscape(run, e, direct);
    }
  }
}

std::vector<const Stmt *> SetEncodingChecker::encodingBody(const EncodingRun &run) const {
  std::vector<const Stmt *> body;
  FunctionDecl *owner = run.stdinStream ? nullptr : run.path.var->owner();
  if (owner != nullptr && owner->body != nullptr) {
    body.push_back(owner->body);
    return body;
  }
  Module *module = this->state.getModuleStates().getCurrentModule();
  if (module != nullptr) {
    for (const Stmt *item : module->file.items) {
      body.push_back(item);
    }
  }

  return body;
}

void SetEncodingChecker::classifyUse(EncodingRun &run, const Expr *e) const {
  static const llvm::StringLiteral listening[] = {
      "on", "once", "addListener", "prependListener", "prependOnceListener", "pipe"};
  if (!SetEncodingChecker::isBefore(run, e)) {
    return;
  }
  auto *c = dyn_cast<CallExpr>(e);
  if (c != nullptr) {
    auto *m = dyn_cast<MemberExpr>(c->callee);
    if (m == nullptr || !this->isEncodingPath(run, m->object)) {
      return;
    }
    if (llvm::is_contained(listening, m->name)) {
      run.escapes.push_back({m->loc(), ErrorTextEncodingUse::listeners(m->name)});
    } else {
      run.safe.insert(SetEncodingChecker::withoutImplicit(m->object));
    }
    return;
  }
  auto *m = dyn_cast<MemberExpr>(e);
  if (m != nullptr && this->isEncodingPath(run, m->object)) {
    run.safe.insert(SetEncodingChecker::withoutImplicit(m->object));
  }
}

void SetEncodingChecker::noteEscape(EncodingRun &run, const Expr *e,
                                    const llvm::SmallPtrSetImpl<const Expr *> &direct) const {
  bool candidate =
      SetEncodingChecker::isBefore(run, e) && !isa<CastExpr>(e) && this->isEncodingPath(run, e);
  if (!candidate) {
    return;
  }
  if (!direct.contains(e)) {
    run.escapes.push_back({e->loc(), ErrorTextEncodingUse::function()});
  } else if (!run.safe.contains(e)) {
    run.escapes.push_back({e->loc(), ErrorTextEncodingUse::elsewhere()});
  }
}

void SetEncodingChecker::reportFirstEscape(const EncodingRun &run, llvm::SMLoc loc) {
  const StreamEscape *first = &run.escapes[0];
  for (const StreamEscape &e : run.escapes) {
    if (e.loc.getPointer() < first->loc.getPointer()) {
      first = &e;
    }
  }
  std::string message = ErrorTextSetEncoding::usedBefore(first->how);
  std::string usedHere = ErrorTextSetEncoding::usedHere();
  this->state.getDiagnostics().error(loc, message);
  this->state.getDiagnostics().note(first->loc, usedHere);
}
}
