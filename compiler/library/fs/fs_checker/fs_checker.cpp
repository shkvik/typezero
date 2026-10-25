#include "compiler/library/fs/fs_checker/fs_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/library/bytes/bytes_checker/bytes_checker.h"
#include "compiler/library/builtin_modules/runtime_object_checker/runtime_object_checker.h"
#include "compiler/library/builtin_modules/node_check_helpers/node_check_helpers.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

namespace {

std::u16string utf16(llvm::StringRef text) {
  llvm::SmallVector<llvm::UTF16, 32> units;
  llvm::convertUTF8ToUTF16String(text, units);

  return std::u16string(units.begin(), units.end());
}

std::string utf8(const std::u16string &text) {
  std::string out;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
      out);

  return out;
}

} // namespace

FsChecker::FsChecker(Sema &sema) : sema_(sema) {}

bool FsChecker::nodeOptions(CallExpr *call, size_t index, llvm::StringRef fn,
                       llvm::ArrayRef<llvm::StringRef> allowed, llvm::StringMap<Expr *> &options) {
  if (index >= call->args.size() || isa<UndefinedExpr>(call->args[index])) {
    return true;
  }
  std::string list;
  for (size_t i = 0; i < allowed.size(); ++i) {
    list += (i == 0 ? "" : i + 1 == allowed.size() ? " and " : ", ") + allowed[i].str();
  }
  Expr *arg = call->args[index];
  auto *lit = dyn_cast<ObjectLitExpr>(arg);
  if (!lit) {
    sema_.diags_.error(arg->loc(), "the options of '" + fn +
                                 "' are written in the call, as an object literal, e.g. '{ " +
                                 allowed[0].str() +
                                 ": ... }': the type of what it gives may depend on them");
    call->args[index] = sema_.exprChecker().checkExpr(arg);
    return false;
  }
  for (const ObjectLitExpr::Property &prop : lit->props) {
    if (prop.spread || prop.key || prop.method || prop.name.empty()) {
      sema_.diags_.error(prop.loc, "write each option of '" + fn + "' by its name, e.g. '{ " +
                                 allowed[0].str() + ": ... }'");
      return false;
    }
    if (!llvm::is_contained(allowed, prop.name)) {
      sema_.diags_.error(prop.loc, "'" + fn + "' has no option '" + prop.name + "'; it takes " + list);
      return false;
    }
    options[prop.name] = prop.value;
  }

  return true;
}

Expr *FsChecker::checkEncoding(Expr *encoding, llvm::StringRef fn, bool any) {
  auto *lit = dyn_cast<StringLitExpr>(encoding);
  if (!lit) {
    sema_.diags_.error(encoding->loc(), "the encoding of '" + fn + "' is written as a literal, 'utf8'");
    sema_.exprChecker().checkExpr(encoding);
    return nullptr;
  }
  lit->type = Type::string();
  Expr *number = sema_.bytesChecker().checkBufferEncoding(lit);
  if (number->type->isError()) {
    return nullptr;
  }
  if (!any && cast<IntLitExpr>(number)->value != 0) {
    sema_.diags_.error(encoding->loc(), "'" + fn + "' gives names in utf8 only; drop the encoding '" +
                                      utf8(lit->value) + "'");

    return nullptr;
  }

  return number;
}

Expr *FsChecker::checkFsCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc) {
  call->type = Type::error();
  // node:fs/promises checks its functions as those of node:fs that end in
  // Sync: `op` is that name, `fn` the one written.
  std::string syncName = sema_.fsAsync_ ? (fn + "Sync").str() : fn.str();
  llvm::StringRef op = syncName;
  auto loosely = [&] {
    for (Expr *&arg : call->args) {
      arg = sema_.exprChecker().checkExpr(arg);
    }

    return call;
  };
  if (op == "constants" || op == "Stats" || op == "Dirent") {
    sema_.diags_.error(loc, "'" + fn + "' of 'node:fs' is not a function");
    return loosely();
  }
  if (sema_.callChecker().rejectSpread(call)) {
    return call;
  }
  const Type *string = Type::string();
  auto boolLit = [&](bool value) {
    auto *lit = sema_.ctx_.make<BoolLitExpr>(loc, value);
    lit->type = Type::boolTy();

    return lit;
  };
  auto intLit = [&](uint64_t value) {
    auto *lit = sema_.ctx_.make<IntLitExpr>(loc, value);
    lit->type = Type::intTy();

    return lit;
  };
  auto stringLit = [&](llvm::StringRef text) {
    auto *lit = sema_.ctx_.make<StringLitExpr>(loc, utf16(text));
    lit->type = Type::string();

    return lit;
  };
  // A path, or with `fd`, an int file descriptor too (0 is stdin); those of
  // node:fs/promises take a FileHandle instead.
  auto path = [&](Expr *arg, bool fd) {
    arg = sema_.exprChecker().checkExpr(arg, string);
    if (fd && !sema_.fsAsync_ && arg->type->isInt()) {
      return arg;
    }

    return sema_.typeRelations().coerce(arg, string);
  };
  // A boolean option that decides the type, which must be a literal.
  auto literalFlag = [&](Expr *value, llvm::StringRef option, bool &out) {
    if (auto *lit = dyn_cast<BoolLitExpr>(value)) {
      lit->type = Type::boolTy();
      out = lit->value;
      return true;
    }
    sema_.diags_.error(value->loc(), "the option '" + option + "' of '" + fn +
                                   "' is written as a literal, true or false: the type of what "
                                   "it gives depends on it");
    sema_.exprChecker().checkExpr(value);

    return false;
  };
  llvm::StringMap<Expr *> options;
  // The argument at `index`: the encoding alone, or the options.
  auto encodingOrOptions = [&](size_t index, llvm::ArrayRef<llvm::StringRef> allowed) {
    if (index >= call->args.size()) {
      return true;
    }
    Expr *arg = call->args[index];
    if (isa<StringLitExpr>(arg)) {
      options["encoding"] = arg;
      return true;
    }
    if (isa<ObjectLitExpr>(arg) || isa<UndefinedExpr>(arg)) {
      return nodeOptions(call, index, fn, allowed, options);
    }
    arg = sema_.exprChecker().checkExpr(arg);
    if (arg->type->isString()) {
      sema_.diags_.error(arg->loc(), "the encoding of '" + fn +
                                   "' is written as a literal, 'utf8': the type of what it gives "
                                   "depends on it");
    } else if (!arg->type->isError()) {
      sema_.diags_.error(arg->loc(), "the options of '" + fn +
                                   "' are written in the call, as an object literal, e.g. '{ " +
                                   allowed[0].str() +
                                   ": ... }': the type of what it gives may depend on them");
    }

    return false;
  };

  if (op == "readFileSync") {
    if (!sema_.argumentChecker().checkArity(call, loc, 1, 2)) {
      return call;
    }
    Expr *file = path(call->args[0], /*fd=*/true);
    using Names = llvm::ArrayRef<llvm::StringRef>;
    if (!encodingOrOptions(1, sema_.fsAsync_ ? Names{"encoding", "flag", "signal"}
                                       : Names{"encoding", "flag"})) {
      return call;
    }
    Expr *encoding = options.lookup("encoding");
    Expr *number = nullptr;
    if (encoding && !isa<UndefinedExpr>(encoding) &&
        !(number = checkEncoding(encoding, fn, true))) {
      return call;
    }
    Expr *flag = options.lookup("flag");
    call->args = {file, flag ? sema_.argumentChecker().checkArg(flag, string) : stringLit("r")};
    if (sema_.fsAsync_) {
      call->args.push_back(sema_.runtimeObjectChecker().signalOption(options.lookup("signal"), loc));
    }
    call->builtin = Builtin::FsReadFile;
    // Without an encoding, the bytes, as Node gives them: a Buffer.
    call->type = number ? string : Type::bytesType(Type::Bytes::Buffer);
    if (number) {
      call->args.push_back(number);
    }
    return call;
  }
  if (op == "writeFileSync" || op == "appendFileSync") {
    if (!sema_.argumentChecker().checkArity(call, loc, 2, 3)) {
      return call;
    }
    Expr *file = path(call->args[0], /*fd=*/true);
    Expr *data = sema_.exprChecker().checkExpr(call->args[1], string);
    // Text, or the bytes of a typed array, a Buffer or a DataView (or of a
    // union of them, a pointer to one as well).
    auto view = [](const Type *t) { return t->isByteView(); };
    bool bytes =
        view(data->type) || (data->type->isUnion() && llvm::all_of(data->type->members(), view));
    if (!data->type->isString() && !bytes && !data->type->isError()) {
      std::string hint =
          data->type->isNumeric() || data->type->isBool() ? "; convert it first, e.g. 'String(x)'"
          : data->type->isBytes() ? "; write a view of it, e.g. 'new Uint8Array(buffer)'"
                                  : "";
      sema_.diags_.error(data->loc(), "'" + fn +
                                    "' writes a string or bytes (a Uint8Array or a "
                                    "Buffer); found '" +
                                    data->type->name() + "'" + hint);
      return loosely();
    }
    using Names = llvm::ArrayRef<llvm::StringRef>;
    if (!encodingOrOptions(2, sema_.fsAsync_ ? Names{"encoding", "flag", "mode", "signal"}
                                       : Names{"encoding", "flag", "mode"})) {
      return call;
    }
    Expr *number = nullptr;
    if (Expr *encoding = options.lookup("encoding"); encoding && !isa<UndefinedExpr>(encoding)) {
      if (bytes) {
        // Node ignores it: the bytes are written as they are.
        sema_.diags_.error(encoding->loc(), "an encoding is for text; '" + fn +
                                          "' writes the bytes as they are: drop the encoding");
        return call;
      }
      if (!(number = checkEncoding(encoding, fn, true))) {
        return call;
      }
    }
    Expr *flag = options.lookup("flag");
    Expr *mode = options.lookup("mode");
    call->args = {file, data,
                  flag ? sema_.argumentChecker().checkArg(flag, string) : stringLit(op == "writeFileSync" ? "w" : "a"),
                  mode ? sema_.argumentChecker().checkArg(mode, Type::intTy()) : intLit(0666)};
    if (!bytes) {
      call->args.push_back(number ? number : intLit(0)); // utf8
    }
    if (sema_.fsAsync_) {
      call->args.push_back(sema_.runtimeObjectChecker().signalOption(options.lookup("signal"), loc));
    }
    call->builtin = Builtin::FsWriteFile;
    call->type = Type::voidTy();
    return call;
  }
  if (op == "existsSync" || op == "unlinkSync" || op == "realpathSync" || op == "mkdtempSync") {
    unsigned max = op == "existsSync" || op == "unlinkSync" ? 1 : 2;
    if (!sema_.argumentChecker().checkArity(call, loc, 1, max)) {
      return call;
    }
    Expr *file = path(call->args[0], false);
    if (max == 2 && !encodingOrOptions(1, {"encoding"})) {
      return call;
    }
    if (Expr *encoding = options.lookup("encoding"); encoding && !isa<UndefinedExpr>(encoding)) {
      if (!checkEncoding(encoding, fn, false)) {
        return call;
      }
    }
    call->args = {file};
    call->builtin = op == "existsSync"     ? Builtin::FsExists
                    : op == "unlinkSync"   ? Builtin::FsUnlink
                    : op == "realpathSync" ? Builtin::FsRealpath
                                           : Builtin::FsMkdtemp;
    call->type = op == "existsSync" ? Type::boolTy() : op == "unlinkSync" ? Type::voidTy() : string;
    return call;
  }
  if (op == "accessSync" || op == "chmodSync" || op == "truncateSync") {
    if (!sema_.argumentChecker().checkArity(call, loc, op == "chmodSync" ? 2 : 1, 2)) {
      return call;
    }
    Expr *file = path(call->args[0], false);
    Expr *value;
    if (op == "truncateSync") {
      value = call->args.size() > 1 ? sema_.argumentChecker().checkArg(call->args[1], Type::longTy()) : nullptr;
      if (!value) {
        auto *zero = sema_.ctx_.make<IntLitExpr>(loc, 0);
        zero->type = Type::longTy();
        value = zero;
      }
    } else if (op == "chmodSync" && isa<StringLitExpr>(call->args[1])) {
      // An octal string, as Node takes it: "755".
      std::string text = utf8(cast<StringLitExpr>(call->args[1])->value);
      unsigned mode = 0;
      if (text.empty() || llvm::StringRef(text).getAsInteger(8, mode)) {
        sema_.diags_.error(call->args[1]->loc(), "the mode of 'chmodSync' is a number, e.g. 0o644, or "
                                           "octal digits in a string, e.g. \"644\"");
        return call;
      }
      value = intLit(mode);
    } else {
      value = call->args.size() > 1 ? sema_.argumentChecker().checkArg(call->args[1], Type::intTy()) : intLit(0);
    }
    call->args = {file, value};
    call->builtin = op == "accessSync"  ? Builtin::FsAccess
                    : op == "chmodSync" ? Builtin::FsChmod
                                        : Builtin::FsTruncate;
    call->type = Type::voidTy();
    return call;
  }
  if (op == "statSync" || op == "lstatSync") {
    if (!sema_.argumentChecker().checkArity(call, loc, 1, 2)) {
      return call;
    }
    Expr *file = path(call->args[0], false);
    // Only statSync has throwIfNoEntry.
    if (!nodeOptions(call, 1, fn,
                     sema_.fsAsync_ ? llvm::ArrayRef<llvm::StringRef>{"bigint"}
                              : llvm::ArrayRef<llvm::StringRef>{"throwIfNoEntry", "bigint"},
                     options)) {
      return call;
    }
    bool throwIfMissing = true, big = false;
    if (Expr *value = options.lookup("throwIfNoEntry");
        value && !literalFlag(value, "throwIfNoEntry", throwIfMissing)) {
      return call;
    }
    if (Expr *value = options.lookup("bigint"); value && !literalFlag(value, "bigint", big)) {
      return call;
    }
    if (big) {
      sema_.diags_.error(options.lookup("bigint")->loc(), "'bigint' is not supported: tz has no bigint; "
                                                    "the numbers of Stats are doubles");
      return call;
    }
    call->args = {file, boolLit(op == "statSync"), boolLit(throwIfMissing)};
    call->builtin = Builtin::FsStat;
    call->type = throwIfMissing ? sema_.statsClass_->type : Type::optional(sema_.statsClass_->type);
    return call;
  }
  if (op == "readdirSync") {
    if (!sema_.argumentChecker().checkArity(call, loc, 1, 2)) {
      return call;
    }
    Expr *dir = path(call->args[0], false);
    if (!encodingOrOptions(1, {"encoding", "withFileTypes", "recursive"})) {
      return call;
    }
    if (Expr *encoding = options.lookup("encoding"); encoding && !isa<UndefinedExpr>(encoding)) {
      if (!checkEncoding(encoding, fn, false)) {
        return call;
      }
    }
    bool types = false;
    if (Expr *value = options.lookup("withFileTypes");
        value && !literalFlag(value, "withFileTypes", types)) {
      return call;
    }
    Expr *recursive = options.lookup("recursive");
    call->args = {dir, recursive ? sema_.argumentChecker().checkArg(recursive, Type::boolTy()) : boolLit(false)};
    call->builtin = Builtin::FsReaddir;
    call->type = Type::array(types ? sema_.direntClass_->type : string);
    return call;
  }
  if (op == "mkdirSync") {
    if (!sema_.argumentChecker().checkArity(call, loc, 1, 2)) {
      return call;
    }
    Expr *dir = path(call->args[0], false);
    Expr *mode = nullptr;
    if (call->args.size() > 1 && !isa<ObjectLitExpr>(call->args[1]) &&
        !isa<UndefinedExpr>(call->args[1])) {
      mode = sema_.argumentChecker().checkArg(call->args[1], Type::intTy()); // mkdirSync(path, 0o755)
    } else {
      if (!nodeOptions(call, 1, fn, {"recursive", "mode"}, options)) {
        return call;
      }
      if (Expr *value = options.lookup("mode")) {
        mode = sema_.argumentChecker().checkArg(value, Type::intTy());
      }
    }
    Expr *recursive = options.lookup("recursive");
    // The first directory made, with `recursive`; undefined otherwise, as
    // in Node.
    call->args = {dir, recursive ? sema_.argumentChecker().checkArg(recursive, Type::boolTy()) : boolLit(false),
                  mode ? mode : intLit(0777)};
    call->builtin = Builtin::FsMkdir;
    call->type = Type::optional(string);
    return call;
  }
  if (op == "rmdirSync") {
    if (!sema_.argumentChecker().checkArity(call, loc, 1, 2)) {
      return call;
    }
    Expr *dir = path(call->args[0], false);
    if (call->args.size() > 1 && !isa<UndefinedExpr>(call->args[1])) {
      sema_.diags_.error(call->args[1]->loc(), "the options of 'rmdirSync' are deprecated in Node; to "
                                         "delete a directory with what is in it, use 'rmSync(path, "
                                         "{ recursive: true })'");
      return loosely();
    }
    call->args = {dir};
    call->builtin = Builtin::FsRmdir;
    call->type = Type::voidTy();
    return call;
  }
  if (op == "rmSync") {
    if (!sema_.argumentChecker().checkArity(call, loc, 1, 2)) {
      return call;
    }
    Expr *target = path(call->args[0], false);
    if (!nodeOptions(call, 1, fn, {"recursive", "force", "maxRetries", "retryDelay"}, options)) {
      return call;
    }
    Expr *recursive = options.lookup("recursive");
    Expr *force = options.lookup("force");
    // Retries are for systems where another program may hold a file: the
    // values are checked, and nothing waits.
    for (llvm::StringRef retry : {"maxRetries", "retryDelay"}) {
      if (Expr *value = options.lookup(retry)) {
        sema_.argumentChecker().checkArg(value, Type::intTy());
      }
    }
    call->args = {target, recursive ? sema_.argumentChecker().checkArg(recursive, Type::boolTy()) : boolLit(false),
                  force ? sema_.argumentChecker().checkArg(force, Type::boolTy()) : boolLit(false)};
    call->builtin = Builtin::FsRm;
    call->type = Type::voidTy();
    return call;
  }
  if (op == "renameSync" || op == "copyFileSync") {
    if (!sema_.argumentChecker().checkArity(call, loc, 2, op == "renameSync" ? 2 : 3)) {
      return call;
    }
    Expr *from = path(call->args[0], false);
    Expr *to = path(call->args[1], false);
    if (op == "renameSync") {
      call->args = {from, to};
      call->builtin = Builtin::FsRename;
    } else {
      Expr *mode = call->args.size() > 2 ? sema_.argumentChecker().checkArg(call->args[2], Type::intTy()) : intLit(0);
      call->args = {from, to, mode};
      call->builtin = Builtin::FsCopyFile;
    }
    call->type = Type::voidTy();
    return call;
  }
  sema_.diags_.error(loc, exportProblem(NodeModule::Fs, fn, "node:fs"));

  return loosely();
}

} // namespace tz
