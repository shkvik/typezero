#include "compiler/library/stream/set_encoding_checker/set_encoding_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/flow/field_facts/field_facts.h"
#include "compiler/check/generics/generic_class_instantiator/generic_class_instantiator.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/library/events/emitter_checker/emitter_checker.h"
#include "compiler/library/process/process_checker/process_checker.h"
#include "compiler/library/stream/chunk_types/chunk_types.h"
#include "compiler/library/stream/stream_pipeline/stream_pipeline.h"
#include "compiler/library/stream/stream_sources/stream_sources.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

struct StreamEscape {
  llvm::SMLoc loc;
  std::string how;
};

}

SetEncodingChecker::SetEncodingChecker(Checker &checker) : checker_(checker) {}

const Type *SetEncodingChecker::streamViewOf(const Checker::FieldPath &path,
                                             llvm::SMLoc loc) const {
  for (auto it = checker_.streamViews_.rbegin(); it != checker_.streamViews_.rend(); ++it) {
    if (it->path == path && it->from.getPointer() < loc.getPointer()) {
      return it->type;
    }
  }

  return nullptr;
}

const Type *SetEncodingChecker::encodedStreamType(ClassDecl *cls, llvm::SMLoc loc) {
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
    if (ClassDecl *inst =
            generic ? checker_.genericClassInstantiator().instantiateClass(generic, args, loc)
                    : nullptr) {
      return inst->type;
    }
    return nullptr;
  }

  return nullptr;
}

Expr *SetEncodingChecker::checkSetEncoding(CallExpr *call, MemberExpr *member, ClassDecl *cls) {
  const Type *receiver = member->object->type;
  Checker::StreamChunks chunks = checker_.chunkTypes().streamChunksOf(cls);
  llvm::SMLoc loc = member->loc();
  auto plainCall = [&]() -> Expr * {
    checker_.emitterChecker().callLibraryMethod(call, member, cls, "setEncoding", {}, nullptr,
                                                false);
    if (!call->type->isError()) {
      call->type = receiver;
    }

    return call;
  };
  if (!chunks.readable) {
    checker_.diags_.error(loc,
                          "'" + receiver->name() + "' has no setEncoding(): it is not readable");
    return call;
  }
  if (chunks.readable->isString()) {
    return plainCall();
  }
  if (!chunks.readable->isBuffer()) {
    checker_.diags_.error(loc,
                          "setEncoding() decodes the Buffers of a stream of bytes, and this is an "
                          "object-mode stream of " +
                              chunks.readable->name().str() + "; push strings into it instead");
    return call;
  }
  std::optional<Checker::FieldPath> path;
  const Expr *object = member->object;
  while (auto *conv = dyn_cast<CastExpr>(object)) {
    if (!conv->isImplicit()) {
      break;
    }
    object = conv->operand;
  }
  bool stdinStream = false;
  if (auto *c = dyn_cast<CallExpr>(object); c && c->func && c->func->name == "processStdin") {
    if (!checker_.module_ || !checker_.module_->isEntry || !checker_.functions_.empty() ||
        checker_.scopes_.size() != 1) {
      checker_.diags_.error(
          loc, "'process.stdin.setEncoding()' changes the type of the chunks of stdin for "
               "the code after it, so it goes in the top-level code of the program's "
               "main file, before any other use of process.stdin");
      return call;
    }
    stdinStream = true;
    path = Checker::FieldPath{};
    path->var = checker_.streamSources().processStdinVar();
  } else if (auto *name = dyn_cast<NameExpr>(object); name && name->var) {
    path = Checker::FieldPath{};
    path->var = name->var;
  } else {
    path = checker_.fieldFacts().fieldPath(object);
  }
  auto reject = [&](const std::string &why) {
    checker_.diags_.error(
        loc, "setEncoding() makes the chunks of the stream strings from here on, so its "
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
  size_t declared = stdinStream ? 0 : checker_.scopes_.size();
  for (size_t i = checker_.scopes_.size(); i-- > 0 && !stdinStream;) {
    auto found = checker_.scopes_[i].find(root->name);
    if (found != checker_.scopes_[i].end() && found->second.var == root) {
      declared = i;
      break;
    }
  }
  bool branched = stdinStream ? checker_.branchDepth_ != 0
                              : declared < checker_.scopeBranches_.size() &&
                                    checker_.scopeBranches_[declared] != checker_.branchDepth_;
  if (declared + 1 != checker_.scopes_.size() || branched) {
    return reject("but this setEncoding() is in a block of its own (an if, a loop, a function) "
                  "and '" +
                  root->name.str() +
                  "' is declared outside it, so the type would "
                  "depend on whether it runs");
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
        return c->func && c->func->name == "processStdin";
      }
      return checker_.processChecker().isProcessMember(e, "stdin");
    }
    if (path->names.empty()) {
      auto *name = dyn_cast<NameExpr>(e);
      return name && (name->var == root || (!name->var && name->name == root->name));
    }
    std::optional<Checker::FieldPath> p = checker_.fieldFacts().fieldPath(e);

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
  } else if (checker_.module_) {
    for (const Stmt *item : checker_.module_->file.items) {
      body.push_back(item);
    }
  }
  for (const Stmt *stmt : body) {
    visitExprs(stmt, classify);
  }
  llvm::SmallPtrSet<const Expr *, 32> direct;
  for (const Stmt *stmt : body) {
    visitExprs(stmt, [&](const Expr *e) { direct.insert(e); }, false);
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
    checker_.diags_.error(
        loc, "setEncoding() on a stream that is used elsewhere already, which sees its "
             "chunks as Buffers, but strings would come: " +
                 first->how +
                 " before this; give the stream its encoding where it is created, e.g. "
                 "'createReadStream(path, \"utf8\")', or call setEncoding() first");
    checker_.diags_.note(first->loc, "the stream is used here");
    return call;
  }
  const Type *view = encodedStreamType(cls, loc);
  plainCall();
  if (!view || call->type->isError()) {
    return call;
  }
  call->type = view;
  checker_.streamViews_.push_back({*path, llvm::SMLoc::getFromPointer(here), view});

  return call;
}

}
