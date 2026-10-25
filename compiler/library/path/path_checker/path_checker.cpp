#include "compiler/library/path/path_checker/path_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/library/builtin_modules/node_check_helpers/node_check_helpers.h"
#include "compiler/whole_program/flows/flows.h"
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

bool targetIsWindows() { return llvm::Triple(llvm::sys::getDefaultTargetTriple()).isOSWindows(); }

const Type *parsedPathType() {
  const Type *s = Type::string();

  return Type::object({{"root", s, false},
                       {"dir", s, false},
                       {"base", s, false},
                       {"ext", s, false},
                       {"name", s, false}},
                      "ParsedPath");
}

const Type *formatInputType() {
  const Type *s = Type::optional(Type::string());

  return Type::object({{"root", s, false},
                       {"dir", s, false},
                       {"base", s, false},
                       {"ext", s, false},
                       {"name", s, false}},
                      "FormatInputPathObject");
}

} // namespace

PathChecker::PathChecker(Checker &checker) : checker_(checker) {}

Expr *PathChecker::checkPathCall(CallExpr *call, NodeModule module, llvm::StringRef fn,
                                 llvm::SMLoc loc) {
  call->type = Type::error();
  auto loosely = [&] {
    for (Expr *&arg : call->args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }

    return call;
  };
  if (fn == "sep" || fn == "delimiter") {
    checker_.diags_.error(loc, "'" + fn + "' is a string, not a function");
    return loosely();
  }
  if (fn == "posix" || fn == "win32") {
    checker_.diags_.error(loc, "'" + fn + "' is not a function: call its functions, e.g. '" + fn +
                                   ".join(a, b)'");
    return loosely();
  }
  bool win32 = module == NodeModule::PathWin32 || (module == NodeModule::Path && targetIsWindows());
  auto *style = checker_.ctx_.make<IntLitExpr>(loc, win32 ? 1 : 0);
  style->type = Type::intTy();
  const Type *string = Type::string();

  if (fn == "join" || fn == "resolve") {
    // The paths become one array, spread ones too: `join(...parts)`.
    auto *paths = checker_.ctx_.make<ArrayLitExpr>(call->args.empty() ? loc : call->args[0]->loc());
    for (size_t i = 0; i < call->args.size(); ++i) {
      paths->elements.push_back({call->args[i], call->spreads(i)});
    }
    Expr *array = checker_.argumentChecker().checkArg(paths, Type::array(string));
    call->args = {style, array};
    call->spread.clear();
    call->builtin = fn == "join" ? Builtin::PathJoin : Builtin::PathResolve;
    call->type = string;
    return call;
  }
  if (checker_.callChecker().rejectSpread(call)) {
    return call;
  }
  struct Shape {
    llvm::StringLiteral name;
    Builtin builtin;
    unsigned min, max;
  };
  static const Shape shapes[] = {
      {"normalize", Builtin::PathNormalize, 1, 1},
      {"isAbsolute", Builtin::PathIsAbsolute, 1, 1},
      {"dirname", Builtin::PathDirname, 1, 1},
      {"basename", Builtin::PathBasename, 1, 2},
      {"extname", Builtin::PathExtname, 1, 1},
      {"relative", Builtin::PathRelative, 2, 2},
      {"parse", Builtin::PathParse, 1, 1},
      {"format", Builtin::PathFormat, 1, 1},
      {"toNamespacedPath", Builtin::PathToNamespacedPath, 1, 1},
  };
  const Shape *shape = llvm::find_if(shapes, [&](const Shape &s) { return s.name == fn; });
  if (shape == std::end(shapes)) {
    checker_.diags_.error(loc,
                          "'" + fn + "' of '" + moduleLabel(module) + "' is not supported yet");
    return loosely();
  }
  if (!checker_.argumentChecker().checkArity(call, loc, shape->min, shape->max)) {
    return call;
  }
  if (shape->builtin == Builtin::PathFormat) {
    // An object with some of root, dir, base, ext and name: a literal takes
    // the type of format's parameter; path.parse() gives all five.
    Expr *arg = checker_.exprChecker().checkExpr(call->args[0], formatInputType());
    const Type *type = arg->type;
    bool fits = type->isObject() && !type->isClass() && !type->isTuple() &&
                llvm::all_of(type->fields(), [&](const Field &f) {
                  return formatInputType()->field(f.name) &&
                         (f.type == string || f.type == Type::optional(string));
                });
    if (!fits && !type->isError()) {
      arg = checker_.typeRelations().coerce(arg, formatInputType());
    }
    call->args = {style, arg};
    call->builtin = Builtin::PathFormat;
    call->type = string;
    return call;
  }
  for (Expr *&arg : call->args) {
    arg = checker_.argumentChecker().checkArg(arg, string);
  }
  call->args.insert(call->args.begin(), style);
  call->builtin = shape->builtin;
  call->type = shape->builtin == Builtin::PathIsAbsolute ? Type::boolTy()
               : shape->builtin == Builtin::PathParse    ? parsedPathType()
                                                         : string;

  return call;
}

} // namespace tz
