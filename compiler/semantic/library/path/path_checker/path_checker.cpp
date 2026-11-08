#include "compiler/semantic/library/path/path_checker/path_checker.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/library/builtin_modules/node_check_helpers/node_check_helpers.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
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

PathChecker::PathChecker(SemanticState &owner)
    : state(owner) {}

PathChecker::ResultPathProperty PathChecker::pathValue(llvm::StringRef name, NodeModule module) {
  if (name == "posix" || name == "win32") {
    std::string message = ErrorTextPath::throughFns(name, NodeCheckHelpers::moduleLabel(module));

    return err(PathCheckerPathValueError{
        .code = PathCheckerPathValueErrorCode::MODULE_AS_VALUE,
        .message = message,
    });
  }

  return ok(name);
}

PathChecker::ResultPathFunction PathChecker::pathFunction(llvm::StringRef fn) {
  if (fn == "sep" || fn == "delimiter") {
    std::string message = ErrorTextPath::notCalled(fn);

    return err(PathCheckerPathFunctionError{
        .code = PathCheckerPathFunctionErrorCode::STRING_CALLED,
        .message = message,
    });
  }
  if (fn == "posix" || fn == "win32") {
    std::string message = ErrorTextPath::module(fn);

    return err(PathCheckerPathFunctionError{
        .code = PathCheckerPathFunctionErrorCode::MODULE_CALLED,
        .message = message,
    });
  }

  return ok(fn);
}

Expr *PathChecker::checkPathValue(NodeModule module, llvm::StringRef name, Expr *expr) {
  bool win32 = module == NodeModule::PathWin32 ||
               (module == NodeModule::Path && NodeCheckHelpers::targetIsWindows());
  if (name == "sep") {
    return NodeCheckHelpers::stringLiteral(this->state.getAst(), expr->loc(), win32 ? "\\" : "/");
  }
  if (name == "delimiter") {
    return NodeCheckHelpers::stringLiteral(this->state.getAst(), expr->loc(), win32 ? ";" : ":");
  }
  ResultPathProperty value = PathChecker::pathValue(name, module);
  if (value.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), value.error().message);
    return expr;
  }

  return nullptr;
}

Expr *PathChecker::checkPathCall(CallExpr *call, NodeModule module, llvm::StringRef fn,
                                 llvm::SMLoc loc) {
  call->type = Type::error();
  auto loosely = [&] {
    for (Expr *&arg : call->args) {
      arg = ExprChecker(this->state).checkExpr(arg);
    }

    return call;
  };
  ResultPathFunction function = PathChecker::pathFunction(fn);
  if (function.isErr()) {
    this->state.getDiagnostics().error(loc, function.error().message);
    return loosely();
  }
  bool win32 = module == NodeModule::PathWin32 ||
               (module == NodeModule::Path && NodeCheckHelpers::targetIsWindows());
  auto *style = this->state.getAst().make<IntLitExpr>(loc, win32 ? 1 : 0);
  style->type = Type::intTy();
  const Type *string = Type::string();

  if (fn == "join" || fn == "resolve") {
    auto *paths =
        this->state.getAst().make<ArrayLitExpr>(call->args.empty() ? loc : call->args[0]->loc());
    for (size_t i = 0; i < call->args.size(); ++i) {
      paths->elements.push_back({call->args[i], call->spreads(i)});
    }
    Expr *array = ArgumentChecker(this->state).checkArg(paths, Type::array(string));
    call->args = {style, array};
    call->spread.clear();
    call->target = BuiltinCall{.builtin = fn == "join" ? Builtin::PathJoin : Builtin::PathResolve};
    call->type = string;
    return call;
  }
  if (CallChecker(this->state).rejectSpread(call)) {
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
    std::string message = ErrorTextPath::notYet(fn, NodeCheckHelpers::moduleLabel(module));
    this->state.getDiagnostics().error(loc, message);
    return loosely();
  }
  ArgumentChecker::ResultArityMatches arity =
      ArgumentChecker(this->state).checkArity(call, shape->min, shape->max);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  if (shape->builtin == Builtin::PathFormat) {
    Expr *arg = ExprChecker(this->state).checkExpr(call->args[0], PathChecker::formatInputType());
    const Type *type = arg->type;
    bool fits = type->isObject() && !type->isClass() && !type->isTuple() &&
                llvm::all_of(type->fields(), [&](const Field &f) {
                  return PathChecker::formatInputType()->field(f.name) &&
                         (f.type == string || f.type == Type::optional(string));
                });
    if (!fits && !type->isError()) {
      arg = TypeRelations(this->state).coerce(arg, PathChecker::formatInputType());
    }
    call->args = {style, arg};
    call->target = BuiltinCall{.builtin = Builtin::PathFormat};
    call->type = string;
    return call;
  }
  for (Expr *&arg : call->args) {
    arg = ArgumentChecker(this->state).checkArg(arg, string);
  }
  call->args.insert(call->args.begin(), style);
  call->target = BuiltinCall{.builtin = shape->builtin};
  call->type = shape->builtin == Builtin::PathIsAbsolute ? Type::boolTy()
               : shape->builtin == Builtin::PathParse    ? PathChecker::parsedPathType()
                                                         : string;

  return call;
}

}
