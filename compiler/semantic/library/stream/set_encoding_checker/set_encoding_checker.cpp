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

namespace {

struct StreamEscape {
  llvm::SMLoc loc;
  std::string how;
};

}

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
  auto plainCall = [&]() -> Expr * {
    this->emitterChecker->callLibraryMethod(call, member, cls, "setEncoding", {}, nullptr, false);
    if (!call->type->isError()) {
      call->type = receiver;
    }

    return call;
  };
  ResultDecodedChunks decoded = SetEncodingChecker::decodedChunks(chunks.readable, receiver);
  if (decoded.isErr()) {
    this->state.getDiagnostics().error(loc, decoded.error().message);
    return call;
  }
  if (chunks.readable->isString()) {
    return plainCall();
  }
  std::optional<FieldPath> path;
  const Expr *object = member->object;
  while (auto *conv = dyn_cast<CastExpr>(object)) {
    if (!conv->isImplicit()) {
      break;
    }
    object = conv->operand;
  }
  bool stdinStream = false;
  if (auto *c = dyn_cast<CallExpr>(object); c && c->func() && c->func()->name == "processStdin") {
    ResultStdinIsPlaced placed = this->stdinPlace();
    if (placed.isErr()) {
      this->state.getDiagnostics().error(loc, placed.error().message);
      return call;
    }
    stdinStream = true;
    path = FieldPath{};
    path->var = this->streamSources->processStdinVar();
  } else if (auto *name = dyn_cast<NameExpr>(object); name && name->var()) {
    path = FieldPath{};
    path->var = name->var();
  } else {
    path = this->fieldFacts->fieldPath(object);
  }
  auto reject = [&](const std::string &why) {
    std::string message = ErrorTextSetEncoding::changes(why);
    this->state.getDiagnostics().error(loc, message);

    return call;
  };
  if (!path || !path->var) {
    return reject(ErrorTextEncodingWhy::noConstant());
  }
  const VarSymbol *root = path->var;
  if (!root->isConst) {
    return reject(ErrorTextEncodingWhy::notConst(root->name));
  }
  {
    const Type *at = root->type;
    for (llvm::StringRef field : path->names) {
      const Field *f = at ? at->nonOptional()->field(field) : nullptr;
      if (!f || !f->readonly) {
        return reject(ErrorTextEncodingWhy::fieldSet(field));
      }
      at = f->type;
    }
  }
  ScopeStack &scopes = this->state.getScopeStack();
  size_t declared = stdinStream ? 0 : scopes.getScopeDepth();
  if (!stdinStream) {
    if (std::optional<size_t> found = scopes.findDeclaringScope(root)) {
      declared = *found;
    }
  }
  bool branched = stdinStream ? scopes.isInsideBranch() : scopes.isBranchedSince(declared);
  if (declared + 1 != this->state.getScopeStack().getScopeDepth() || branched) {
    return reject(ErrorTextEncodingWhy::ownBlock(root->name));
  }
  std::vector<StreamEscape> escapes;
  const Expr *base = member->object;
  for (;;) {
    if (auto *conv = dyn_cast<CastExpr>(base)) {
      base = conv->operand;
    } else if (auto *m = dyn_cast<MemberExpr>(base)) {
      base = m->object;
    } else {
      break;
    }
  }
  const char *here = base->loc().getPointer();
  const char *start = stdinStream ? nullptr : root->loc.getPointer();
  auto before = [&](const Expr *e) {
    const char *at = e->loc().getPointer();

    return at > start && at < here;
  };
  auto isPath = [&](const Expr *e) -> bool {
    while (auto *conv = dyn_cast<CastExpr>(e)) {
      if (!conv->isImplicit()) {
        break;
      }
      e = conv->operand;
    }
    if (stdinStream) {
      if (auto *c = dyn_cast<CallExpr>(e)) {
        return c->func() && c->func()->name == "processStdin";
      }
      return this->processChecker->isProcessMember(e, "stdin");
    }
    if (path->names.empty()) {
      auto *name = dyn_cast<NameExpr>(e);
      return name && (name->var() == root || (!name->var() && name->name == root->name));
    }
    std::optional<FieldPath> p = this->fieldFacts->fieldPath(e);

    return p && *p == *path;
  };
  llvm::SmallPtrSet<const Expr *, 16> safe;
  static const llvm::StringLiteral listening[] = {
      "on", "once", "addListener", "prependListener", "prependOnceListener", "pipe"};
  auto unwrap = [](const Expr *e) {
    while (auto *conv = dyn_cast<CastExpr>(e)) {
      if (!conv->isImplicit()) {
        break;
      }
      e = conv->operand;
    }

    return e;
  };
  auto classify = [&](const Expr *e) {
    if (!before(e)) {
      return;
    }
    if (auto *c = dyn_cast<CallExpr>(e)) {
      auto *m = dyn_cast<MemberExpr>(c->callee);
      if (m && isPath(m->object)) {
        if (llvm::is_contained(listening, m->name)) {
          escapes.push_back({m->loc(), ErrorTextEncodingUse::listeners(m->name)});
        } else {
          safe.insert(unwrap(m->object));
        }
      }
    } else if (auto *m = dyn_cast<MemberExpr>(e)) {
      if (isPath(m->object)) {
        safe.insert(unwrap(m->object));
      }
    }
  };
  std::vector<const Stmt *> body;
  if (FunctionDecl *owner = stdinStream ? nullptr : root->owner(); owner && owner->body) {
    body.push_back(owner->body);
  } else if (this->state.getModuleStates().getCurrentModule()) {
    for (const Stmt *item : this->state.getModuleStates().getCurrentModule()->file.items) {
      body.push_back(item);
    }
  }
  for (const Stmt *stmt : body) {
    AstWalker::visitExprs(stmt, classify);
  }
  llvm::SmallPtrSet<const Expr *, 32> direct;
  for (const Stmt *stmt : body) {
    AstWalker::visitExprs(stmt, [&](const Expr *e) { direct.insert(e); }, false);
  }
  for (const Stmt *stmt : body) {
    AstWalker::visitExprs(stmt, [&](const Expr *e) {
      if (!before(e) || isa<CastExpr>(e) || !isPath(e)) {
        return;
      }
      if (!direct.contains(e)) {
        escapes.push_back({e->loc(), ErrorTextEncodingUse::function()});
      } else if (!safe.contains(e)) {
        escapes.push_back({e->loc(), ErrorTextEncodingUse::elsewhere()});
      }
    });
  }
  if (!escapes.empty()) {
    const StreamEscape *first = &escapes[0];
    for (const StreamEscape &e : escapes) {
      if (e.loc.getPointer() < first->loc.getPointer()) {
        first = &e;
      }
    }
    std::string message = ErrorTextSetEncoding::usedBefore(first->how);
    std::string usedHere = ErrorTextSetEncoding::usedHere();
    this->state.getDiagnostics().error(loc, message);
    this->state.getDiagnostics().note(first->loc, usedHere);
    return call;
  }
  const Type *view = this->encodedStreamType(cls, loc);
  plainCall();
  if (!view || call->type->isError()) {
    return call;
  }
  call->type = view;
  this->state.getStreamStore().rememberStreamView(*path, llvm::SMLoc::getFromPointer(here), view);

  return call;
}

}
