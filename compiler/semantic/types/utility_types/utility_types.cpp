#include "compiler/semantic/types/utility_types/utility_types.h"
#include "compiler/program/ast/ast.h"
#include "compiler/semantic/bodies/classes/class_value_checker/class_value_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/declare/type_declarer/type_declarer.h"
#include "compiler/semantic/generics/type_params/type_params.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/conditional_types/conditional_types.h"
#include "compiler/semantic/types/type_operators/type_operators.h"
#include "compiler/semantic/types/typeops_check_helpers/typeops_check_helpers.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/MemoryBuffer.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

UtilityTypes::UtilityTypes(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.scopes;
  this->typeParams = &all.typeParams;
  this->exprChecker = &all.exprChecker;
  this->typeDeclarer = &all.typeDeclarer;
  this->typeOperators = &all.typeOperators;
  this->returnInferrer = &all.returnInferrer;
  this->conditionalTypes = &all.conditionalTypes;
  this->classValueChecker = &all.classValueChecker;
}

llvm::ArrayRef<const Type *> UtilityTypes::membersOf(const Type *&type) const {
  return type->isUnion() ? type->members() : llvm::ArrayRef<const Type *>(type);
}

const Type *UtilityTypes::functionValueType(FuncSymbol *function) {
  if (!function->isGeneric()) {
    return this->returnInferrer->functionType(function);
  }
  bool hasAllDefaults = function->typeDefaults.size() == function->typeParams.size() &&
                        !llvm::is_contained(function->typeDefaults, nullptr);
  if (!hasAllDefaults) {
    return nullptr;
  }
  const auto *type = this->returnInferrer->functionType(function);

  return this->typeParams->substitute(type, function->typeParams, function->typeDefaults);
}

const Type *UtilityTypes::importedValueType(Module *from, llvm::StringRef name) {
  for (Stmt *item : from->file.items) {
    auto *functionDecl = dyn_cast<FunctionDecl>(item);
    auto *classDecl = dyn_cast<ClassDecl>(item);
    auto *varDecl = dyn_cast<VarDeclStmt>(item);

    if (functionDecl != nullptr && functionDecl->name == name) {
      return this->functionDeclType(functionDecl);
    }
    if (classDecl != nullptr && classDecl->name == name) {
      return this->classDeclType(classDecl);
    }
    if (varDecl != nullptr && this->hasDeclarator(varDecl, name)) {
      return this->varDeclType(varDecl, name);
    }
  }

  return nullptr;
}

const Type *UtilityTypes::functionDeclType(FunctionDecl *functionDecl) {
  if (functionDecl->sym == nullptr) {
    return nullptr;
  }

  return this->functionValueType(functionDecl->sym);
}

const Type *UtilityTypes::classDeclType(ClassDecl *classDecl) {
  if (classDecl->isGeneric()) {
    return nullptr;
  }

  return this->classValueChecker->classValueType(classDecl);
}

const VarDeclarator *UtilityTypes::declaratorNamed(const VarDeclStmt *varDecl,
                                                   llvm::StringRef name) const {
  for (const VarDeclarator &declarator : varDecl->decls) {
    if (declarator.name == name) {
      return &declarator;
    }
  }

  return nullptr;
}

bool UtilityTypes::hasDeclarator(const VarDeclStmt *varDecl, llvm::StringRef name) const {
  return this->declaratorNamed(varDecl, name) != nullptr;
}

const Type *UtilityTypes::varDeclType(const VarDeclStmt *varDecl, llvm::StringRef name) const {
  const auto *declarator = this->declaratorNamed(varDecl, name);
  if (declarator == nullptr || declarator->sym == nullptr) {
    return nullptr;
  }

  return declarator->sym->type;
}

UtilityTypes::ResultUtilityApplied UtilityTypes::resolveUtilityType(const TypeRef &ref) {
  const auto *utility = this->findUtility(ref.name);
  if (utility == nullptr) {
    return err(UtilityTypesResolveUtilityTypeError{
        .code = UtilityTypesResolveUtilityTypeErrorCode::NOT_A_UTILITY,
        .message = "",
    });
  }
  auto argumentCount = this->checkArgumentCount(ref, *utility);
  if (argumentCount.isErr()) {
    return err(argumentCount.error());
  }
  auto operands = this->utilityOperands(ref, *utility);
  if (operands.back()->isError()) {
    return ok(operands.back());
  }
  auto operand = this->checkOperand(ref, *utility, operands.front());
  if (operand.isErr()) {
    return err(operand.error());
  }
  const auto *unevaluated = Type::typeOp(utility->op, operands);
  const auto *evaluated = this->typeOperators->evalTypeOp(unevaluated, operands);

  return ok(evaluated);
}

const UtilityTypeName *UtilityTypes::findUtility(llvm::StringRef name) const {
  const auto *utility = TypeNameTable::findUtility(name);
  bool isShadowed = utility != nullptr && this->typeDeclarer->lookupType(name) != nullptr;
  if (isShadowed) {
    return nullptr;
  }

  return utility;
}

UtilityTypes::ResultUtilityAllowed
UtilityTypes::checkArgumentCount(const TypeRef &ref, const UtilityTypeName &utility) const {
  if (int(ref.args.size()) != utility.argumentCount) {
    auto message = ErrorTextUtility::count(ref.name, utility.argumentCount);

    return err(UtilityTypesResolveUtilityTypeError{
        .code = UtilityTypesResolveUtilityTypeErrorCode::WRONG_ARGUMENT_COUNT,
        .message = message,
    });
  }

  return ok();
}

std::vector<const Type *> UtilityTypes::utilityOperands(const TypeRef &ref,
                                                        const UtilityTypeName &utility) {
  auto operands = std::vector<const Type *>();
  for (const TypeRef &arg : ref.args) {
    const Type *operand = nullptr;
    if (utility.operand == UtilityOperand::FUNCTION_TYPE) {
      operand = this->conditionalTypes->signatureForUtility(arg);
    }
    if (operand == nullptr) {
      operand = this->conditionalTypes->resolveTypeArgument(arg);
    }
    operands.push_back(operand);
    if (operand->isError()) {
      break;
    }
  }

  return operands;
}

UtilityTypes::ResultUtilityAllowed UtilityTypes::checkOperand(const TypeRef &ref,
                                                              const UtilityTypeName &utility,
                                                              const Type *operand) const {
  if (TypeQueries::mentionsParam(operand)) {
    return ok();
  }
  switch (utility.operand) {
    case UtilityOperand::ANY_TYPE:
      return ok();
    case UtilityOperand::FUNCTION_TYPE:
      return this->checkFunctionOperand(ref, operand);
    case UtilityOperand::TEXT_TYPE:
      return this->checkTextOperand(ref, operand);
  }

  return ok();
}

UtilityTypes::ResultUtilityAllowed UtilityTypes::checkFunctionOperand(const TypeRef &ref,
                                                                      const Type *operand) const {
  if (!operand->nonOptional()->isFunction()) {
    auto message = ErrorTextUtility::noFunction(ref.name, operand->name());

    return err(UtilityTypesResolveUtilityTypeError{
        .code = UtilityTypesResolveUtilityTypeErrorCode::NOT_A_FUNCTION_TYPE,
        .message = message,
    });
  }

  return ok();
}

UtilityTypes::ResultUtilityAllowed UtilityTypes::checkTextOperand(const TypeRef &ref,
                                                                  const Type *operand) const {
  const auto *nonString = this->firstNonString(operand);
  if (nonString != nullptr) {
    auto message = ErrorTextUtility::notString(ref.name, nonString->name());

    return err(UtilityTypesResolveUtilityTypeError{
        .code = UtilityTypesResolveUtilityTypeErrorCode::NOT_A_STRING_TYPE,
        .message = message,
    });
  }

  return ok();
}

const Type *UtilityTypes::firstNonString(const Type *operand) const {
  for (const Type *member : TypeopsCheckHelpers::distributedMembers(operand)) {
    if (!member->isString() && !member->isStringLiteral()) {
      return member;
    }
  }

  return nullptr;
}

bool UtilityTypes::isGenericKey(const Type *key) const {
  return key->isParam() && !key->isListParam() && this->isExpectingLiteral(key);
}

bool UtilityTypes::isGenericKeyName(const NameExpr *keyName) const {
  auto entry = this->scopes->lookup(keyName->name);
  const VarSymbol *variable = entry.var();
  if (variable == nullptr || variable->type == nullptr) {
    return false;
  }

  return this->isGenericKey(variable->type);
}

UtilityTypes::ResultGenericIndexed UtilityTypes::checkGenericIndex(IndexExpr *index) {
  const Type *object = index->object->type;
  bool isOpenObject = object->isParam() && !object->isListParam() && object->iterated() == nullptr;
  bool isNamedKey = object->isObject() && isa<NameExpr>(index->index);
  if (!isOpenObject && !isNamedKey) {
    return err(UtilityTypesCheckGenericIndexError{
        .code = UtilityTypesCheckGenericIndexErrorCode::NOT_A_GENERIC_INDEX,
        .message = "",
    });
  }
  if (!isOpenObject) {
    const auto *keyName = cast<NameExpr>(index->index);
    if (!this->isGenericKeyName(keyName)) {
      return err(UtilityTypesCheckGenericIndexError{
          .code = UtilityTypesCheckGenericIndexErrorCode::NOT_A_GENERIC_INDEX,
          .message = "",
      });
    }
  }
  index->index = this->exprChecker->checkExpr(index->index);
  const Type *key = index->index->type;
  if (key->isError()) {
    return ok(index);
  }
  if (!key->isStringLiteral() && !this->isGenericKey(key)) {
    auto message = ErrorTextGenericIndex::notByKey(object->name(), key->name());

    return err(UtilityTypesCheckGenericIndexError{
        .code = UtilityTypesCheckGenericIndexErrorCode::NOT_INDEXED_BY_KEY,
        .message = message,
    });
  }
  const auto *indexed = Type::indexedAccess(object, key);
  index->type = this->typeOperators->evalTypeOp(indexed, {object, key});

  return ok(index);
}

bool UtilityTypes::isExpectingLiteral(const Type *param) const {
  if (!param->isParam()) {
    return false;
  }
  const Type *constraint = param->constraint();
  if (constraint == nullptr) {
    return false;
  }
  if (constraint->typeOp() == TypeOp::KeyOf) {
    return true;
  }
  for (const Type *member : this->membersOf(constraint)) {
    if (!member->isStringLiteral()) {
      return false;
    }
  }

  return true;
}

}
