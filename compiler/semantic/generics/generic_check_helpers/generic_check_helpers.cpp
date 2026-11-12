#include "compiler/semantic/generics/generic_check_helpers/generic_check_helpers.h"
#include "compiler/semantic/types/type_parts/type_routes.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

namespace {

constexpr llvm::StringLiteral kUnionSeparator = " | ";

}

bool GenericCheckHelpers::mentionsAnyOf(const Type *type, llvm::ArrayRef<const Type *> params) {
  if (params.empty() || !TypeQueries::mentionsParam(type)) {
    return false;
  }
  auto parts = TypeParts::collect(type, kMentionsRoute);
  for (const Type *part : parts.getTypes()) {
    if (GenericCheckHelpers::isParamOf(part, params)) {
      return true;
    }
  }

  return false;
}

bool GenericCheckHelpers::isParamOf(const Type *type, llvm::ArrayRef<const Type *> params) {
  bool isPlainParam = type->isParam() && type->typeOp() == TypeOp::None;

  return isPlainParam && llvm::is_contained(params, type);
}

std::string GenericCheckHelpers::typeNames(llvm::ArrayRef<const Type *> types,
                                           llvm::StringRef separator) {
  auto text = std::string();
  for (const Type *type : types) {
    if (!text.empty()) {
      text += separator.str();
    }
    text += type->name().str();
  }

  return text;
}

std::string GenericCheckHelpers::bindingNames(llvm::ArrayRef<const Type *> params,
                                              llvm::ArrayRef<const Type *> args) {
  auto text = std::string();
  size_t count = std::min(params.size(), args.size());
  for (size_t index = 0; index < count; ++index) {
    if (!text.empty()) {
      text += ", ";
    }
    text += params[index]->name().str() + " = " + args[index]->name().str();
  }

  return text;
}

llvm::StringRef GenericCheckHelpers::recordName(const InstanceRecord *record) {
  if (record->cls != nullptr) {
    return record->cls->name;
  }

  return record->generic->name;
}

bool GenericCheckHelpers::isDefaultArgument(llvm::ArrayRef<const Type *> defaults,
                                            llvm::ArrayRef<const Type *> args, size_t index) {
  if (index >= defaults.size() || defaults[index] == nullptr) {
    return false;
  }
  const Type *defaultType = defaults[index];
  const Type *arg = args[index];
  bool isBothNever = defaultType->isNever() && arg->isNever();

  return defaultType == arg || isBothNever;
}

size_t GenericCheckHelpers::shownArgumentCount(llvm::ArrayRef<const Type *> defaults,
                                               llvm::ArrayRef<const Type *> args) {
  size_t shown = args.size();
  while (shown > 0 && GenericCheckHelpers::isDefaultArgument(defaults, args, shown - 1)) {
    --shown;
  }

  return shown;
}

bool GenericCheckHelpers::isUnionOfNames(const TypeRef &ref) {
  if (ref.kind() != TypeRefKind::UNION_REF) {
    return false;
  }
  for (const TypeRef &member : ref.args) {
    if (member.kind() != TypeRefKind::NAME_REF) {
      return false;
    }
  }

  return true;
}

std::string GenericCheckHelpers::refNames(llvm::ArrayRef<TypeRef> refs) {
  auto text = std::string();
  for (const TypeRef &ref : refs) {
    if (!text.empty()) {
      text += kUnionSeparator.str();
    }
    text += ref.name.str();
  }

  return text;
}

std::string GenericCheckHelpers::constraintSpelling(const Type *param,
                                                    const TypeParamDecl *typeParam) {
  bool isWritten = typeParam != nullptr && typeParam->constraint.has_value();
  if (isWritten && typeParam->constraint->kind() == TypeRefKind::NAME_REF) {
    return typeParam->constraint->name.str();
  }
  if (isWritten && GenericCheckHelpers::isUnionOfNames(*typeParam->constraint)) {
    return GenericCheckHelpers::refNames(typeParam->constraint->args);
  }
  const Type *constraint = param->constraint();
  if (constraint != nullptr) {
    return constraint->name().str();
  }

  return GenericCheckHelpers::typeNames(param->alternatives(), kUnionSeparator);
}

}
