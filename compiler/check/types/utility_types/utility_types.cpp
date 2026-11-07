#include "compiler/check/types/utility_types/utility_types.h"
#include "compiler/check/classes/class_value_checker/class_value_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/generics/type_params/type_params.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/types/conditional_types/conditional_types.h"
#include "compiler/check/types/type_operators/type_operators.h"
#include "compiler/check/types/typeops_check_helpers/typeops_check_helpers.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/declare/type_declarer/type_declarer.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/load/lexer/lexer.h"
#include "compiler/load/parser/parser.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/MemoryBuffer.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

llvm::ArrayRef<const Type *> UtilityTypes::membersOf(const Type *&type) {
  return type->isUnion() ? type->members() : llvm::ArrayRef<const Type *>(type);
}

UtilityTypes::UtilityTypes(Checker &owner)
    : checker(owner) {}

const Type *UtilityTypes::functionValueType(FuncSymbol *fn) {
  if (!fn->isGeneric()) {
    return this->checker.returnInferrer().functionType(fn);
  }
  if (fn->typeDefaults.size() != fn->typeParams.size() ||
      llvm::is_contained(fn->typeDefaults, nullptr)) {
    return nullptr;
  }
  const Type *type = this->checker.returnInferrer().functionType(fn);

  return this->checker.typeParams().substitute(type, fn->typeParams, fn->typeDefaults);
}

const Type *UtilityTypes::importedValueType(Module *from, llvm::StringRef name) {
  for (Stmt *item : from->file.items) {
    if (auto *fn = dyn_cast<FunctionDecl>(item); fn && fn->name == name) {
      return fn->sym ? this->functionValueType(fn->sym) : nullptr;
    }
    if (auto *cls = dyn_cast<ClassDecl>(item); cls && cls->name == name) {
      return cls->isGeneric() ? nullptr : this->checker.classValueChecker().classValueType(cls);
    }
    if (auto *decl = dyn_cast<VarDeclStmt>(item)) {
      for (const VarDeclarator &d : decl->decls) {
        if (d.name == name) {
          return d.sym ? d.sym->type : nullptr;
        }
      }
    }
  }

  return nullptr;
}

const UtilityTypes::Utility *UtilityTypes::utilityNamed(llvm::StringRef name) {
  static const Utility utilities[] = {
      {"Exclude", TypeOp::Exclude, 2},           {"Extract", TypeOp::Extract, 2},
      {"NonNullable", TypeOp::NonNullable, 1},   {"ReturnType", TypeOp::ReturnType, 1},
      {"Parameters", TypeOp::Parameters, 1},     {"Uppercase", TypeOp::Uppercase, 1},
      {"Lowercase", TypeOp::Lowercase, 1},       {"Capitalize", TypeOp::Capitalize, 1},
      {"Uncapitalize", TypeOp::Uncapitalize, 1},
  };
  for (const Utility &utility : utilities) {
    if (utility.name == name) {
      return &utility;
    }
  }

  return nullptr;
}

UtilityTypes::ResultUtilityApplied UtilityTypes::resolveUtilityType(const TypeRef &ref) {
  const Utility *utility = UtilityTypes::utilityNamed(ref.name);
  if (!utility || this->checker.typeDeclarer().lookupType(ref.name)) {
    return err(UtilityTypesResolveUtilityTypeError{
        .code = UtilityTypesResolveUtilityTypeErrorCode::NOT_A_UTILITY,
        .message = "",
    });
  }
  if (int(ref.args.size()) != utility->count) {
    std::string message = ErrorTextUtility::count(ref.name, utility->count);

    return err(UtilityTypesResolveUtilityTypeError{
        .code = UtilityTypesResolveUtilityTypeErrorCode::WRONG_ARGUMENT_COUNT,
        .message = message,
    });
  }

  bool function = utility->op == TypeOp::ReturnType || utility->op == TypeOp::Parameters;
  bool text = utility->op == TypeOp::Uppercase || utility->op == TypeOp::Lowercase ||
              utility->op == TypeOp::Capitalize || utility->op == TypeOp::Uncapitalize;
  std::vector<const Type *> operands;
  for (const TypeRef &arg : ref.args) {
    const Type *signature =
        function ? this->checker.conditionalTypes().signatureForUtility(arg) : nullptr;
    operands.push_back(signature ? signature
                                 : this->checker.conditionalTypes().resolveTypeArgument(arg));
    if (operands.back()->isError()) {
      return ok(operands.back());
    }
  }
  if (function && !mentionsParam(operands[0]) && !operands[0]->nonOptional()->isFunction()) {
    std::string message = ErrorTextUtility::noFunction(ref.name, operands[0]->name());

    return err(UtilityTypesResolveUtilityTypeError{
        .code = UtilityTypesResolveUtilityTypeErrorCode::NOT_A_FUNCTION_TYPE,
        .message = message,
    });
  }
  if (text && !mentionsParam(operands[0])) {
    for (const Type *m : distributedMembers(operands[0])) {
      if (!m->isString() && !m->isStringLiteral()) {
        std::string message = ErrorTextUtility::notString(ref.name, m->name());

        return err(UtilityTypesResolveUtilityTypeError{
            .code = UtilityTypesResolveUtilityTypeErrorCode::NOT_A_STRING_TYPE,
            .message = message,
        });
      }
    }
  }

  const Type *evaluated =
      this->checker.typeOperators().evalTypeOp(Type::typeOp(utility->op, operands), operands);

  return ok(evaluated);
}

UtilityTypes::ResultGenericIndexed UtilityTypes::checkGenericIndex(IndexExpr *index) {
  const Type *object = index->object->type;
  auto genericKey = [&](const Type *key) {
    return key->isParam() && !key->isListParam() && this->wantsLiteral(key);
  };
  bool open = object->isParam() && !object->isListParam() && !object->iterated();
  if (!open && !(object->isObject() && isa<NameExpr>(index->index))) {
    return err(UtilityTypesCheckGenericIndexError{
        .code = UtilityTypesCheckGenericIndexErrorCode::NOT_A_GENERIC_INDEX,
        .message = "",
    });
  }
  if (!open) {
    auto *name = cast<NameExpr>(index->index);
    VarSymbol *var = this->checker.scopes().lookup(name->name).var();
    if (!var || !var->type || !genericKey(var->type)) {
      return err(UtilityTypesCheckGenericIndexError{
          .code = UtilityTypesCheckGenericIndexErrorCode::NOT_A_GENERIC_INDEX,
          .message = "",
      });
    }
  }
  index->index = this->checker.exprChecker().checkExpr(index->index);
  const Type *key = index->index->type;
  if (key->isError()) {
    return ok(index);
  }
  if (!key->isStringLiteral() && !genericKey(key)) {
    std::string message = ErrorTextGenericIndex::notByKey(object->name(), key->name());

    return err(UtilityTypesCheckGenericIndexError{
        .code = UtilityTypesCheckGenericIndexErrorCode::NOT_INDEXED_BY_KEY,
        .message = message,
    });
  }
  index->type =
      this->checker.typeOperators().evalTypeOp(Type::indexedAccess(object, key), {object, key});

  return ok(index);
}

bool UtilityTypes::wantsLiteral(const Type *param) const {
  const Type *constraint = param->isParam() ? param->constraint() : nullptr;
  if (!constraint) {
    return false;
  }
  if (constraint->typeOp() == TypeOp::KeyOf) {
    return true;
  }
  const Type *c = constraint;

  return llvm::all_of(UtilityTypes::membersOf(c),
                      [](const Type *m) { return m->isStringLiteral(); });
}

}
