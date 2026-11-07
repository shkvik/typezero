#include "compiler/pipeline/check_program/topics/objects/record_literal_checker/record_literal_checker.h"
#include "compiler/pipeline/check_program/declare/class_declarer/class_declarer.h"
#include "compiler/pipeline/check_program/topics/expr/expr_checker/expr_checker.h"
#include "compiler/pipeline/check_program/topics/objects/dict_checker/dict_checker.h"
#include "compiler/pipeline/check_program/types/type_relations/type_relations.h"
#include "compiler/pipeline/check_program/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;

namespace tz {

RecordLiteralChecker::RecordLiteralChecker(CheckState &owner)
    : state(owner) {}

RecordLiteralChecker::ResultPlainProperty
RecordLiteralChecker::plainProperty(const ObjectLitExpr::Property &prop) {
  if (prop.defaulted()) {
    std::string message = ErrorTextRecordProperty::defaulted(prop.name);

    return err(RecordLiteralCheckerPlainPropertyError{
        .code = RecordLiteralCheckerPlainPropertyErrorCode::DEFAULT_IN_LITERAL,
        .message = message,
    });
  }
  if (prop.method()) {
    std::string message = ErrorTextRecordProperty::method(prop.name);

    return err(RecordLiteralCheckerPlainPropertyError{
        .code = RecordLiteralCheckerPlainPropertyErrorCode::METHOD_IN_RECORD,
        .message = message,
    });
  }

  return ok(&prop);
}

void RecordLiteralChecker::reportSpread(const ObjectLitExpr::Property &prop,
                                        const ResultSpreadIntoMap &spread) {
  llvm::SMLoc at = prop.value->loc();
  switch (spread.error().code) {
    case RecordLiteralCheckerCheckRecordSpreadErrorCode::KEYED_REST:
      at = prop.loc;
      break;
    case RecordLiteralCheckerCheckRecordSpreadErrorCode::MAY_BE_UNDEFINED:
    case RecordLiteralCheckerCheckRecordSpreadErrorCode::OTHER_VALUES:
    case RecordLiteralCheckerCheckRecordSpreadErrorCode::FIELD_TYPE_DIFFERS:
    case RecordLiteralCheckerCheckRecordSpreadErrorCode::HAS_METHODS:
    case RecordLiteralCheckerCheckRecordSpreadErrorCode::NOT_SPREADABLE:
      break;
  }
  this->state.getDiagnostics().error(at, spread.error().message);
}

Expr *RecordLiteralChecker::checkRecordLit(ObjectLitExpr *lit, const Type *record) {
  const Type *value = record->valueType();
  bool typed = true;
  for (ObjectLitExpr::Property &prop : lit->props) {
    ResultPlainProperty plain = RecordLiteralChecker::plainProperty(prop);
    if (plain.isErr()) {
      this->state.getDiagnostics().error(prop.loc, plain.error().message);
      typed = false;
    }
    if (prop.defaulted()) {
      prop.value = cast<AssignExpr>(prop.value)->value;
      prop.form = ValueProperty{};
    }
    if (prop.method()) {
      prop.value = ExprChecker(this->state).checkExpr(prop.value);
      continue;
    }
    if (prop.spread()) {
      prop.value = ExprChecker(this->state).checkExpr(prop.value, record);
      const Type *type = prop.value->type;
      if (type->isError()) {
        typed = false;
        continue;
      }
      ResultSpreadIntoMap spread = this->checkRecordSpread(prop, value);
      if (spread.isErr()) {
        this->reportSpread(prop, spread);
        typed = false;
      }
      continue;
    }
    if (ComputedProperty *computed = prop.form.as<ComputedProperty>()) {
      computed->key = DictChecker(this->state).checkRecordKey(computed->key);
    }
    if (auto *fe = dyn_cast<FunctionExpr>(prop.value); fe && fe->fn->name.empty()) {
      fe->fn->name = prop.name;
    }
    std::string what = prop.key() ? ErrorTextRecordProperty::valueLabel()
                                  : ErrorTextRecordProperty::keyLabel(prop.name);
    prop.value = TypeRelations(this->state)
                     .coerce(ExprChecker(this->state).checkExpr(prop.value, value), value, what);
  }
  lit->type = typed ? record : Type::error();

  return lit;
}

RecordLiteralChecker::ResultSpreadIntoMap
RecordLiteralChecker::checkRecordSpread(ObjectLitExpr::Property &prop, const Type *value) {
  const Type *type = prop.value->type;
  if (type->isOptional() && (type->inner()->isRecord() || type->inner()->isObject())) {
    std::string message = ErrorTextRecordSpread::undefined();

    return err(RecordLiteralCheckerCheckRecordSpreadError{
        .code = RecordLiteralCheckerCheckRecordSpreadErrorCode::MAY_BE_UNDEFINED,
        .message = message,
    });
  }
  if (!prop.omitKeys().empty()) {
    std::string message = ErrorTextRecordSpread::keyedRest();

    return err(RecordLiteralCheckerCheckRecordSpreadError{
        .code = RecordLiteralCheckerCheckRecordSpreadErrorCode::KEYED_REST,
        .message = message,
    });
  }
  if (type->isRecord() && !TypeRelations::isImplicitlyConvertible(type->valueType(), value)) {
    std::string message = ErrorTextRecordSpread::values(type->name(), value->name());

    return err(RecordLiteralCheckerCheckRecordSpreadError{
        .code = RecordLiteralCheckerCheckRecordSpreadErrorCode::OTHER_VALUES,
        .message = message,
    });
  }
  if (type->isRecord()) {
    return ok(type);
  }
  bool object = type->isObject() && !type->isTuple() && !type->isRegExp() && !type->isMatch() &&
                !type->isAnyObject();
  if (!object) {
    bool collection = type->isMap() || type->isSet();
    std::string message = ErrorTextRecordSpread::notObject(type->name(), collection);

    return err(RecordLiteralCheckerCheckRecordSpreadError{
        .code = RecordLiteralCheckerCheckRecordSpreadErrorCode::NOT_SPREADABLE,
        .message = message,
    });
  }
  if (ClassDecl *cls = ClassDeclarer::classOf(type)) {
    ClassDeclarer(this->state).completeClass(cls);
  }
  for (const Field &f : type->fields()) {
    if (llvm::is_contained(prop.omit(), f.name)) {
      continue;
    }
    const Type *t = f.type->isOptional() && !value->isOptional() ? f.type->inner() : f.type;
    if (!TypeRelations::isImplicitlyConvertible(t, value)) {
      std::string message =
          ErrorTextRecordSpread::field(f.name, type->name(), f.type->name(), value->name());

      return err(RecordLiteralCheckerCheckRecordSpreadError{
          .code = RecordLiteralCheckerCheckRecordSpreadErrorCode::FIELD_TYPE_DIFFERS,
          .message = message,
      });
    }
  }
  bool methods = false;
  auto isMethod = [](const ClassField &f) { return f.method(); };
  for (const ClassDecl *c = ClassDeclarer::classOf(type); c; c = c->parent) {
    methods |= c->literal() || llvm::any_of(c->fields, isMethod);
  }
  if (methods) {
    std::string message = ErrorTextRecordSpread::methods();

    return err(RecordLiteralCheckerCheckRecordSpreadError{
        .code = RecordLiteralCheckerCheckRecordSpreadErrorCode::HAS_METHODS,
        .message = message,
    });
  }

  return ok(type);
}

RecordLiteralChecker::ResultValueTypeJoin RecordLiteralChecker::valuesJoin(const Type *value,
                                                                           const Type *type) {
  const Type *next = value ? TypeRelations::unify(value, type) : type;
  if (!next || next->isError()) {
    std::string message = ErrorTextRecordValues::differ(value->name(), type->name());

    return err(RecordLiteralCheckerValuesJoinError{
        .code = RecordLiteralCheckerValuesJoinErrorCode::VALUE_TYPES_DIFFER,
        .message = message,
    });
  }

  return ok(next);
}

RecordLiteralChecker::ResultValueTypeKnow RecordLiteralChecker::valueKnown(const Type *value) {
  if (value->isUndefined() || value->isVoid()) {
    std::string message = ErrorTextRecordValue::undefined();

    return err(RecordLiteralCheckerValueKnownError{
        .code = RecordLiteralCheckerValueKnownErrorCode::ONLY_UNDEFINED,
        .message = message,
    });
  }

  return ok(value);
}

Expr *RecordLiteralChecker::recordFromSpreads(ObjectLitExpr *lit) {
  const Type *value = nullptr;
  bool typed = true;
  auto meet = [&](const Type *t, llvm::SMLoc loc) {
    if (!t || t->isError()) {
      typed = false;
      return;
    }
    ResultValueTypeJoin joined = RecordLiteralChecker::valuesJoin(value, t);
    if (joined.isErr()) {
      this->state.getDiagnostics().error(loc, joined.error().message);
      typed = false;
      return;
    }
    value = joined.value();
  };
  for (const ObjectLitExpr::Property &prop : lit->props) {
    const Type *type = prop.value->type;
    if (!prop.spread()) {
      meet(type, prop.value->loc());
      continue;
    }
    if (type->isRecord()) {
      meet(type->valueType(), prop.value->loc());
    } else if (type->isObject()) {
      for (const Field &f : type->fields()) {
        if (!llvm::is_contained(prop.omit(), f.name)) {
          meet(f.type->isOptional() ? f.type->inner() : f.type, prop.value->loc());
        }
      }
    }
  }
  if (!typed || !value) {
    lit->type = Type::error();
    return lit;
  }
  ResultValueTypeKnow known = RecordLiteralChecker::valueKnown(value);
  if (known.isErr()) {
    this->state.getDiagnostics().error(lit->loc(), known.error().message);
    lit->type = Type::error();
    return lit;
  }
  const Type *record = Type::dict(Type::DictKind::Record, Type::string(), value);
  for (ObjectLitExpr::Property &prop : lit->props) {
    if (!prop.spread()) {
      prop.value = TypeRelations(this->state).convert(prop.value, value);
      continue;
    }
    ResultSpreadIntoMap spread = this->checkRecordSpread(prop, value);
    if (spread.isErr()) {
      this->reportSpread(prop, spread);
      typed = false;
    }
  }
  lit->type = typed ? record : Type::error();

  return lit;
}

}
