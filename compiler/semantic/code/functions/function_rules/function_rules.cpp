#include "compiler/semantic/code/functions/function_rules/function_rules.h"
#include "compiler/program/library/builtin_names/function_value_table/function_value_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/semantic/code/functions/code_functions_registry_errors.h"

using llvm::cast;
using llvm::dyn_cast;

namespace tz {

FunctionRules::FunctionRules(SemanticState &owner)
    : state(owner) {}

FunctionExpr *FunctionRules::builtinFunctionValue(Expr *arg, const Signature &expected,
                                                  unsigned primary, llvm::StringRef method) {
  auto shape = this->functionValueShape(arg);
  if (!shape.has_value()) {
    return nullptr;
  }
  size_t offered = std::min<size_t>(shape->maxArgs, expected.params.size());
  auto matched = this->checkExtraValueArguments(arg, shape->name, method, offered, primary);
  if (matched.isErr()) {
    this->state.diagnostics.error(arg->loc(), matched.error().message);
    return this->forwardingFunction(arg, *shape, primary);
  }

  return this->forwardingFunction(arg, *shape, matched.value());
}

std::optional<FunctionRules::FunctionValueShape>
FunctionRules::functionValueShape(const Expr *arg) const {
  auto *named = dyn_cast<NameExpr>(arg);
  if (named != nullptr) {
    return this->globalFunctionShape(named);
  }
  auto *member = dyn_cast<MemberExpr>(arg);
  if (member != nullptr) {
    return this->staticFunctionShape(member);
  }

  return std::nullopt;
}

std::optional<FunctionRules::FunctionValueShape>
FunctionRules::globalFunctionShape(const NameExpr *named) const {
  auto result = this->state.library.result.resultName(named->name);
  if (result == GlobalName::OK || result == GlobalName::ERR) {
    return FunctionValueShape{
        .name = named->name,
        .maxArgs = 1,
        .isReturningNothing = false,
    };
  }
  auto global = this->namedGlobal(named);
  const FunctionValueGlobalInfo *info =
      global.has_value() ? FunctionValueTable::findGlobal(*global) : nullptr;
  if (info == nullptr) {
    return std::nullopt;
  }

  return FunctionValueShape{
      .name = GlobalNameTable::getName(info->global),
      .maxArgs = info->maxArgs,
      .isReturningNothing = false,
  };
}

std::optional<FunctionRules::FunctionValueShape>
FunctionRules::staticFunctionShape(const MemberExpr *member) const {
  auto owner = this->namedGlobal(member->object);
  const FunctionValueStaticInfo *info =
      owner.has_value() ? FunctionValueTable::findStatic(*owner, member->name) : nullptr;
  if (info == nullptr) {
    return std::nullopt;
  }

  return FunctionValueShape{
      .name = member->name,
      .maxArgs = info->maxArgs,
      .isReturningNothing = info->isReturningNothing,
  };
}

std::optional<GlobalName> FunctionRules::namedGlobal(const Expr *expr) const {
  auto *named = dyn_cast<NameExpr>(expr);
  if (named == nullptr) {
    return std::nullopt;
  }
  auto global = GlobalNameTable::find(named->name);
  if (!global.has_value() || !this->state.isGlobalName(named, *global)) {
    return std::nullopt;
  }

  return global;
}

FunctionExpr *FunctionRules::forwardingFunction(Expr *arg, const FunctionValueShape &shape,
                                                size_t count) {
  auto *functionDecl = this->state.ast.make<FunctionDecl>(arg->loc(), shape.name);
  functionDecl->place = FunctionPlace::EXPRESSION;
  functionDecl->syntax = ArrowFunction{.concise = true};
  auto args = std::vector<Expr *>();
  for (size_t index = 0; index < count; ++index) {
    auto param = Param{
        .name = this->state.ast.hiddenName(),
        .loc = arg->loc(),
    };
    functionDecl->params.push_back(param);
    auto *forwarded = this->state.ast.make<NameExpr>(arg->loc(), param.name);
    args.push_back(forwarded);
  }
  auto *call = this->state.ast.make<CallExpr>(arg->loc(), arg, std::move(args));
  functionDecl->body = this->state.ast.make<BlockStmt>(arg->loc());
  auto *statement = this->forwardingStatement(arg, call, shape);
  functionDecl->body->body.push_back(statement);

  return this->state.ast.make<FunctionExpr>(arg->loc(), functionDecl);
}

Stmt *FunctionRules::forwardingStatement(const Expr *arg, CallExpr *call,
                                         const FunctionValueShape &shape) {
  if (shape.isReturningNothing) {
    return this->state.ast.make<ExprStmt>(arg->loc(), call);
  }

  return this->state.ast.make<ReturnStmt>(arg->loc(), call);
}

FunctionRules::ResultArgumentsMatch
FunctionRules::checkExtraValueArguments(const Expr *arg, llvm::StringRef name,
                                        llvm::StringRef method, size_t count,
                                        unsigned primary) const {
  if (count <= primary) {
    return ok(count);
  }
  auto qualified = this->qualifiedName(arg, name);
  auto argument = ErrorTextCallbackArg::argName(method, primary);
  auto ordinal = ErrorTextCallbackArg::position(primary);
  auto passes = ErrorTextCallbackArg::argsShape(method);
  auto message = ErrorTextExtraArguments::passed(qualified, argument, ordinal, method, passes);

  return err(CodeFunctionsRegistryFunctionValueExtraArgumentsError{
      .code = CodeFunctionsRegistryFunctionValueExtraArgumentsErrorCode::MORE_ARGUMENTS,
      .message = message,
  });
}

std::string FunctionRules::qualifiedName(const Expr *arg, llvm::StringRef name) const {
  auto *member = dyn_cast<MemberExpr>(arg);
  if (member == nullptr) {
    return name.str();
  }
  auto *owner = cast<NameExpr>(member->object);

  return owner->name.str() + "." + name.str();
}

}
