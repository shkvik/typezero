#include "compiler/semantic/library/path/path_checker/path_checker.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/library/builtin_modules/node_check_helpers/node_check_helpers.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

PathChecker::PathChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
  this->typeRelations = &all.typeRelations;
  this->argumentChecker = &all.argumentChecker;
}

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
  bool isWin32 = module == NodeModule::PathWin32 ||
                 (module == NodeModule::Path && NodeCheckHelpers::targetIsWindows());
  if (name == "sep") {
    return NodeCheckHelpers::stringLiteral(this->state.getAst(), expr->loc(), isWin32 ? "\\" : "/");
  }
  if (name == "delimiter") {
    return NodeCheckHelpers::stringLiteral(this->state.getAst(), expr->loc(), isWin32 ? ";" : ":");
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
  ResultPathFunction function = PathChecker::pathFunction(fn);
  if (function.isErr()) {
    this->state.getDiagnostics().error(loc, function.error().message);
    return this->checkArgsLoosely(call);
  }
  bool isWin32 = module == NodeModule::PathWin32 ||
                 (module == NodeModule::Path && NodeCheckHelpers::targetIsWindows());
  auto *style = this->state.getAst().make<IntLitExpr>(loc, isWin32 ? 1 : 0);
  style->type = Type::intTy();
  const PathFunctionInfo *path = PathFunctionTable::find(fn);
  if (path != nullptr && path->form == PathFunctionForm::JOINED) {
    return this->checkJoinedPath(call, style, path->builtin, loc);
  }
  if (this->callChecker->rejectSpread(call)) {
    return call;
  }
  if (path == nullptr) {
    std::string message = ErrorTextPath::notYet(fn, NodeCheckHelpers::moduleLabel(module));
    this->state.getDiagnostics().error(loc, message);
    return this->checkArgsLoosely(call);
  }
  ArgumentChecker::ResultArityMatches arity =
      this->argumentChecker->checkArity(call, path->min, path->max);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  if (path->form == PathFunctionForm::FORMAT) {
    return this->checkFormatPath(call, style);
  }

  return this->checkTextPath(call, style, *path);
}

Expr *PathChecker::checkArgsLoosely(CallExpr *call) {
  for (Expr *&arg : call->args) {
    arg = this->exprChecker->checkExpr(arg);
  }

  return call;
}

Expr *PathChecker::checkJoinedPath(CallExpr *call, Expr *style, Builtin builtin, llvm::SMLoc loc) {
  llvm::SMLoc at = call->args.empty() ? loc : call->args[0]->loc();
  auto *paths = this->state.getAst().make<ArrayLitExpr>(at);
  for (size_t i = 0; i < call->args.size(); ++i) {
    paths->elements.push_back({call->args[i], call->spreads(i)});
  }
  Expr *array = this->argumentChecker->checkArg(paths, Type::array(Type::string()));
  call->args = {style, array};
  call->spread.clear();
  call->target = BuiltinCall{.builtin = builtin};
  call->type = Type::string();

  return call;
}

Expr *PathChecker::checkFormatPath(CallExpr *call, Expr *style) {
  Expr *arg = this->exprChecker->checkExpr(call->args[0], PathChecker::formatInputType());
  const Type *type = arg->type;
  if (!PathChecker::fitsFormatInput(type) && !type->isError()) {
    arg = this->typeRelations->coerce(arg, PathChecker::formatInputType());
  }
  call->args = {style, arg};
  call->target = BuiltinCall{.builtin = Builtin::PathFormat};
  call->type = Type::string();

  return call;
}

bool PathChecker::fitsFormatInput(const Type *type) {
  if (!type->isObject() || type->isClass() || type->isTuple()) {
    return false;
  }
  const Type *string = Type::string();
  for (const Field &f : type->fields()) {
    bool isKnown = PathChecker::formatInputType()->field(f.name) != nullptr;
    if (!isKnown || (f.type != string && f.type != Type::optional(string))) {
      return false;
    }
  }

  return true;
}

Expr *PathChecker::checkTextPath(CallExpr *call, Expr *style, const PathFunctionInfo &path) {
  for (Expr *&arg : call->args) {
    arg = this->argumentChecker->checkArg(arg, Type::string());
  }
  call->args.insert(call->args.begin(), style);
  call->target = BuiltinCall{.builtin = path.builtin};
  call->type = PathChecker::pathResultType(path.form);

  return call;
}

const Type *PathChecker::pathResultType(PathFunctionForm form) {
  if (form == PathFunctionForm::TEST) {
    return Type::boolTy();
  }
  if (form == PathFunctionForm::PARSED) {
    return PathChecker::parsedPathType();
  }

  return Type::string();
}

}
