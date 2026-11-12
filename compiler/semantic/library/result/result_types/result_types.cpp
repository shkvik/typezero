#include "compiler/semantic/library/result/result_types/result_types.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/builtin_names/type_name_table/type_name_table.h"
#include "compiler/semantic/declare/type_declarer/type_declarer.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/literal_types/literal_types.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

ResultTypes::ResultTypes(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.scopes;
  this->typeDeclarer = &all.typeDeclarer;
  this->typeResolver = &all.typeResolver;
  this->typeRelations = &all.typeRelations;
}

std::optional<GlobalName> ResultTypes::resultName(llvm::StringRef name) const {
  auto entry = this->scopes->lookup(name);
  if (entry.isFound()) {
    return std::nullopt;
  }
  static const GlobalName resultNames[] = {GlobalName::OK, GlobalName::ERR, GlobalName::RESULT,
                                           GlobalName::OK_TYPE, GlobalName::ERR_TYPE};
  auto global = GlobalNameTable::find(name);
  if (!global.has_value() || !llvm::is_contained(resultNames, *global)) {
    return std::nullopt;
  }

  return global;
}

std::optional<TypeName> ResultTypes::findResultType(llvm::StringRef name) const {
  if (this->typeDeclarer->lookupType(name) != nullptr) {
    return std::nullopt;
  }
  const auto *builtin = TypeNameTable::find(name);
  if (builtin == nullptr || !builtin->form.is<ResultTypeName>()) {
    return std::nullopt;
  }

  return builtin->type;
}

ResultTypes::ResultImportSource ResultTypes::checkImportSource(const ImportDecl &importDecl) const {
  if (importDecl.path == "neverthrow") {
    auto message = ErrorTextResultType::neverthrow();

    return err(ResultTypesImportSourceError{
        .code = ResultTypesImportSourceErrorCode::NEVERTHROW_IMPORT,
        .message = message,
    });
  }

  return ok(&importDecl);
}

ResultTypes::ResultTypeArgsPair ResultTypes::checkResultTypeArgs(const TypeRef &ref) const {
  if (ref.args.size() != 2) {
    auto message = ErrorTextResultType::typeArgs(ref.name);

    return err(ResultTypesResultTypeArgsError{
        .code = ResultTypesResultTypeArgsErrorCode::NOT_TWO_TYPE_ARGUMENTS,
        .message = message,
    });
  }

  return ok(&ref);
}

const Type *ResultTypes::resolveResultType(const TypeRef &ref, TypeName result) {
  auto args = this->checkResultTypeArgs(ref);
  if (args.isErr()) {
    this->state.getDiagnostics().error(ref.loc, args.error().message);
    return Type::error();
  }
  const auto *valueHalf = this->resolveResultHalf(ref.args[0]);
  const auto *errorHalf = this->resolveResultHalf(ref.args[1]);
  const auto *value = result == TypeName::ERR ? Type::never() : valueHalf;
  const auto *error = result == TypeName::OK ? Type::never() : errorHalf;

  return Type::result(value, error);
}

const Type *ResultTypes::resolveResultHalf(const TypeRef &arg) {
  if (arg.kind() == TypeRefKind::NAME_REF && TypeNameTable::isName(arg.name, TypeName::NEVER)) {
    return Type::never();
  }

  return this->typeResolver->resolveTypeInner(arg);
}

const Type *ResultTypes::unifyHalf(const Type *first, const Type *second) const {
  return TypeRelations::unifyAs(UnifyForm::HALF, first, second);
}

UnifyShortcut ResultTypes::halfShortcut(const Type *first, const Type *second) {
  if (first == second || second->isNever()) {
    return {true, first};
  }
  if (first->isNever()) {
    return {true, second};
  }
  if (first->isVoid() || second->isVoid()) {
    return {true, nullptr};
  }

  return {false, nullptr};
}

void ResultTypes::neverOperands(BinaryExpr *binary) {
  const Type *leftType = binary->lhs->type;
  const Type *rightType = binary->rhs->type;
  if (leftType->isNever() && rightType->isNever()) {
    binary->lhs = this->typeRelations->convert(binary->lhs, Type::doubleTy());
    binary->rhs = this->typeRelations->convert(binary->rhs, Type::doubleTy());
  } else if (leftType->isNever() && !rightType->isError() && !rightType->isVoid()) {
    binary->lhs = this->typeRelations->convert(binary->lhs, rightType);
  } else if (rightType->isNever() && !leftType->isError() && !leftType->isVoid()) {
    binary->rhs = this->typeRelations->convert(binary->rhs, leftType);
  }
}

}
