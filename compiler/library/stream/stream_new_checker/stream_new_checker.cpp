#include "compiler/library/stream/stream_new_checker/stream_new_checker.h"
#include "compiler/check/classes/method_call_checker/method_call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/generics/generic_class_instantiator/generic_class_instantiator.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/library/events/event_map_inference/event_map_inference.h"
#include "compiler/library/stream/chunk_types/chunk_types.h"
#include "compiler/library/tz_modules/library_calls/library_calls.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

struct StreamOptionFlags {
  const ObjectLitExpr *lit = nullptr;
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
      return 1;
    }

    return -1;
  }
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

}

StreamNewChecker::StreamNewChecker(Checker &checker) : checker_(checker) {}

Expr *StreamNewChecker::checkStreamNew(NewExpr *expr, ClassDecl *generic, const Type *expected) {
  llvm::StringRef name = generic->name;
  bool readable = name == "Readable";
  bool writable = name == "Writable";
  bool duplex = name == "Duplex" || name == "Transform" || name == "PassThrough";
  if (!readable && !writable && !duplex) {
    return nullptr;
  }
  if (checker_.libraryCalls().inLibrary(expr->loc())) {
    return nullptr;
  }
  if (name != "PassThrough") {
    llvm::StringRef hook = readable           ? "onRead"
                           : writable         ? "onWrite"
                           : name == "Duplex" ? "onRead' and 'onWrite"
                                              : "onTransform";
    std::string example = readable   ? "class Lines extends Readable<string> { protected onRead() "
                                       "{ ... } }"
                          : writable ? "class Sink extends Writable<string> { protected "
                                       "onWrite(chunk, encoding, callback) { ... } }"
                                     : "class Upper extends " + name.str() + "<string> { ... }";
    checker_.diags_.error(expr->loc(),
                          "'" + name.str() +
                              "' is abstract: write a class that extends it and "
                              "puts its work in '" +
                              hook.str() + "', e.g. '" + example + "'" +
                              (readable ? "; for a stream of values you already have, use "
                                          "'Readable.from(values)'"
                                        : ""));
    expr->type = Type::error();
    for (Expr *&arg : expr->args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }

    return expr;
  }
  if (!expr->typeArgs.empty()) {
    return nullptr;
  }
  if (const Type *want = expected ? expected->nonOptional() : nullptr;
      want && ClassDeclarer::classOf(want) && ClassDeclarer::classOf(want)->generic == generic) {
    return nullptr;
  }
  StreamOptionFlags options;
  if (!expr->args.empty()) {
    options.lit = dyn_cast<ObjectLitExpr>(expr->args[0]);
  }
  const Type *buffer = Type::bytesType(Type::Bytes::Buffer);
  std::string site = "stream " + checker_.eventMapInference().placeOf(expr->loc());
  auto side = [&](bool objectMode, bool text, llvm::StringRef key, const Type *fallback,
                  llvm::StringRef what) -> const Type * {
    if (!objectMode) {
      return text ? Type::string() : buffer;
    }
    if (const Type *found =
            checker_.chunkTypes().inferredChunkType(site + " " + key.str(), expr->loc())) {
      return found;
    }
    if (fallback) {
      return fallback;
    }
    checker_.diags_.error(
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
      arg = checker_.exprChecker().checkExpr(arg);
    }

    return expr;
  };
  bool writableObjects = objectMode || options.flag("writableObjectMode") == 1;
  bool readableObjects = objectMode || options.flag("readableObjectMode") == 1;
  const Type *w =
      side(writableObjects, options.flag("decodeStrings") == 0, "W", nullptr, "written chunks");
  if (!w) {
    return failed();
  }
  const Type *r = side(readableObjects, options.has("encoding"), "R", writableObjects ? w : nullptr,
                       "chunks it gives");
  if (!r) {
    return failed();
  }
  std::vector<const Type *> args = {w, r};
  ClassDecl *inst =
      checker_.genericClassInstantiator().instantiateClass(generic, std::move(args), expr->loc());
  if (!inst) {
    return nullptr;
  }

  return checker_.methodCallChecker().checkClassNew(expr, inst, expected);
}

}
