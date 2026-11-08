#include "compiler/semantic/bodies/objects/object_literal_checker/object_literal_checker.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/classes/method_literal_checker/method_literal_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/objects/keyed_field_checker/keyed_field_checker.h"
#include "compiler/semantic/bodies/objects/record_literal_checker/record_literal_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/library/json/json_checker/json_checker.h"
#include "compiler/semantic/types/literal_types/literal_types.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;

namespace tz {

bool ObjectLiteralChecker::hasMethodFields(const ClassDecl *cls) {
  if (cls->literal()) {
    return true;
  }
  for (const ClassDecl *c = cls; c; c = c->parent) {
    if (llvm::any_of(c->fields, [](const ClassField &f) { return f.method(); })) {
      return true;
    }
  }

  return false;
}

ObjectLiteralChecker::ObjectLiteralChecker(SemanticState &owner)
    : state(owner) {}

ObjectLiteralChecker::ResultPropertyHasValue
ObjectLiteralChecker::propertyPlain(const ObjectLitExpr::Property &prop) {
  if (prop.defaulted()) {
    std::string message = ErrorTextObjectLiteral::defaulted(prop.name);

    return err(ObjectLiteralCheckerPropertyPlainError{
        .code = ObjectLiteralCheckerPropertyPlainErrorCode::DEFAULT_VALUE,
        .message = message,
    });
  }

  return ok(&prop);
}

ObjectLiteralChecker::ResultComputedKeyKnown
ObjectLiteralChecker::computedKey(const Type *keyType) {
  if (keyType->nonOptional()->isNumeric()) {
    std::string message = ErrorTextObjectLiteral::numberKey();

    return err(ObjectLiteralCheckerComputedKeyError{
        .code = ObjectLiteralCheckerComputedKeyErrorCode::NUMBER_KEY,
        .message = message,
    });
  }
  if (keyType->isString()) {
    std::string message = ErrorTextObjectLiteral::runtimeKey();

    return err(ObjectLiteralCheckerComputedKeyError{
        .code = ObjectLiteralCheckerComputedKeyErrorCode::RUNTIME_KEY,
        .message = message,
    });
  }
  std::string message = ErrorTextObjectLiteral::keyType(keyType->name());

  return err(ObjectLiteralCheckerComputedKeyError{
      .code = ObjectLiteralCheckerComputedKeyErrorCode::NOT_A_STRING,
      .message = message,
  });
}

ObjectLiteralChecker::ResultFieldNameIsFresh
ObjectLiteralChecker::uniqueName(const ObjectLitExpr *lit, size_t i, llvm::ArrayRef<bool> named) {
  for (size_t j = 0; j < i && named[i]; ++j) {
    if (named[j] && lit->props[j].name == lit->props[i].name) {
      std::string message = ErrorTextObjectLiteral::duplicate(lit->props[i].name);

      return err(ObjectLiteralCheckerUniqueNameError{
          .code = ObjectLiteralCheckerUniqueNameErrorCode::DUPLICATE_FIELD,
          .message = message,
      });
    }
  }

  return ok(&lit->props[i]);
}

ObjectLiteralChecker::ResultValueTypeIsKnown
ObjectLiteralChecker::inferredValue(const Type *value) {
  if (value->isVoid() || value->isUndefined()) {
    std::string message = ErrorTextObjectLiteral::inferFrom(value->name());

    return err(ObjectLiteralCheckerInferredValueError{
        .code = ObjectLiteralCheckerInferredValueErrorCode::CANNOT_INFER,
        .message = message,
    });
  }

  return ok(value);
}

ObjectLiteralChecker::ResultKeyedValuesAgree
ObjectLiteralChecker::sameKeyedType(llvm::StringRef name, const Field &same, const Type *value) {
  if (same.type != Type::optional(value)) {
    std::string message = ErrorTextObjectLiteral::keyTypes(name);

    return err(ObjectLiteralCheckerSameKeyedTypeError{
        .code = ObjectLiteralCheckerSameKeyedTypeErrorCode::FIELD_TYPES_DIFFER,
        .message = message,
    });
  }

  return ok(value);
}

ObjectLiteralChecker::ResultKeyWithoutSpread
ObjectLiteralChecker::keyedPlacement(const ObjectLitExpr *lit, const Type *keyType) {
  bool methods =
      llvm::any_of(lit->props, [](const ObjectLitExpr::Property &p) { return p.method(); });
  if (lit->hasSpread() || methods) {
    std::string message = ErrorTextObjectLiteral::keySpread(keyType->name());

    return err(ObjectLiteralCheckerKeyedPlacementError{
        .code = ObjectLiteralCheckerKeyedPlacementErrorCode::KEY_WITH_SPREAD,
        .message = message,
    });
  }

  return ok(keyType);
}

ObjectLiteralChecker::ResultKeyedObjectTyped
ObjectLiteralChecker::keyedTarget(const Type *target, const Type *keyType) {
  if (!target) {
    std::string message = ErrorTextObjectLiteral::keyNoType(keyType->name());

    return err(ObjectLiteralCheckerKeyedTargetError{
        .code = ObjectLiteralCheckerKeyedTargetErrorCode::NO_OBJECT_TYPE,
        .message = message,
    });
  }

  return ok(target);
}

ObjectLiteralChecker::ResultKeyedFieldsAgree ObjectLiteralChecker::keyedField(const Type *target,
                                                                              const Type *keyType) {
  const Type *field = KeyedFieldChecker::keyedFieldType(target, keyType);
  if (!field) {
    std::string message = ErrorTextObjectLiteral::keyFields(keyType->name(), target->name());

    return err(ObjectLiteralCheckerKeyedFieldError{
        .code = ObjectLiteralCheckerKeyedFieldErrorCode::FIELDS_DIFFER,
        .message = message,
    });
  }
  if (!field->isOptional()) {
    std::string message = ErrorTextObjectLiteral::keyedOpt(keyType->name(), field->name());

    return err(ObjectLiteralCheckerKeyedFieldError{
        .code = ObjectLiteralCheckerKeyedFieldErrorCode::NOT_OPTIONAL,
        .message = message,
    });
  }

  return ok(field);
}

ObjectLiteralChecker::ResultTargetFieldFound
ObjectLiteralChecker::targetField(const Type *target, llvm::StringRef name) {
  const Field *field = target->field(name);
  if (!field && target->overload(name)) {
    return err(ObjectLiteralCheckerTargetFieldError{
        .code = ObjectLiteralCheckerTargetFieldErrorCode::OVERLOAD_NAME,
        .message = "",
    });
  }
  if (!field) {
    bool stream = target->name() == "StreamError";
    std::string message = ErrorTextObjectLiteral::noField(target->name(), name, stream);

    return err(ObjectLiteralCheckerTargetFieldError{
        .code = ObjectLiteralCheckerTargetFieldErrorCode::NO_SUCH_FIELD,
        .message = message,
    });
  }

  return ok(field);
}

ObjectLiteralChecker::ResultUsableFieldValue
ObjectLiteralChecker::valueTyped(const Type *type, llvm::StringRef what, llvm::StringRef name) {
  if (type->isVoid()) {
    std::string message = ErrorTextObjectLiteral::voidValue(what);

    return err(ObjectLiteralCheckerValueTypedError{
        .code = ObjectLiteralCheckerValueTypedErrorCode::VOID_VALUE,
        .message = message,
    });
  }
  if (type->isUndefined()) {
    std::string message = ErrorTextObjectLiteral::undefOf(what, name);

    return err(ObjectLiteralCheckerValueTypedError{
        .code = ObjectLiteralCheckerValueTypedErrorCode::UNDEFINED_VALUE,
        .message = message,
    });
  }

  return ok(type);
}

ObjectLiteralChecker::ResultRequiredFieldSet
ObjectLiteralChecker::fieldGiven(const ObjectLitExpr *lit, const Field &field, const Type *target) {
  bool given = llvm::any_of(lit->props,
                            [&](const ObjectLitExpr::Property &p) { return p.name == field.name; });
  if (!given && !field.type->isOptional()) {
    std::string message = ErrorTextObjectLiteral::missing(field.name, target->name());

    return err(ObjectLiteralCheckerFieldGivenError{
        .code = ObjectLiteralCheckerFieldGivenErrorCode::MISSING_FIELD,
        .message = message,
    });
  }

  return ok(&field);
}

Expr *ObjectLiteralChecker::checkObjectLit(ObjectLitExpr *lit, const Type *expected) {
  if (expected && expected->nonOptional()->isJson()) {
    return JsonChecker(this->state).checkJsonObjectLit(lit);
  }
  if (expected && expected->nonOptional()->isRecord()) {
    return RecordLiteralChecker(this->state).checkRecordLit(lit, expected->nonOptional());
  }
  const Type *target = expected ? expected->nonOptional() : nullptr;
  const Type *expectedUnion = target && target->isUnion() ? target : nullptr;
  auto usable = [](const Type *t) {
    return t && t->isObject() && !t->isIncomplete() && !t->isTuple() && !t->isClass() &&
           !t->isRegExp() && !t->isMatch() && !t->isDate() && !t->isIntl() && !t->isBytes() &&
           !t->isCrypto() && !t->isAnyObject();
  };
  if (!usable(target)) {
    target = nullptr;
  }
  bool typed = true;

  llvm::SmallVector<bool, 8> named(lit->props.size(), true);
  for (size_t i = 0; i < lit->props.size(); ++i) {
    ObjectLitExpr::Property &prop = lit->props[i];
    named[i] = !prop.spread();
    ResultPropertyHasValue plain = ObjectLiteralChecker::propertyPlain(prop);
    if (plain.isErr()) {
      this->state.getDiagnostics().error(prop.loc, plain.error().message);
      prop.value = cast<AssignExpr>(prop.value)->value;
      prop.form = ValueProperty{};
      typed = false;
    }
    ComputedProperty *computed = prop.form.as<ComputedProperty>();
    if (!computed) {
      continue;
    }
    computed->key = ExprChecker(this->state).checkExpr(computed->key);
    const Type *keyType = prop.key()->type;
    if (std::optional<std::string> name = KeyedFieldChecker(this->state).constantKey(prop.key())) {
      prop.name = this->state.getAst().keepName(std::move(*name));
      this->state.getInventoryStore().addFieldName(prop.name);
      continue;
    }
    named[i] = false;
    llvm::ArrayRef<const Type *> keyNames =
        keyType->isUnion() ? keyType->members() : llvm::ArrayRef<const Type *>(keyType);
    if (llvm::all_of(keyNames, [](const Type *n) { return n->isStringLiteral(); })) {
      computed->keyed = true;
      continue;
    }
    typed = false;
    if (keyType->isError()) {
      continue;
    }
    ResultComputedKeyKnown key = ObjectLiteralChecker::computedKey(keyType);
    if (key.isErr()) {
      this->state.getDiagnostics().error(prop.key()->loc(), key.error().message);
    }
  }

  for (size_t i = 0; i < lit->props.size(); ++i) {
    ResultFieldNameIsFresh fresh = ObjectLiteralChecker::uniqueName(lit, i, named);
    if (fresh.isErr()) {
      this->state.getDiagnostics().error(lit->props[i].loc, fresh.error().message);
      typed = false;
    }
  }
  if (expectedUnion) {
    target = LiteralTypes(this->state).literalMember(lit, expectedUnion);
    if (!usable(target)) {
      target = nullptr;
    }
    if (!target) {
      for (const Type *m : expectedUnion->members()) {
        if (m->isRecord()) {
          return RecordLiteralChecker(this->state).checkRecordLit(lit, m);
        }
      }
    }
  }
  if (target) {
    for (ObjectLitExpr::Property &prop : lit->props) {
      if (const Field *wanted = prop.method() ? target->field(prop.name) : nullptr;
          wanted && wanted->type->nonOptional()->isFunction() &&
          wanted->type->nonOptional()->hasThis()) {
        prop.form = ValueProperty{};
      }
    }
  }
  bool inferred = false;
  if (!target && !lit->props.empty() &&
      llvm::all_of(lit->props, [](const ObjectLitExpr::Property &p) { return p.keyed(); })) {
    std::vector<Field> fields;
    for (ObjectLitExpr::Property &prop : lit->props) {
      prop.value = ExprChecker(this->state).checkExpr(prop.value);
      const Type *value = prop.value->type;
      if (value->isError()) {
        lit->type = Type::error();
        return lit;
      }
      ResultValueTypeIsKnown known = ObjectLiteralChecker::inferredValue(value);
      if (known.isErr()) {
        this->state.getDiagnostics().error(prop.value->loc(), known.error().message);
        lit->type = Type::error();
        return lit;
      }
      const Type *keyType = prop.key()->type;
      for (const Type *n :
           keyType->isUnion() ? keyType->members() : llvm::ArrayRef<const Type *>(keyType)) {
        std::string name = ToUtf8::toUtf8(n->literalText());
        auto same = llvm::find_if(fields, [&](const Field &f) { return f.name == name; });
        if (same == fields.end()) {
          fields.push_back({name, Type::optional(value), false});
          continue;
        }
        ResultKeyedValuesAgree agree = ObjectLiteralChecker::sameKeyedType(name, *same, value);
        if (agree.isErr()) {
          this->state.getDiagnostics().error(prop.key()->loc(), agree.error().message);
          lit->type = Type::error();
          return lit;
        }
      }
    }
    target = Type::object(std::move(fields));
    inferred = true;
  }
  for (ObjectLitExpr::Property &prop : lit->props) {
    if (!prop.keyed()) {
      continue;
    }
    const Type *keyType = prop.key()->type;
    ResultKeyWithoutSpread placed = ObjectLiteralChecker::keyedPlacement(lit, keyType);
    if (placed.isErr()) {
      this->state.getDiagnostics().error(prop.key()->loc(), placed.error().message);
      return lit;
    }
    ResultKeyedObjectTyped object = ObjectLiteralChecker::keyedTarget(target, keyType);
    if (object.isErr()) {
      this->state.getDiagnostics().error(prop.key()->loc(), object.error().message);
      prop.value = ExprChecker(this->state).checkExpr(prop.value);
      lit->type = Type::error();
      return lit;
    }
    ResultKeyedFieldsAgree agree = ObjectLiteralChecker::keyedField(target, keyType);
    if (agree.isErr()) {
      this->state.getDiagnostics().error(prop.key()->loc(), agree.error().message);
      prop.value = ExprChecker(this->state).checkExpr(prop.value);
      lit->type = Type::error();
      return lit;
    }
    const Type *field = agree.value();
    std::string what = ErrorTextObjectLiteral::keyedName(keyType->name());
    prop.value =
        TypeRelations(this->state)
            .coerce(inferred ? prop.value : ExprChecker(this->state).checkExpr(prop.value, field),
                    field, what);
  }
  if (llvm::any_of(lit->props, [](const ObjectLitExpr::Property &p) { return p.method(); })) {
    return MethodLiteralChecker(this->state).checkMethodLiteral(lit, target);
  }
  if (lit->hasSpread()) {
    return this->checkSpreadLit(lit, target, named, typed);
  }

  std::vector<Field> fields;
  for (size_t i = 0; i < lit->props.size(); ++i) {
    ObjectLitExpr::Property &prop = lit->props[i];
    if (prop.keyed()) {
      continue;
    }
    if (!named[i]) {
      prop.value = ExprChecker(this->state).checkExpr(prop.value);
      continue;
    }
    if (auto *fe = dyn_cast<FunctionExpr>(prop.value); fe && fe->fn->name.empty()) {
      fe->fn->name = prop.name;
    }
    std::string what = ErrorTextObjectLiteral::fieldName(prop.name);
    if (target) {
      ResultTargetFieldFound found = ObjectLiteralChecker::targetField(target, prop.name);
      if (found.isErr()) {
        bool prechecked = this->state.getExpressionMarks().isPrechecked(prop.value);
        this->state.getExpressionMarks().forgetPrechecked(prop.value);
        if (!prechecked) {
          prop.value = ExprChecker(this->state).checkExpr(prop.value);
        }
        if (found.error().code != ObjectLiteralCheckerTargetFieldErrorCode::OVERLOAD_NAME) {
          this->state.getDiagnostics().error(prop.loc, found.error().message);
        }
        continue;
      }
      const Field *field = found.value();
      bool prechecked = this->state.getExpressionMarks().isPrechecked(prop.value);
      this->state.getExpressionMarks().forgetPrechecked(prop.value);
      Expr *value =
          prechecked ? prop.value : ExprChecker(this->state).checkExpr(prop.value, field->type);
      prop.value = TypeRelations(this->state).coerce(value, field->type, what);
      continue;
    }
    bool prechecked = this->state.getExpressionMarks().isPrechecked(prop.value);
    this->state.getExpressionMarks().forgetPrechecked(prop.value);
    if (!prechecked) {
      prop.value = ExprChecker(this->state).checkExpr(prop.value);
    }
    const Type *type = prop.value->type;
    if (type->isError()) {
      typed = false;
    } else {
      ResultUsableFieldValue usableValue = ObjectLiteralChecker::valueTyped(type, what, prop.name);
      if (usableValue.isErr()) {
        this->state.getDiagnostics().error(prop.value->loc(), usableValue.error().message);
        typed = false;
      }
    }
    fields.push_back({prop.name.str(), type, false});
  }

  if (target) {
    for (const Field &f : target->fields()) {
      ResultRequiredFieldSet given = ObjectLiteralChecker::fieldGiven(lit, f, target);
      if (given.isErr()) {
        this->state.getDiagnostics().error(lit->loc(), given.error().message);
      }
    }
    for (const Overload &o : target->overloads()) {
      std::string message = ErrorTextObjectLiteral::overloads(target->name(), o.name);
      this->state.getDiagnostics().error(lit->loc(), message);
    }
    lit->type = target;
    return lit;
  }
  lit->type = typed ? Type::object(std::move(fields)) : Type::error();

  return lit;
}

ObjectLiteralChecker::ResultDefinedSpreading ObjectLiteralChecker::spreadDefined(const Type *type) {
  if (type->isOptional() && type->inner()->isObject() && !type->inner()->isTuple()) {
    std::string message = ErrorTextSpreadLiteral::undefined();

    return err(ObjectLiteralCheckerSpreadDefinedError{
        .code = ObjectLiteralCheckerSpreadDefinedErrorCode::MAY_BE_UNDEFINED,
        .message = message,
    });
  }

  return ok(type);
}

ObjectLiteralChecker::ResultSpreadIsAnObject ObjectLiteralChecker::spreadObject(const Type *type) {
  bool object = type->isObject() && !type->isTuple() && !type->isRegExp() && !type->isMatch() &&
                !type->isDate() && !type->isIntl() && !type->isBytes() && !type->isCrypto();
  if (!object) {
    bool indices =
        type->nonOptional()->isArray() || type->nonOptional()->isTuple() || type->isString();
    std::string message = ErrorTextSpreadLiteral::notObject(type->name(), indices);

    return err(ObjectLiteralCheckerSpreadObjectError{
        .code = ObjectLiteralCheckerSpreadObjectErrorCode::NOT_AN_OBJECT,
        .message = message,
    });
  }

  return ok(type);
}

ObjectLiteralChecker::ResultPlainSpreadClass
ObjectLiteralChecker::spreadFields(const ClassDecl *cls) {
  if (ObjectLiteralChecker::hasMethodFields(cls)) {
    std::string message = ErrorTextSpreadLiteral::methods();

    return err(ObjectLiteralCheckerSpreadFieldsError{
        .code = ObjectLiteralCheckerSpreadFieldsErrorCode::METHOD_FIELDS,
        .message = message,
    });
  }

  return ok(cls);
}

ObjectLiteralChecker::ResultSpreadFieldJoins
ObjectLiteralChecker::spreadJoin(const Field &spread, const Field &field, const Type *type) {
  const Type *base = TypeRelations::unify(field.type->nonOptional(), spread.type->inner());
  if (!base || base->isError()) {
    std::string message = ErrorTextSpreadLiteral::twoTypes(spread.name, field.type->name(),
                                                           spread.type->name(), type->name());

    return err(ObjectLiteralCheckerSpreadJoinError{
        .code = ObjectLiteralCheckerSpreadJoinErrorCode::TYPES_DIFFER,
        .message = message,
    });
  }

  return ok(base);
}

Expr *ObjectLiteralChecker::checkSpreadLit(ObjectLitExpr *lit, const Type *target,
                                           llvm::ArrayRef<bool> named, bool typed) {
  bool records = false;
  std::vector<Field> fields;
  auto slot = [&](llvm::StringRef name) -> Field * {
    for (Field &f : fields) {
      if (f.name == name) {
        return &f;
      }
    }

    return nullptr;
  };
  for (size_t i = 0; i < lit->props.size(); ++i) {
    ObjectLitExpr::Property &prop = lit->props[i];
    if (!prop.spread()) {
      if (!named[i]) {
        prop.value = ExprChecker(this->state).checkExpr(prop.value);
        continue;
      }
      if (auto *fe = dyn_cast<FunctionExpr>(prop.value); fe && fe->fn->name.empty()) {
        fe->fn->name = prop.name;
      }
      const Field *wanted = target ? target->field(prop.name) : nullptr;
      prop.value = ExprChecker(this->state).checkExpr(prop.value, wanted ? wanted->type : nullptr);
      const Type *type = prop.value->type;
      Field *field = slot(prop.name);
      std::string what = ErrorTextObjectLiteral::fieldName(prop.name);
      if (type->isError()) {
        typed = false;
        continue;
      }
      bool filled = type->isUndefined() && field;
      ResultUsableFieldValue usableValue =
          filled ? ResultUsableFieldValue(ok(type))
                 : ObjectLiteralChecker::valueTyped(type, what, prop.name);
      if (usableValue.isErr()) {
        this->state.getDiagnostics().error(prop.value->loc(), usableValue.error().message);
        typed = false;
        continue;
      }
      if (filled) {
        type = Type::optional(field->type);
      }
      if (field) {
        field->type = type;
      } else {
        fields.push_back({prop.name.str(), type, false});
      }
      continue;
    }

    prop.value = ExprChecker(this->state).checkExpr(prop.value);
    const Type *type = prop.value->type;
    if (type->isError()) {
      typed = false;
      continue;
    }
    if (type->isRecord() && !target) {
      records = true;
      continue;
    }
    ResultDefinedSpreading defined = ObjectLiteralChecker::spreadDefined(type);
    if (defined.isErr()) {
      this->state.getDiagnostics().error(prop.value->loc(), defined.error().message);
      typed = false;
      continue;
    }
    ResultSpreadIsAnObject object = ObjectLiteralChecker::spreadObject(type);
    if (object.isErr()) {
      this->state.getDiagnostics().error(prop.value->loc(), object.error().message);
      typed = false;
      continue;
    }
    SpreadProperty *spread = prop.form.as<SpreadProperty>();
    for (const IndexExpr *read : spread->omitKeys) {
      if (!read->field().empty()) {
        spread->omit.push_back(read->field());
      }
    }
    spread->omitKeys.clear();
    if (ClassDecl *cls = ClassDeclarer::classOf(type)) {
      ClassDeclarer(this->state).completeClass(cls);
      ResultPlainSpreadClass plain = ObjectLiteralChecker::spreadFields(cls);
      if (plain.isErr()) {
        this->state.getDiagnostics().error(prop.value->loc(), plain.error().message);
        typed = false;
        continue;
      }
    }
    for (const Field &f : type->fields()) {
      if (llvm::is_contained(prop.omit(), f.name)) {
        continue;
      }
      Field *field = slot(f.name);
      if (!field) {
        fields.push_back({f.name, f.type, false});
        continue;
      }
      if (!f.type->isOptional()) {
        field->type = f.type;
        continue;
      }
      ResultSpreadFieldJoins joined = ObjectLiteralChecker::spreadJoin(f, *field, type);
      if (joined.isErr()) {
        this->state.getDiagnostics().error(prop.loc, joined.error().message);
        typed = false;
        continue;
      }
      const Type *base = joined.value();
      field->type = field->type->isOptional() ? Type::optional(base) : base;
    }
  }
  if (records && typed) {
    return RecordLiteralChecker(this->state).recordFromSpreads(lit);
  }
  if (!typed) {
    lit->type = Type::error();
    return lit;
  }

  const Type *result = Type::object(fields);
  if (target) {
    bool fits = llvm::all_of(fields, [&](const Field &f) {
      const Field *t = target->field(f.name);

      return t && TypeRelations::isImplicitlyConvertible(f.type, t->type);
    });
    fits &= llvm::all_of(target->fields(),
                         [&](const Field &t) { return t.type->isOptional() || slot(t.name); });
    if (fits) {
      result = target;
    }
  }

  for (size_t i = 0; i < lit->props.size(); ++i) {
    ObjectLitExpr::Property &prop = lit->props[i];
    auto replacedLater = [&](llvm::StringRef name) {
      for (size_t j = i + 1; j < lit->props.size(); ++j) {
        const ObjectLitExpr::Property &later = lit->props[j];
        if (!later.spread()) {
          if (later.name == name) {
            return true;
          }
        } else if (!llvm::is_contained(later.omit(), name)) {
          if (const Field *f = later.value->type->field(name); f && !f->type->isOptional()) {
            return true;
          }
        }
      }

      return false;
    };
    if (!prop.spread()) {
      if (replacedLater(prop.name)) {
        prop.replaced = true;
      } else {
        prop.value = TypeRelations(this->state).convert(prop.value, result->field(prop.name)->type);
      }
      continue;
    }
    SpreadProperty *spread = prop.form.as<SpreadProperty>();
    for (const Field &f : prop.value->type->fields()) {
      if (!llvm::is_contained(spread->omit, f.name) && replacedLater(f.name)) {
        spread->replacedFields.push_back(f.name);
      }
    }
  }
  lit->type = result;

  return lit;
}

}
