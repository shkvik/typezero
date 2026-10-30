#include "compiler/library/fs/fs_checker/fs_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/declare/prelude/prelude.h"
#include "compiler/declare/type_declarer/type_declarer.h"
#include "compiler/library/builtin_modules/builtin_module_checker/builtin_module_checker.h"
#include "compiler/library/bytes/bytes_checker/bytes_checker.h"
#include "compiler/library/fs/fs_exports/fs_exports.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

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

}

FsChecker::FsChecker(Checker &checker) : checker_(checker) {}

bool FsChecker::nodeOptions(CallExpr *call, size_t index, llvm::StringRef fn,
                            llvm::ArrayRef<llvm::StringRef> allowed,
                            llvm::StringMap<Expr *> &options) {
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
    checker_.diags_.error(
        arg->loc(),
        "the options of '" + fn + "' are written in the call, as an object literal, e.g. '{ " +
            allowed[0].str() + ": ... }': the type of what it gives may depend on them");
    call->args[index] = checker_.exprChecker().checkExpr(arg);
    return false;
  }
  for (const ObjectLitExpr::Property &prop : lit->props) {
    if (prop.spread || prop.key || prop.method || prop.name.empty()) {
      checker_.diags_.error(prop.loc, "write each option of '" + fn + "' by its name, e.g. '{ " +
                                          allowed[0].str() + ": ... }'");
      return false;
    }
    if (!llvm::is_contained(allowed, prop.name)) {
      checker_.diags_.error(prop.loc,
                            "'" + fn + "' has no option '" + prop.name + "'; it takes " + list);
      return false;
    }
    options[prop.name] = prop.value;
  }

  return true;
}

const Type *FsChecker::nativeError() const {
  TypeAliasDecl *alias = checker_.libTypes_.lookup("NativeError");

  return alias ? checker_.typeDeclarer().resolveAlias(alias) : Type::error();
}

Expr *FsChecker::checkEncoding(Expr *encoding, llvm::StringRef fn, bool any) {
  auto *lit = dyn_cast<StringLitExpr>(encoding);
  if (!lit) {
    checker_.diags_.error(encoding->loc(),
                          "the encoding of '" + fn + "' is written as a literal, 'utf8'");
    checker_.exprChecker().checkExpr(encoding);
    return nullptr;
  }
  lit->type = Type::string();
  Expr *number = checker_.bytesChecker().checkBufferEncoding(lit);
  if (number->type->isError()) {
    return nullptr;
  }
  if (!any && cast<IntLitExpr>(number)->value != 0) {
    checker_.diags_.error(encoding->loc(), "'" + fn +
                                               "' gives names in utf8 only; drop the encoding '" +
                                               utf8(lit->value) + "'");

    return nullptr;
  }

  return number;
}

Expr *FsChecker::checkFsCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc) {
  call->type = Type::error();
  std::string syncName = checker_.fsAsync_ ? (fn + "Sync").str() : fn.str();
  llvm::StringRef op = syncName;
  auto loosely = [&] {
    for (Expr *&arg : call->args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }

    return call;
  };
  if (op == "constants" || op == "Stats" || op == "Dirent") {
    checker_.diags_.error(loc, "'" + fn + "' of 'node:fs' is not a function");
    return loosely();
  }
  if (checker_.callChecker().rejectSpread(call)) {
    return call;
  }
  const Type *string = Type::string();
  auto boolLit = [&](bool value) {
    auto *lit = checker_.ctx_.make<BoolLitExpr>(loc, value);
    lit->type = Type::boolTy();

    return lit;
  };
  auto intLit = [&](uint64_t value) {
    auto *lit = checker_.ctx_.make<IntLitExpr>(loc, value);
    lit->type = Type::intTy();

    return lit;
  };
  auto stringLit = [&](llvm::StringRef text) {
    auto *lit = checker_.ctx_.make<StringLitExpr>(loc, utf16(text));
    lit->type = Type::string();

    return lit;
  };
  auto path = [&](Expr *arg, bool fd) {
    arg = checker_.exprChecker().checkExpr(arg, string);
    if (auto *lit = dyn_cast<StringLitExpr>(arg);
        lit && lit->value.find(u'\0') != std::u16string::npos) {
      checker_.diags_.error(arg->loc(), "the path has a zero character in it, which no file "
                                        "name can have");
    }
    if (fd && !checker_.fsAsync_ && arg->type->isInt()) {
      return arg;
    }

    return checker_.typeRelations().coerce(arg, string);
  };
  auto literalFlag = [&](Expr *value, llvm::StringRef option, bool &out) {
    if (auto *lit = dyn_cast<BoolLitExpr>(value)) {
      lit->type = Type::boolTy();
      out = lit->value;
      return true;
    }
    checker_.diags_.error(value->loc(),
                          "the option '" + option + "' of '" + fn +
                              "' is written as a literal, true or false: the type of what "
                              "it gives depends on it");
    checker_.exprChecker().checkExpr(value);

    return false;
  };
  auto openFlag = [&](Expr *flag, llvm::StringRef fallback) -> Expr * {
    if (!flag) {
      return stringLit(fallback);
    }
    static constexpr llvm::StringLiteral flags[] = {
        "r",   "rs", "sr", "r+", "rs+", "sr+", "w",  "wx",  "xw",  "w+",  "wx+",
        "xw+", "a",  "ax", "xa", "as",  "sa",  "a+", "ax+", "xa+", "as+", "sa+"};
    auto *lit = dyn_cast<StringLitExpr>(flag);
    if (lit && llvm::is_contained(flags, llvm::StringRef(utf8(lit->value)))) {
      lit->type = string;
      return lit;
    }
    checker_.exprChecker().checkExpr(flag);
    std::string known = "'r', 'r+', 'w', 'wx', 'w+', 'wx+', 'a', 'ax', 'a+', 'ax+', and with 's' "
                        "for synchronous writes 'rs', 'rs+', 'as', 'as+'";
    checker_.diags_.error(flag->loc(),
                          lit ? "'" + utf8(lit->value) + "' is not a flag of '" + fn +
                                    "'; the flags are " + known
                              : "the flag of '" + fn + "' is written as a literal: " + known);

    return nullptr;
  };
  llvm::StringMap<Expr *> options;
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
    arg = checker_.exprChecker().checkExpr(arg);
    if (arg->type->isString()) {
      checker_.diags_.error(arg->loc(),
                            "the encoding of '" + fn +
                                "' is written as a literal, 'utf8': the type of what it gives "
                                "depends on it");
    } else if (!arg->type->isError()) {
      checker_.diags_.error(
          arg->loc(),
          "the options of '" + fn + "' are written in the call, as an object literal, e.g. '{ " +
              allowed[0].str() + ": ... }': the type of what it gives may depend on them");
    }

    return false;
  };

  if (op == "readFileSync") {
    if (!checker_.argumentChecker().checkArity(call, loc, 1, 2)) {
      return call;
    }
    Expr *file = path(call->args[0], true);
    if (!encodingOrOptions(1, {"encoding", "flag"})) {
      return call;
    }
    Expr *encoding = options.lookup("encoding");
    Expr *number = nullptr;
    if (encoding && !isa<UndefinedExpr>(encoding) &&
        !(number = checkEncoding(encoding, fn, true))) {
      return call;
    }
    Expr *flag = openFlag(options.lookup("flag"), "r");
    if (!flag) {
      return call;
    }
    call->args = {file, flag};
    call->builtin = Builtin::FsReadFile;
    call->type = number ? string : Type::bytesType(Type::Bytes::Buffer);
    if (number) {
      call->args.push_back(number);
    }
    return call;
  }
  if (op == "writeFileSync" || op == "appendFileSync") {
    if (!checker_.argumentChecker().checkArity(call, loc, 2, 3)) {
      return call;
    }
    Expr *file = path(call->args[0], true);
    Expr *data = checker_.exprChecker().checkExpr(call->args[1], string);
    auto view = [](const Type *t) { return t->isByteView(); };
    bool bytes =
        view(data->type) || (data->type->isUnion() && llvm::all_of(data->type->members(), view));
    if (!data->type->isString() && !bytes && !data->type->isError()) {
      std::string hint =
          data->type->isNumeric() || data->type->isBool() ? "; convert it first, e.g. 'String(x)'"
          : data->type->isBytes() ? "; write a view of it, e.g. 'new Uint8Array(buffer)'"
                                  : "";
      checker_.diags_.error(data->loc(), "'" + fn +
                                             "' writes a string or bytes (a Uint8Array or a "
                                             "Buffer); found '" +
                                             data->type->name() + "'" + hint);
      return loosely();
    }
    if (!encodingOrOptions(2, {"encoding", "flag", "mode"})) {
      return call;
    }
    Expr *number = nullptr;
    if (Expr *encoding = options.lookup("encoding"); encoding && !isa<UndefinedExpr>(encoding)) {
      if (bytes) {
        checker_.diags_.error(encoding->loc(),
                              "an encoding is for text; '" + fn +
                                  "' writes the bytes as they are: drop the encoding");
        return call;
      }
      if (!(number = checkEncoding(encoding, fn, true))) {
        return call;
      }
    }
    Expr *flag = openFlag(options.lookup("flag"), op == "writeFileSync" ? "w" : "a");
    if (!flag) {
      return call;
    }
    Expr *mode = options.lookup("mode");
    call->args = {file, data, flag,
                  mode ? checker_.argumentChecker().checkArg(mode, Type::intTy()) : intLit(0666)};
    if (!bytes) {
      call->args.push_back(number ? number : intLit(0));
    }
    call->builtin = Builtin::FsWriteFile;
    call->type = Type::voidTy();
    return call;
  }
  if (op == "existsSync" || op == "unlinkSync" || op == "realpathSync" || op == "mkdtempSync") {
    unsigned max = op == "existsSync" || op == "unlinkSync" ? 1 : 2;
    if (!checker_.argumentChecker().checkArity(call, loc, 1, max)) {
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
    if (!checker_.argumentChecker().checkArity(call, loc, op == "chmodSync" ? 2 : 1, 2)) {
      return call;
    }
    Expr *file = path(call->args[0], false);
    Expr *value;
    if (op == "truncateSync") {
      value = call->args.size() > 1
                  ? checker_.argumentChecker().checkArg(call->args[1], Type::intTy())
                  : nullptr;
      if (!value) {
        auto *zero = checker_.ctx_.make<IntLitExpr>(loc, 0);
        zero->type = Type::intTy();
        value = zero;
      }
    } else if (op == "chmodSync" && isa<StringLitExpr>(call->args[1])) {
      std::string text = utf8(cast<StringLitExpr>(call->args[1])->value);
      unsigned mode = 0;
      if (text.empty() || llvm::StringRef(text).getAsInteger(8, mode)) {
        checker_.diags_.error(call->args[1]->loc(),
                              "the mode of 'chmodSync' is a number, e.g. 0o644, or "
                              "octal digits in a string, e.g. \"644\"");
        return call;
      }
      value = intLit(mode);
    } else {
      value = call->args.size() > 1
                  ? checker_.argumentChecker().checkArg(call->args[1], Type::intTy())
                  : intLit(0);
    }
    call->args = {file, value};
    call->builtin = op == "accessSync"  ? Builtin::FsAccess
                    : op == "chmodSync" ? Builtin::FsChmod
                                        : Builtin::FsTruncate;
    call->type = Type::voidTy();
    return call;
  }
  if (op == "statSync" || op == "lstatSync") {
    if (!checker_.argumentChecker().checkArity(call, loc, 1, 2)) {
      return call;
    }
    Expr *file = path(call->args[0], false);
    if (!nodeOptions(call, 1, fn,
                     checker_.fsAsync_
                         ? llvm::ArrayRef<llvm::StringRef>{"bigint"}
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
      checker_.diags_.error(options.lookup("bigint")->loc(),
                            "'bigint' is not supported: tz has no bigint; "
                            "the numbers of Stats are doubles");
      return call;
    }
    call->args = {file, boolLit(op == "statSync"), boolLit(throwIfMissing)};
    call->builtin = Builtin::FsStat;
    call->type =
        throwIfMissing ? checker_.statsClass_->type : Type::optional(checker_.statsClass_->type);
    return call;
  }
  if (op == "readdirSync") {
    if (!checker_.argumentChecker().checkArity(call, loc, 1, 2)) {
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
    call->args = {dir, recursive ? checker_.argumentChecker().checkArg(recursive, Type::boolTy())
                                 : boolLit(false)};
    call->builtin = Builtin::FsReaddir;
    call->type = Type::array(types ? checker_.direntClass_->type : string);
    return call;
  }
  if (op == "mkdirSync") {
    if (!checker_.argumentChecker().checkArity(call, loc, 1, 2)) {
      return call;
    }
    Expr *dir = path(call->args[0], false);
    Expr *mode = nullptr;
    if (call->args.size() > 1 && !isa<ObjectLitExpr>(call->args[1]) &&
        !isa<UndefinedExpr>(call->args[1])) {
      mode = checker_.argumentChecker().checkArg(call->args[1], Type::intTy());
    } else {
      if (!nodeOptions(call, 1, fn, {"recursive", "mode"}, options)) {
        return call;
      }
      if (Expr *value = options.lookup("mode")) {
        mode = checker_.argumentChecker().checkArg(value, Type::intTy());
      }
    }
    Expr *recursive = options.lookup("recursive");
    call->args = {dir,
                  recursive ? checker_.argumentChecker().checkArg(recursive, Type::boolTy())
                            : boolLit(false),
                  mode ? mode : intLit(0777)};
    call->builtin = Builtin::FsMkdir;
    call->type = Type::optional(string);
    return call;
  }
  if (op == "rmdirSync") {
    if (!checker_.argumentChecker().checkArity(call, loc, 1, 2)) {
      return call;
    }
    Expr *dir = path(call->args[0], false);
    if (call->args.size() > 1 && !isa<UndefinedExpr>(call->args[1])) {
      checker_.diags_.error(call->args[1]->loc(),
                            "the options of 'rmdirSync' are deprecated in Node; to "
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
    if (!checker_.argumentChecker().checkArity(call, loc, 1, 2)) {
      return call;
    }
    Expr *target = path(call->args[0], false);
    if (!nodeOptions(call, 1, fn, {"recursive", "force", "maxRetries", "retryDelay"}, options)) {
      return call;
    }
    Expr *recursive = options.lookup("recursive");
    Expr *force = options.lookup("force");
    for (llvm::StringRef retry : {"maxRetries", "retryDelay"}) {
      if (Expr *value = options.lookup(retry)) {
        checker_.argumentChecker().checkArg(value, Type::intTy());
      }
    }
    call->args = {
        target,
        recursive ? checker_.argumentChecker().checkArg(recursive, Type::boolTy()) : boolLit(false),
        force ? checker_.argumentChecker().checkArg(force, Type::boolTy()) : boolLit(false)};
    call->builtin = Builtin::FsRm;
    call->type = Type::voidTy();
    return call;
  }
  if (op == "renameSync" || op == "copyFileSync" || op == "replaceSync") {
    if (op == "copyFileSync" && call->args.size() == 3) {
      checker_.diags_.error(call->args[2]->loc(),
                            "'" + fn +
                                "' never replaces a file that is already there, so it takes no "
                                "mode: drop it; to replace a file on purpose, copy it to a "
                                "temporary name and move that over the file with 'replaceSync' "
                                "from \"tz:fs\"");
      return loosely();
    }
    if (!checker_.argumentChecker().checkArity(call, loc, 2, 2)) {
      return call;
    }
    Expr *from = path(call->args[0], false);
    Expr *to = path(call->args[1], false);
    call->args = {from, to};
    call->builtin = op == "renameSync"    ? Builtin::FsRename
                    : op == "replaceSync" ? Builtin::FsReplace
                                          : Builtin::FsCopyFile;
    call->type = Type::voidTy();
    return call;
  }
  checker_.diags_.error(loc, fsExportProblem(fn, "node:fs"));

  return loosely();
}

Expr *FsChecker::checkTzIoCall(CallExpr *call, llvm::SMLoc loc) {
  if (checker_.callChecker().rejectSpread(call) ||
      !checker_.argumentChecker().checkArity(call, loc, 0, 1)) {
    return call;
  }
  if (!call->args.empty()) {
    call->args[0] = checker_.argumentChecker().checkArg(call->args[0], Type::string());
  }
  call->builtin = Builtin::ReadLine;
  call->type = Type::optional(Type::string());

  return call;
}

Expr *FsChecker::checkFsValue(llvm::StringRef name, Expr *expr) {
  if (name != "constants") {
    return nullptr;
  }
  checker_.diags_.error(expr->loc(),
                        "'constants' of 'node:fs' is used as 'constants.F_OK' and the like");

  return expr;
}

Expr *FsChecker::checkFsConstant(MemberExpr *member) {
  auto isFs = [](const Checker::NodeName *n) {
    return n && (n->module == NodeModule::Fs || n->module == NodeModule::FsPromises);
  };
  auto constantOf = [&](const Expr *object) {
    if (auto *name = dyn_cast<NameExpr>(object)) {
      const Checker::NodeName *n = checker_.builtinModuleChecker().nodeName(name->name);
      return isFs(n) && n->name == "constants";
    }
    auto *inner = dyn_cast<MemberExpr>(object);
    auto *name = inner ? dyn_cast<NameExpr>(inner->object) : nullptr;
    const Checker::NodeName *n =
        name ? checker_.builtinModuleChecker().nodeName(name->name) : nullptr;

    return isFs(n) && n->name.empty() && inner->name == "constants";
  };
  if (!constantOf(member->object)) {
    return nullptr;
  }
  static const std::pair<llvm::StringLiteral, int> constants[] = {{"F_OK", 0},
                                                                  {"R_OK", 4},
                                                                  {"W_OK", 2},
                                                                  {"X_OK", 1},
                                                                  {"COPYFILE_EXCL", 1},
                                                                  {"COPYFILE_FICLONE", 2},
                                                                  {"COPYFILE_FICLONE_FORCE", 4}};
  for (const auto &[name, value] : constants) {
    if (name == member->name) {
      auto *lit = checker_.ctx_.make<IntLitExpr>(member->loc(), uint64_t(value));
      lit->type = Type::intTy();
      return lit;
    }
  }
  checker_.diags_.error(member->loc(), "'constants." + member->name +
                                           "' of 'node:fs' is not supported yet; tz has F_OK, "
                                           "R_OK, W_OK, X_OK and the COPYFILE_ ones");
  member->type = Type::error();

  return member;
}

}
