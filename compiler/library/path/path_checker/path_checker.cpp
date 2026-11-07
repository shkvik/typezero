#include "compiler/library/path/path_checker/path_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/library/builtin_modules/node_check_helpers/node_check_helpers.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"


namespace tz {

const Type *PathChecker::parsedPathType() {
  const Type *s = Type::string();

  return Type::object({{"root", s, false},
                       {"dir", s, false},
                       {"base", s, false},
                       {"ext", s, false},
                       {"name", s, false}},
                      "ParsedPath");
}

const Type *PathChecker::formatInputType() {
  const Type *s = Type::optional(Type::string());

  return Type::object({{"root", s, false},
                       {"dir", s, false},
                       {"base", s, false},
                       {"ext", s, false},
                       {"name", s, false}},
                      "FormatInputPathObject");
}

PathChecker::PathChecker(Checker &owner)
    : checker(owner) {}

Expr *PathChecker::checkPathValue(NodeModule module, llvm::StringRef name, Expr *expr) {
  bool win32 = module == NodeModule::PathWin32 || (module == NodeModule::Path && targetIsWindows());
  if (name == "sep") {
    return stringLiteral(this->checker.ctx_, expr->loc(), win32 ? "\\" : "/");
  }
  if (name == "delimiter") {
    return stringLiteral(this->checker.ctx_, expr->loc(), win32 ? ";" : ":");
  }
  if (name == "posix" || name == "win32") {
    this->checker.diags_.error(expr->loc(), "'" + name + "' of '" + moduleLabel(module) +
                                                "' is used through its functions, e.g. '" + name +
                                                ".join(a, b)'");
    return expr;
  }

  return nullptr;
}

Expr *PathChecker::checkPathCall(CallExpr *call, NodeModule module, llvm::StringRef fn,
                                 llvm::SMLoc loc) {
  call->type = Type::error();
  auto loosely = [&] {
    for (Expr *&arg : call->args) {
      arg = this->checker.exprChecker().checkExpr(arg);
    }

    return call;
  };
  if (fn == "sep" || fn == "delimiter") {
    this->checker.diags_.error(loc, "'" + fn + "' is a string, not a function");
    return loosely();
  }
  if (fn == "posix" || fn == "win32") {
    this->checker.diags_.error(loc, "'" + fn + "' is not a function: call its functions, e.g. '" +
                                        fn + ".join(a, b)'");
    return loosely();
  }
  bool win32 = module == NodeModule::PathWin32 || (module == NodeModule::Path && targetIsWindows());
  auto *style = this->checker.ctx_.make<IntLitExpr>(loc, win32 ? 1 : 0);
  style->type = Type::intTy();
  const Type *string = Type::string();

  if (fn == "join" || fn == "resolve") {
    auto *paths =
        this->checker.ctx_.make<ArrayLitExpr>(call->args.empty() ? loc : call->args[0]->loc());
    for (size_t i = 0; i < call->args.size(); ++i) {
      paths->elements.push_back({call->args[i], call->spreads(i)});
    }
    Expr *array = this->checker.argumentChecker().checkArg(paths, Type::array(string));
    call->args = {style, array};
    call->spread.clear();
    call->target = BuiltinCall{.builtin = fn == "join" ? Builtin::PathJoin : Builtin::PathResolve};
    call->type = string;
    return call;
  }
  if (this->checker.callChecker().rejectSpread(call)) {
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
    this->checker.diags_.error(loc, "'" + fn + "' of '" + moduleLabel(module) +
                                        "' is not supported yet");
    return loosely();
  }
  ArgumentChecker::ResultArityMatches arity =
      this->checker.argumentChecker().checkArity(call, shape->min, shape->max);
  if (arity.isErr()) {
    this->checker.diags_.error(loc, arity.error().message);
    return call;
  }
  if (shape->builtin == Builtin::PathFormat) {
    Expr *arg =
        this->checker.exprChecker().checkExpr(call->args[0], PathChecker::formatInputType());
    const Type *type = arg->type;
    bool fits = type->isObject() && !type->isClass() && !type->isTuple() &&
                llvm::all_of(type->fields(), [&](const Field &f) {
                  return PathChecker::formatInputType()->field(f.name) &&
                         (f.type == string || f.type == Type::optional(string));
                });
    if (!fits && !type->isError()) {
      arg = this->checker.typeRelations().coerce(arg, PathChecker::formatInputType());
    }
    call->args = {style, arg};
    call->target = BuiltinCall{.builtin = Builtin::PathFormat};
    call->type = string;
    return call;
  }
  for (Expr *&arg : call->args) {
    arg = this->checker.argumentChecker().checkArg(arg, string);
  }
  call->args.insert(call->args.begin(), style);
  call->target = BuiltinCall{.builtin = shape->builtin};
  call->type = shape->builtin == Builtin::PathIsAbsolute ? Type::boolTy()
               : shape->builtin == Builtin::PathParse    ? PathChecker::parsedPathType()
                                                         : string;

  return call;
}

}
