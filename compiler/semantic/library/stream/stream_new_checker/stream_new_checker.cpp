#include "compiler/semantic/library/stream/stream_new_checker/stream_new_checker.h"
#include "compiler/semantic/bodies/classes/method_call_checker/method_call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/generics/generic_class_instantiator/generic_class_instantiator.h"
#include "compiler/semantic/library/events/event_map_inference/event_map_inference.h"
#include "compiler/semantic/library/stream/chunk_types/chunk_types.h"
#include "compiler/semantic/library/tz_modules/library_calls/library_calls.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;
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
      if (prop.spread() || prop.name != name) {
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
      if (!prop.spread() && prop.name == name && !isa<UndefinedExpr>(prop.value)) {
        return true;
      }
    }

    return false;
  }
};

}

StreamNewChecker::StreamNewChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->chunkTypes = &all.getChunkTypes();
  this->exprChecker = &all.getExprChecker();
  this->libraryCalls = &all.getLibraryCalls();
  this->eventMapInference = &all.getEventMapInference();
  this->methodCallChecker = &all.getMethodCallChecker();
  this->genericClassInstantiator = &all.getGenericClassInstantiator();
}

StreamNewChecker::ResultConcreteStream
StreamNewChecker::concreteStream(llvm::StringRef name, bool readable, bool writable) {
  if (name != "PassThrough") {
    std::string message = ErrorTextStreamNew::abstract(name, readable, writable);

    return err(StreamNewCheckerConcreteStreamError{
        .code = StreamNewCheckerConcreteStreamErrorCode::ABSTRACT_STREAM,
        .message = message,
    });
  }

  return ok(name);
}

Expr *StreamNewChecker::checkStreamNew(NewExpr *expr, ClassDecl *generic, const Type *expected) {
  llvm::StringRef name = generic->name;
  bool readable = name == "Readable";
  bool writable = name == "Writable";
  bool duplex = name == "Duplex" || name == "Transform" || name == "PassThrough";
  if (!readable && !writable && !duplex) {
    return nullptr;
  }
  if (this->libraryCalls->inLibrary(expr->loc())) {
    return nullptr;
  }
  ResultConcreteStream concrete = StreamNewChecker::concreteStream(name, readable, writable);
  if (concrete.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), concrete.error().message);
    expr->type = Type::error();
    for (Expr *&arg : expr->args) {
      arg = this->exprChecker->checkExpr(arg);
    }

    return expr;
  }
  if (!expr->typeArgs.empty()) {
    return nullptr;
  }
  if (const Type *want = expected ? expected->nonOptional() : nullptr;
      want && ClassDeclarer::classOf(want) && ClassDeclarer::classOf(want)->generic() == generic) {
    return nullptr;
  }
  StreamOptionFlags options;
  if (!expr->args.empty()) {
    options.lit = dyn_cast<ObjectLitExpr>(expr->args[0]);
  }
  const Type *buffer = Type::bytesType(Type::Bytes::Buffer);
  std::string site = "stream " + this->eventMapInference->placeOf(expr->loc());
  auto side = [&](bool objectMode, bool text, llvm::StringRef key, const Type *fallback,
                  llvm::StringRef what) -> const Type * {
    if (!objectMode) {
      return text ? Type::string() : buffer;
    }
    if (const Type *found =
            this->chunkTypes->inferredChunkType(site + " " + key.str(), expr->loc())) {
      return found;
    }
    if (fallback) {
      return fallback;
    }
    std::string message = ErrorTextStreamNew::untyped(what, name, key == "R");
    this->state.getDiagnostics().error(expr->loc(), message);

    return nullptr;
  };
  bool objectMode = options.flag("objectMode") == 1;
  auto failed = [&] {
    expr->type = Type::error();
    for (Expr *&arg : expr->args) {
      arg = this->exprChecker->checkExpr(arg);
    }

    return expr;
  };
  bool writableObjects = objectMode || options.flag("writableObjectMode") == 1;
  bool readableObjects = objectMode || options.flag("readableObjectMode") == 1;
  std::string written = ErrorTextStreamSide::written();
  const Type *w = side(writableObjects, options.flag("decodeStrings") == 0, "W", nullptr, written);
  if (!w) {
    return failed();
  }
  std::string given = ErrorTextStreamSide::given();
  const Type *r =
      side(readableObjects, options.has("encoding"), "R", writableObjects ? w : nullptr, given);
  if (!r) {
    return failed();
  }
  std::vector<const Type *> args = {w, r};
  GenericClassInstantiator::ResultClassMade made =
      this->genericClassInstantiator->instantiateClass(generic, std::move(args), expr->loc());
  if (made.isErr() &&
      made.error().code != GenericClassInstantiatorInstantiateClassErrorCode::ALREADY_REPORTED) {
    llvm::SMLoc at =
        made.error().code == GenericClassInstantiatorInstantiateClassErrorCode::ENDLESS_INSTANCES
            ? generic->loc()
            : expr->loc();
    this->state.getDiagnostics().error(at, made.error().message);
  }
  if (made.isErr()) {
    return nullptr;
  }

  return this->methodCallChecker->checkClassNew(expr, made.value(), expected);
}

}
