#include "compiler/library/stream/stream_sources/stream_sources.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/iterators/iterable_checker/iterable_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/declare/prelude/prelude.h"
#include "compiler/declare/type_declarer/type_declarer.h"
#include "compiler/library/events/event_target_checker/event_target_checker.h"
#include "compiler/library/stream/set_encoding_checker/set_encoding_checker.h"
#include "compiler/library/stream/stream_pipeline/stream_pipeline.h"
#include "compiler/library/tz_modules/library_calls/library_calls.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

StreamSources::StreamSources(Checker &checker) : checker_(checker) {}

Expr *StreamSources::checkFsStreamFunction(CallExpr *call, FunctionDecl *decl, llvm::SMLoc loc) {
  llvm::StringRef fn = decl->name;
  bool reading = fn == "createReadStream";
  if (!reading && fn != "createWriteStream") {
    return nullptr;
  }
  if (checker_.libraryCalls().inLibrary(call->loc())) {
    return nullptr;
  }
  call->type = Type::error();
  if (checker_.callChecker().rejectSpread(call) ||
      !checker_.argumentChecker().checkArity(call, loc, 1, 2)) {
    return call;
  }
  TypeAliasDecl *alias =
      checker_.libTypes_.lookup(reading ? "ReadStreamOptions" : "WriteStreamOptions");
  if (!alias) {
    return call;
  }
  const Type *options = Type::optional(checker_.typeDeclarer().resolveAlias(alias));
  const Type *encodingType = Type::optional(checker_.prelude().builtinType("BufferEncoding"));
  auto undefinedOf = [&](const Type *type) {
    return checker_.typeRelations().convert(
        checker_.exprChecker().checkExpr(checker_.ctx_.make<UndefinedExpr>(loc)), type);
  };
  Expr *path = checker_.argumentChecker().checkArg(call->args[0], Type::string());
  Expr *opts = nullptr;
  Expr *encoding = nullptr;
  bool text = false;
  if (call->args.size() == 2) {
    Expr *arg = call->args[1];
    if (isa<ObjectLitExpr>(arg)) {
      auto *lit = cast<ObjectLitExpr>(arg);
      for (const ObjectLitExpr::Property &prop : lit->props) {
        if (!prop.spread && prop.name == "encoding" && !isa<UndefinedExpr>(prop.value)) {
          text = true;
        }
      }
      opts = checker_.argumentChecker().checkArg(arg, options);
    } else if (isa<StringLitExpr>(arg)) {
      encoding = checker_.argumentChecker().checkArg(arg, encodingType);
      text = true;
    } else {
      arg = checker_.exprChecker().checkExpr(arg);
      if (arg->type->isSubtypeOf(Type::string())) {
        encoding = checker_.typeRelations().coerce(arg, encodingType);
        text = true;
      } else {
        opts = checker_.typeRelations().coerce(arg, options);
      }
    }
  }
  if (!opts) {
    opts = undefinedOf(options);
  }
  if (!encoding) {
    encoding = undefinedOf(encodingType);
  }
  if (path->type->isError() || opts->type->isError() || encoding->type->isError()) {
    return call;
  }
  call->args = {path, opts, encoding};
  std::vector<bool> given(3, true);
  if (!reading) {
    return checker_.eventTargetChecker().callLibraryFunction(call, "streamWriteFile", {}, given,
                                                             loc);
  }
  const Type *chunk = text ? Type::string() : Type::bytesType(Type::Bytes::Buffer);

  return checker_.eventTargetChecker().callLibraryFunction(call, "streamReadFile", {chunk}, given,
                                                           loc);
}

Expr *StreamSources::processStream(MemberExpr *member) {
  llvm::StringRef name = member->name;
  bool in = name == "stdin";
  llvm::StringRef fn = in ? "processStdin" : "processStdout";
  if (!checker_.libFunctions_.lookup(fn)) {
    return nullptr;
  }
  llvm::SMLoc loc = member->loc();
  std::vector<Expr *> args;
  if (!in) {
    args.push_back(checker_.exprChecker().checkExpr(
        checker_.ctx_.make<IntLitExpr>(loc, name == "stdout" ? 1 : 2), Type::intTy()));
  }
  auto *callee = checker_.ctx_.make<NameExpr>(loc, fn);
  auto *call = checker_.ctx_.make<CallExpr>(loc, callee, std::move(args));
  call->type = Type::error();
  std::vector<bool> given(call->args.size(), true);
  Expr *stream = checker_.eventTargetChecker().callLibraryFunction(call, fn, {}, given, loc);
  if (in && !checker_.streamViews_.empty()) {
    Checker::FieldPath path;
    path.var = processStdinVar();
    if (const Type *view = checker_.setEncodingChecker().streamViewOf(path, loc)) {
      return checker_.typeRelations().convert(stream, view);
    }
  }

  return stream;
}

VarSymbol *StreamSources::processStdinVar() {
  if (!processStdin_) {
    processStdin_ = checker_.ctx_.newVar();
    processStdin_->name = "process.stdin";
    processStdin_->isConst = true;
  }

  return processStdin_;
}

const Type *StreamSources::streamFromOptionsType() {
  TypeAliasDecl *alias = checker_.libTypes_.lookup("ReadableFromOptions");

  return alias ? checker_.typeDeclarer().resolveAlias(alias) : nullptr;
}

Expr *StreamSources::checkStreamFrom(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  if (checker_.callChecker().rejectSpread(call) ||
      !checker_.argumentChecker().checkArity(call, member->loc(), 1, 2)) {
    return call;
  }
  llvm::SMLoc loc = member->loc();
  Expr *source = checker_.exprChecker().checkExpr(call->args[0]);
  const Type *type = source->type;
  if (type->isError()) {
    return call;
  }
  const Type *optionsType = streamFromOptionsType();
  if (!optionsType) {
    return call;
  }
  Expr *options =
      call->args.size() > 1
          ? checker_.argumentChecker().checkArg(call->args[1], Type::optional(optionsType))
          : checker_.typeRelations().convert(
                checker_.exprChecker().checkExpr(checker_.ctx_.make<UndefinedExpr>(loc)),
                Type::optional(optionsType));
  if (options->type->isError()) {
    return call;
  }
  std::vector<bool> given{true, true};
  if (type->isString() || type->isStringLiteral() || type->isBuffer()) {
    const Type *chunk = type->isBuffer() ? type : Type::string();
    call->args = {checker_.typeRelations().coerce(source, chunk), options};
    return checker_.eventTargetChecker().callLibraryFunction(call, "streamFromValue", {chunk},
                                                             given, loc);
  }
  if (type->isAsyncIterator()) {
    call->args = {source, options};
    return checker_.eventTargetChecker().callLibraryFunction(call, "streamFromAsyncIterator",
                                                             {type->yielded()}, given, loc);
  }
  if (Expr *async = checker_.iterableChecker().asyncUserIterator(source);
      async && async->type->isAsyncIterator()) {
    call->args = {async, options};
    return checker_.eventTargetChecker().callLibraryFunction(call, "streamFromAsyncIterator",
                                                             {async->type->yielded()}, given, loc);
  }
  Expr *iterator = checker_.streamPipeline().valuesIteratorOf(source);
  if (!iterator) {
    checker_.diags_.error(source->loc(),
                          "'Readable.from' takes an iterable, an async iterable, a string "
                          "or a Buffer, but this is '" +
                              type->name() + "'");
    return call;
  }
  call->args = {iterator, options};

  return checker_.eventTargetChecker().callLibraryFunction(call, "streamFromIterator",
                                                           {iterator->type->yielded()}, given, loc);
}

}
