#include "compiler/library/stream/stream_new_checker/stream_new_checker.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/check/classes/method_call_checker/method_call_checker.h"
#include "compiler/check/generics/generic_class_instantiator/generic_class_instantiator.h"
#include "compiler/library/tz_modules/library_calls/library_calls.h"
#include "compiler/library/events/event_map_inference/event_map_inference.h"
#include "compiler/library/stream/chunk_types/chunk_types.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

namespace {

// What the options literal of `new Readable({...})` says of the chunk
// types: an option written as a literal, `true`, `false` or a string.
struct StreamOptionFlags {
  const ObjectLitExpr *lit = nullptr;
  // -1 when the option is not written, 0 or 1 for false or true.
  int flag(llvm::StringRef name) const {
    if (!lit) {
      return -1;
    }
    for (const ObjectLitExpr::Property &prop : lit->props) {
      if (prop.spread || prop.name != name) {
        continue;
      }
      if (auto *b = dyn_cast<BoolLitExpr>(prop.value)) {
        return b->value ? 1 : 0;
      }
      return 1; // a value known only when the program runs
    }

    return -1;
  }
  // Whether a string option is written (not undefined).
  bool has(llvm::StringRef name) const {
    if (!lit) {
      return false;
    }
    for (const ObjectLitExpr::Property &prop : lit->props) {
      if (!prop.spread && prop.name == name && !isa<UndefinedExpr>(prop.value)) {
        return true;
      }
    }

    return false;
  }
};

} // namespace

StreamNewChecker::StreamNewChecker(Sema &sema) : sema_(sema) {}

Expr *StreamNewChecker::checkStreamNew(NewExpr *expr, ClassDecl *generic, const Type *expected) {
  llvm::StringRef name = generic->name;
  bool readable = name == "Readable";
  bool writable = name == "Writable";
  bool duplex = name == "Duplex" || name == "Transform" || name == "PassThrough";
  if (!readable && !writable && !duplex) {
    return nullptr;
  }
  if (!expr->typeArgs.empty() || sema_.libraryCalls().inLibrary(expr->loc())) {
    return nullptr;
  }
  // `const r: Readable<string> = new Readable({...})`: the types of the place
  // it goes.
  if (const Type *want = expected ? expected->nonOptional() : nullptr;
      want && ClassDeclarer::classOf(want) && ClassDeclarer::classOf(want)->generic == generic) {
    return nullptr;
  }
  StreamOptionFlags options;
  if (!expr->args.empty()) {
    options.lit = dyn_cast<ObjectLitExpr>(expr->args[0]);
  }
  const Type *buffer = Type::bytesType(Type::Bytes::Buffer);
  // The type the chunk parameter of a function of the literal is declared
  // with: `write(chunk: User, ...)`, `transform(chunk: Row, ...)`.
  auto annotated = [&](llvm::StringRef fn) -> const Type * {
    if (!options.lit) {
      return nullptr;
    }
    for (const ObjectLitExpr::Property &prop : options.lit->props) {
      auto *fe = prop.name == fn ? dyn_cast<FunctionExpr>(prop.value) : nullptr;
      if (!fe || fe->fn->params.empty()) {
        continue;
      }
      const Param &param = fe->fn->params[0].name == "this" && fe->fn->params.size() > 1
                               ? fe->fn->params[1]
                               : fe->fn->params[0];
      if (param.type) {
        return sema_.typeResolver().resolveType(*param.type, /*allowVoid=*/false);
      }
    }

    return nullptr;
  };
  // The chunk type of a side: of an object-mode side what the program puts
  // into it (see inferredChunkType); a string where it decodes text, a
  // Buffer otherwise.
  std::string site = "stream " + sema_.eventMapInference().placeOf(expr->loc());
  auto side = [&](bool objectMode, bool text, llvm::StringRef key, const Type *fallback,
                  llvm::StringRef what) -> const Type * {
    if (!objectMode) {
      return text ? Type::string() : buffer;
    }
    if (const Type *found = sema_.chunkTypes().inferredChunkType(site + " " + key.str(), expr->loc())) {
      return found;
    }
    if (fallback) {
      return fallback;
    }
    sema_.diags_.error(
        expr->loc(),
        "cannot infer the type of the " + what.str() + " of this object-mode " + name.str() +
            ": no code of the program " + (key == "R" ? "pushes into it" : "writes to it") +
            "; give the type, e.g. 'new " + name.str() + "<User>({ " + "objectMode: true, ... })'");

    return nullptr;
  };
  bool objectMode = options.flag("objectMode") == 1;
  auto failed = [&] {
    expr->type = Type::error();
    for (Expr *&arg : expr->args) {
      arg = sema_.exprChecker().checkExpr(arg);
    }

    return expr;
  };
  std::vector<const Type *> args;
  if (readable) {
    const Type *t = side(objectMode, options.has("encoding"), "R", nullptr, "chunks");
    if (!t) {
      return failed();
    }
    args.push_back(t);
  } else if (writable) {
    const Type *t = objectMode ? annotated("write") : nullptr;
    if (!t) {
      t = side(objectMode, options.flag("decodeStrings") == 0, "W", nullptr, "chunks");
    }
    if (!t) {
      return failed();
    }
    args.push_back(t);
  } else {
    bool writableObjects = objectMode || options.flag("writableObjectMode") == 1;
    bool readableObjects = objectMode || options.flag("readableObjectMode") == 1;
    const Type *w = writableObjects ? annotated(name == "Duplex" ? "write" : "transform") : nullptr;
    if (!w) {
      w = side(writableObjects, options.flag("decodeStrings") == 0, "W", nullptr, "written chunks");
    }
    if (!w) {
      return failed();
    }
    // What a Transform gives is what it takes, unless it pushes others or
    // gives others to the callbacks of transform() and flush().
    const Type *r = side(readableObjects, options.has("encoding"), "R",
                         name == "Duplex" || !writableObjects ? nullptr : w, "chunks it gives");
    if (!r) {
      return failed();
    }
    if (sema_.openMaps_.count(r) && options.lit) {
      for (const ObjectLitExpr::Property &prop : options.lit->props) {
        auto *fe = dyn_cast<FunctionExpr>(prop.value);
        size_t at = prop.name == "transform" ? 2 : prop.name == "flush" ? 0 : ~size_t(0);
        if (fe && at < fe->fn->params.size()) {
          sema_.streamCallbacks_.push_back({fe->fn, fe->fn->params[at].name, r});
        }
      }
    }
    args = {w, r};
  }
  ClassDecl *inst = sema_.genericClassInstantiator().instantiateClass(generic, std::move(args), expr->loc());
  if (!inst) {
    return nullptr;
  }

  return sema_.methodCallChecker().checkClassNew(expr, inst, expected);
}

} // namespace tz
