#include "compiler/semantic/library/result/result_types/result_types.h"
#include "compiler/semantic/declare/type_declarer/type_declarer.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/types/literal_types/literal_types.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

namespace {

const llvm::StringLiteral kResultNames[] = {"ok", "err", "Result", "Ok", "Err"};

const llvm::StringLiteral kResultTypes[] = {"Result", "Ok", "Err"};

}

llvm::StringRef ResultTypes::findName(llvm::ArrayRef<llvm::StringLiteral> names,
                                      llvm::StringRef name) {
  for (llvm::StringLiteral n : names) {
    if (n == name) {
      return n;
    }
  }

  return {};
}

ResultTypes::ResultTypes(SemanticState &owner)
    : state(owner) {}

llvm::StringRef ResultTypes::resultName(llvm::StringRef name) const {
  ScopeEntry entry = Scopes(this->state).lookup(name);
  if (entry.found()) {
    return {};
  }

  return ResultTypes::findName(kResultNames, name);
}

llvm::StringRef ResultTypes::resultTypeName(llvm::StringRef name) const {
  if (TypeDeclarer(this->state).lookupType(name)) {
    return {};
  }

  return ResultTypes::findName(kResultTypes, name);
}

ResultTypes::ResultImportSource ResultTypes::importSource(const ImportDecl &decl) {
  if (decl.path == "neverthrow") {
    std::string message = ErrorTextResultType::neverthrow();

    return err(ResultTypesImportSourceError{
        .code = ResultTypesImportSourceErrorCode::NEVERTHROW_IMPORT,
        .message = message,
    });
  }

  return ok(&decl);
}

ResultTypes::ResultTypeArgsPair ResultTypes::resultTypeArgs(const TypeRef &ref) {
  if (ref.args.size() != 2) {
    std::string message = ErrorTextResultType::typeArgs(ref.name);

    return err(ResultTypesResultTypeArgsError{
        .code = ResultTypesResultTypeArgsErrorCode::NOT_TWO_TYPE_ARGUMENTS,
        .message = message,
    });
  }

  return ok(&ref);
}

const Type *ResultTypes::resolveResultType(const TypeRef &ref, llvm::StringRef name) {
  ResultTypeArgsPair args = ResultTypes::resultTypeArgs(ref);
  if (args.isErr()) {
    this->state.getDiagnostics().error(ref.loc, args.error().message);
    return Type::error();
  }
  auto half = [&](const TypeRef &arg) -> const Type * {
    if (arg.kind() == TypeRefKind::NAME_REF && arg.name == "never") {
      return Type::never();
    }

    return TypeResolver(this->state).resolveTypeInner(arg);
  };
  const Type *value = half(ref.args[0]);
  const Type *error = half(ref.args[1]);
  if (name == "Ok") {
    error = Type::never();
  } else if (name == "Err") {
    value = Type::never();
  }
  return Type::result(value, error);
}

const Type *ResultTypes::unifyHalf(const Type *a, const Type *b) {
  if (a == b || b->isNever()) {
    return a;
  }
  if (a->isNever()) {
    return b;
  }
  if (a->isVoid() || b->isVoid()) {
    return nullptr;
  }
  const Type *common = LiteralTypes::unionUnify(a, b);

  return common && !common->isError() ? common : nullptr;
}

void ResultTypes::neverOperands(BinaryExpr *bin) {
  const Type *l = bin->lhs->type;
  const Type *r = bin->rhs->type;
  if (l->isNever() && r->isNever()) {
    bin->lhs = TypeRelations(this->state).convert(bin->lhs, Type::doubleTy());
    bin->rhs = TypeRelations(this->state).convert(bin->rhs, Type::doubleTy());
  } else if (l->isNever() && !r->isError() && !r->isVoid()) {
    bin->lhs = TypeRelations(this->state).convert(bin->lhs, r);
  } else if (r->isNever() && !l->isError() && !l->isVoid()) {
    bin->rhs = TypeRelations(this->state).convert(bin->rhs, l);
  }
}

}
