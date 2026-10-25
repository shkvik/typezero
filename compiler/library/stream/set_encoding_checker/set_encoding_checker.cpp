#include "compiler/library/stream/set_encoding_checker/set_encoding_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/check/generics/generic_class_instantiator/generic_class_instantiator.h"
#include "compiler/check/flow/field_facts/field_facts.h"
#include "compiler/library/events/emitter_checker/emitter_checker.h"
#include "compiler/library/process/process_checker/process_checker.h"
#include "compiler/library/stream/chunk_types/chunk_types.h"
#include "compiler/library/stream/stream_sources/stream_sources.h"
#include "compiler/library/stream/stream_pipeline/stream_pipeline.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

namespace {

// How the code before a setEncoding() used the stream: it escaped there,
// where code that sees its chunks as Buffers may get the strings.
struct StreamEscape {
  llvm::SMLoc loc;
  std::string how;
};

} // namespace

SetEncodingChecker::SetEncodingChecker(Sema &sema) : sema_(sema) {}

const Type *SetEncodingChecker::streamViewOf(const Sema::FieldPath &path, llvm::SMLoc loc) const {
  for (auto it = sema_.streamViews_.rbegin(); it != sema_.streamViews_.rend(); ++it) {
    if (it->path == path && it->from.getPointer() < loc.getPointer()) {
      return it->type;
    }
  }

  return nullptr;
}

const Type *SetEncodingChecker::encodedStreamType(ClassDecl *cls, llvm::SMLoc loc) {
  // The class of the program's own stream, if it is one, has its chunk
  // types fixed: the view is the library's class it extends.
  for (ClassDecl *c = cls; c; c = c->parent) {
    if (!c->library || !c->generic || c->typeArgs.empty()) {
      continue;
    }
    llvm::StringRef name = c->generic->name;
    std::vector<const Type *> args;
    ClassDecl *generic = c->generic;
    if (name == "Readable") {
      args = {Type::string()};
    } else if (name == "Duplex" || name == "Transform") {
      args = {c->typeArgs[0], Type::string()};
    } else if (name == "PassThrough") {
      args = {c->typeArgs[0], Type::string()};
    } else {
      continue;
    }
    if (ClassDecl *inst = generic ? sema_.genericClassInstantiator().instantiateClass(generic, args, loc) : nullptr) {
      return inst->type;
    }
    return nullptr;
  }

  return nullptr;
}

Expr *SetEncodingChecker::checkSetEncoding(CallExpr *call, MemberExpr *member, ClassDecl *cls) {
  const Type *receiver = member->object->type;
  Sema::StreamChunks chunks = sema_.chunkTypes().streamChunksOf(cls);
  llvm::SMLoc loc = member->loc();
  auto plainCall = [&]() -> Expr * {
    sema_.emitterChecker().callLibraryMethod(call, member, cls, "setEncoding", {}, nullptr, false);
    if (!call->type->isError()) {
      call->type = receiver;
    }

    return call;
  };
  if (!chunks.readable) {
    sema_.diags_.error(loc, "'" + receiver->name() + "' has no setEncoding(): it is not readable");
    return call;
  }
  // A stream of strings already: another encoding, the same type.
  if (chunks.readable->isString()) {
    return plainCall();
  }
  if (!chunks.readable->isBuffer()) {
    sema_.diags_.error(loc, "setEncoding() decodes the Buffers of a stream of bytes, and this is an "
                      "object-mode stream of " +
                          chunks.readable->name().str() + "; push strings into it instead");
    return call;
  }
  // The stream must be known by a constant, or a read-only field of one,
  // whose type changes from here on (docs/streams.md, section 2).
  std::optional<Sema::FieldPath> path;
  const Expr *object = member->object;
  while (auto *conv = dyn_cast<CastExpr>(object)) {
    if (!conv->isImplicit()) {
      break;
    }
    object = conv->operand;
  }
  // process.stdin: a stream of the whole program, which the entry's
  // top-level code may set the encoding of.
  bool stdinStream = false;
  if (auto *c = dyn_cast<CallExpr>(object); c && c->func && c->func->name == "processStdin") {
    if (!sema_.module_ || !sema_.module_->isEntry || !sema_.functions_.empty() || sema_.scopes_.size() != 1) {
      sema_.diags_.error(loc, "'process.stdin.setEncoding()' changes the type of the chunks of stdin for "
                        "the code after it, so it goes in the top-level code of the program's "
                        "main file, before any other use of process.stdin");
      return call;
    }
    stdinStream = true;
    path = Sema::FieldPath{};
    path->var = sema_.streamSources().processStdinVar();
  } else if (auto *name = dyn_cast<NameExpr>(object); name && name->var) {
    path = Sema::FieldPath{};
    path->var = name->var;
  } else {
    path = sema_.fieldFacts().fieldPath(object);
  }
  auto reject = [&](const std::string &why) {
    sema_.diags_.error(loc, "setEncoding() makes the chunks of the stream strings from here on, so its "
                      "type changes here; " +
                          why +
                          ": give the stream its encoding where it is "
                          "created, e.g. 'createReadStream(path, \"utf8\")' or 'new Readable({ "
                          "encoding: \"utf8\", ... })'");

    return call;
  };
  if (!path || !path->var) {
    return reject("but this stream is not known by a constant");
  }
  const VarSymbol *root = path->var;
  if (!root->isConst) {
    return reject("but '" + root->name.str() +
                  "' may hold another stream later; declare it with 'const'");
  }
  // The fields on the way must be read-only.
  {
    const Type *at = root->type;
    for (llvm::StringRef field : path->names) {
      const Field *f = at ? at->nonOptional()->field(field) : nullptr;
      if (!f || !f->readonly) {
        return reject("but '" + field.str() + "' is a field that code may set to another stream");
      }
      at = f->type;
    }
  }
  // In the block that declares it, not in an if or a loop: after a
  // setEncoding() that may not run, the type of the chunks is not known.
  size_t declared = stdinStream ? 0 : sema_.scopes_.size();
  for (size_t i = sema_.scopes_.size(); i-- > 0 && !stdinStream;) {
    auto found = sema_.scopes_[i].find(root->name);
    if (found != sema_.scopes_[i].end() && found->second.var == root) {
      declared = i;
      break;
    }
  }
  bool branched =
      stdinStream ? sema_.branchDepth_ != 0
                  : declared < sema_.scopeBranches_.size() && sema_.scopeBranches_[declared] != sema_.branchDepth_;
  if (declared + 1 != sema_.scopes_.size() || branched) {
    return reject("but this setEncoding() is in a block of its own (an if, a loop, a function) "
                  "and '" +
                  root->name.str() +
                  "' is declared outside it, so the type would "
                  "depend on whether it runs");
  }
  // What the code before did with the stream.
  std::vector<StreamEscape> escapes;
  // Before the stream this call is on, which starts with its root.
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
  // A use of the path: a name of the variable, or `root.f.g`.
  auto isPath = [&](const Expr *e) -> bool {
    while (auto *conv = dyn_cast<CastExpr>(e)) {
      if (!conv->isImplicit()) {
        break;
      }
      e = conv->operand;
    }
    if (stdinStream) {
      if (auto *c = dyn_cast<CallExpr>(e)) {
        return c->func && c->func->name == "processStdin";
      }
      return sema_.processChecker().isProcessMember(e, "stdin");
    }
    if (path->names.empty()) {
      auto *name = dyn_cast<NameExpr>(e);
      return name && (name->var == root || (!name->var && name->name == root->name));
    }
    std::optional<Sema::FieldPath> p = sema_.fieldFacts().fieldPath(e);

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
          escapes.push_back({m->loc(), "it got listeners (" + m->name.str() + "())"});
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
  if (FunctionDecl *owner = stdinStream ? nullptr : root->owner; owner && owner->body) {
    body.push_back(owner->body);
  } else if (sema_.module_) {
    for (const Stmt *item : sema_.module_->file.items) {
      body.push_back(item);
    }
  }
  for (const Stmt *stmt : body) {
    visitExprs(stmt, classify);
  }
  // The uses in functions written before: they may run after this.
  llvm::SmallPtrSet<const Expr *, 32> direct;
  for (const Stmt *stmt : body) {
    visitExprs(stmt, [&](const Expr *e) { direct.insert(e); }, /*intoFunctions=*/false);
  }
  for (const Stmt *stmt : body) {
    visitExprs(stmt, [&](const Expr *e) {
      if (!before(e) || isa<CastExpr>(e) || !isPath(e)) {
        return;
      }
      if (!direct.contains(e)) {
        escapes.push_back({e->loc(), "a function written before uses it"});
      } else if (!safe.contains(e)) {
        escapes.push_back({e->loc(), "it went elsewhere (passed, stored or returned)"});
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
    sema_.diags_.error(loc, "setEncoding() on a stream that is used elsewhere already, which sees its "
                      "chunks as Buffers, but strings would come: " +
                          first->how +
                          " before this; give the stream its encoding where it is created, e.g. "
                          "'createReadStream(path, \"utf8\")', or call setEncoding() first");
    sema_.diags_.note(first->loc, "the stream is used here");
    return call;
  }
  const Type *view = encodedStreamType(cls, loc);
  plainCall();
  if (!view || call->type->isError()) {
    return call;
  }
  call->type = view;
  // From this call on: the constant is not seen outside its block.
  sema_.streamViews_.push_back({*path, llvm::SMLoc::getFromPointer(here), view});

  return call;
}

Expr *SetEncodingChecker::checkStreamPipe(CallExpr *call, MemberExpr *member, ClassDecl *cls) {
  if (!sema_.argumentChecker().checkArity(call, member->loc(), 1, 2)) {
    return call;
  }
  call->args[0] = sema_.exprChecker().checkExpr(call->args[0]);
  Expr *dest = call->args[0];
  if (dest->type->isError()) {
    return call;
  }
  ClassDecl *destClass = ClassDeclarer::classOf(dest->type);
  if (!destClass || !sema_.chunkTypes().isStreamClass(destClass) || dest->type->isOptional()) {
    sema_.diags_.error(dest->loc(),
                 "pipe() takes the stream to write to, and this is '" + dest->type->name() + "'");
    return call;
  }
  Sema::StreamChunks from = sema_.chunkTypes().streamChunksOf(cls);
  Sema::StreamChunks to = sema_.chunkTypes().streamChunksOf(destClass);
  if (!from.readable) {
    sema_.diags_.error(member->loc(),
                 "'" + member->object->type->name() + "' has no pipe(): it is not readable");
    return call;
  }
  if (!to.writable) {
    sema_.diags_.error(dest->loc(), "pipe() writes to the stream it takes, but '" + dest->type->name() +
                                  "' is not writable");
    return call;
  }
  // A writable side whose type a round looks for gets these chunks.
  if (!sema_.chunkTypes().recordStreamChunk(to.writable, from.readable, dest->loc()) &&
      !sema_.openMaps_.count(from.readable) && !sema_.streamPipeline().streamTakes(to.writable, from.readable)) {
    sema_.diags_.error(dest->loc(), "pipe() writes chunks of " + from.readable->name().str() + " to '" +
                                  dest->type->name().str() + "', which takes " +
                                  to.writable->name().str());
    return call;
  }
  std::vector<bool> given(call->args.size(), false);
  given[0] = true;
  sema_.emitterChecker().callLibraryMethod(call, member, cls, "pipe", {dest->type}, &given, false);
  if (!call->type->isError()) {
    call->type = dest->type;
  }

  return call;
}

} // namespace tz
