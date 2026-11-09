#include "compiler/semantic/library/result/result_types/result_types.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/semantic/declare/type_declarer/type_declarer.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/literal_types/literal_types.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

namespace {

const llvm::StringLiteral kResultTypes[] = {"Result", "Ok", "Err"};

}

ResultTypes::ResultTypes(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.getScopes();
  this->typeDeclarer = &all.getTypeDeclarer();
  this->typeResolver = &all.getTypeResolver();
  this->typeRelations = &all.getTypeRelations();
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

std::optional<GlobalName> ResultTypes::resultName(llvm::StringRef name) const {
  ScopeEntry entry = this->scopes->lookup(name);
  if (entry.found()) {
    return std::nullopt;
  }
  static const GlobalName resultNames[] = {GlobalName::OK, GlobalName::ERR, GlobalName::RESULT,
                                           GlobalName::OK_TYPE, GlobalName::ERR_TYPE};
  std::optional<GlobalName> global = GlobalNameTable::find(name);
  if (!global.has_value() || !llvm::is_contained(resultNames, *global)) {
    return std::nullopt;
  }

  return global;
}

llvm::StringRef ResultTypes::resultTypeName(llvm::StringRef name) const {
  if (this->typeDeclarer->lookupType(name)) {
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

    return this->typeResolver->resolveTypeInner(arg);
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
    bin->lhs = this->typeRelations->convert(bin->lhs, Type::doubleTy());
    bin->rhs = this->typeRelations->convert(bin->rhs, Type::doubleTy());
  } else if (l->isNever() && !r->isError() && !r->isVoid()) {
    bin->lhs = this->typeRelations->convert(bin->lhs, r);
  } else if (r->isNever() && !l->isError() && !l->isVoid()) {
    bin->rhs = this->typeRelations->convert(bin->rhs, l);
  }
}

}
