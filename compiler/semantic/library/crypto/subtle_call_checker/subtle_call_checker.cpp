#include "compiler/semantic/library/crypto/subtle_call_checker/subtle_call_checker.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/crypto/crypto_api/crypto_api.h"
#include "compiler/program/library/crypto/curve_name_table/curve_name_table.h"
#include "compiler/program/library/crypto/subtle_family_table/subtle_family_table.h"
#include "compiler/program/library/crypto/subtle_key_usage_table/subtle_key_usage_table.h"
#include "compiler/program/library/json/json_static_table/json_static_table.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/classes/method_call_checker/method_call_checker.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/library/crypto/subtle_algorithms/subtle_algorithms.h"
#include "compiler/semantic/library/events/event_target_checker/event_target_checker.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <functional>

using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

SubtleCallChecker::SubtleCallChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.scopes;
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
  this->classDeclarer = &all.classDeclarer;
  this->argumentChecker = &all.argumentChecker;
  this->methodCallChecker = &all.methodCallChecker;
  this->eventTargetChecker = &all.eventTargetChecker;
}

SubtleCallChecker::ResultKeyFormatKnown
SubtleCallChecker::checkKnownFormat(llvm::StringRef format) const {
  auto known = SubtleKeyFormatTable::find(format);
  if (!known.has_value()) {
    auto message = ErrorTextKeyFormat::unknown(format);

    return err(SubtleCallCheckerKnownFormatError{
        .code = SubtleCallCheckerKnownFormatErrorCode::UNKNOWN_FORMAT,
        .message = message,
    });
  }

  return ok(*known);
}

std::optional<double> SubtleCallChecker::numberLiteral(const Expr *expr) const {
  const auto *integer = dyn_cast_or_null<IntLitExpr>(expr);
  if (integer != nullptr) {
    return double(integer->value);
  }
  const auto *fraction = dyn_cast_or_null<FloatLitExpr>(expr);
  if (fraction != nullptr) {
    return fraction->value;
  }

  return std::nullopt;
}

const ObjectLitExpr::Property *SubtleCallChecker::propertyNamed(const ObjectLitExpr *literal,
                                                                llvm::StringRef name) const {
  for (const ObjectLitExpr::Property &property : literal->props) {
    if (!property.spread() && !property.key() && property.name == name) {
      return &property;
    }
  }

  return nullptr;
}

const ObjectLitExpr::Property *SubtleCallChecker::memberProperty(const ObjectLitExpr *literal,
                                                                 SubtleMember member) const {
  auto name = SubtleMemberTable::getName(member);

  return this->propertyNamed(literal, name);
}

SubtleCallChecker::ResultAlgorithmKnown
SubtleCallChecker::checkKnownAlgorithm(llvm::StringRef written, SubtleOperation operation,
                                       std::optional<SubtleOperation> fallback) const {
  std::string upper = written.upper();
  const SubtleAlgorithmInfo *known = SubtleAlgorithmTable::findIn(operation, upper);
  if (known == nullptr && fallback.has_value()) {
    known = SubtleAlgorithmTable::findIn(*fallback, upper);
  }
  if (known != nullptr) {
    return ok(known);
  }
  auto list = ErrorTextAlgorithm::choices(SubtleAlgorithmTable::getNames(operation));
  auto verb = ErrorTextAlgorithm::verb(operation);
  auto operationName = SubtleOperationTable::getName(operation);
  if (SubtleAlgorithmTable::isExperimental(upper)) {
    auto message = ErrorTextAlgorithm::experiment(written, verb, list);

    return err(SubtleCallCheckerKnownAlgorithmError{
        .code = SubtleCallCheckerKnownAlgorithmErrorCode::EXPERIMENTAL,
        .message = message,
    });
  }
  if (SubtleAlgorithmTable::findUpper(upper) != nullptr) {
    auto message = ErrorTextAlgorithm::otherOp(written, operationName, verb, list);

    return err(SubtleCallCheckerKnownAlgorithmError{
        .code = SubtleCallCheckerKnownAlgorithmErrorCode::OTHER_OPERATION,
        .message = message,
    });
  }
  auto message = ErrorTextAlgorithm::unknown(written, operationName, verb, list);

  return err(SubtleCallCheckerKnownAlgorithmError{
      .code = SubtleCallCheckerKnownAlgorithmErrorCode::UNKNOWN_ALGORITHM,
      .message = message,
  });
}

SubtleCallChecker::ResultParamsAsObject
SubtleCallChecker::checkParamsAsObject(const SubtleAlgorithmInfo &algorithm,
                                       SubtleOperation operation,
                                       const Dictionary &dictionary) const {
  if (!dictionary.required.empty()) {
    llvm::StringRef member = dictionary.required[0];
    auto example = SubtleMemberTable::getExample(member);
    auto operationName = SubtleOperationTable::getName(operation);
    auto message =
        ErrorTextAlgorithm::object(algorithm.name, operationName, dictionary.name, member, example);

    return err(SubtleCallCheckerParamsAsObjectError{
        .code = SubtleCallCheckerParamsAsObjectErrorCode::NEEDS_OBJECT,
        .message = message,
    });
  }

  return ok(&algorithm);
}

SubtleCallChecker::ResultMembersPresent
SubtleCallChecker::checkMembersPresent(const SubtleAlgorithmInfo &algorithm,
                                       SubtleOperation operation, const Dictionary &dictionary,
                                       const ObjectLitExpr *literal) const {
  for (llvm::StringRef member : dictionary.required) {
    if (this->propertyNamed(literal, member) == nullptr) {
      auto example = SubtleMemberTable::getExample(member);
      auto operationName = SubtleOperationTable::getName(operation);
      auto message = ErrorTextAlgorithm::needs(algorithm.name, operationName, member,
                                               dictionary.name, example);

      return err(SubtleCallCheckerMembersPresentError{
          .code = SubtleCallCheckerMembersPresentErrorCode::MISSING_MEMBER,
          .message = message,
      });
    }
  }

  return ok(literal);
}

SubtleCallChecker::ResultShaHashIsKnown
SubtleCallChecker::checkKnownShaHash(llvm::StringRef hash, llvm::StringRef dictionary) const {
  bool isSha = SubtleAlgorithmTable::hasShaPrefix(hash);
  bool isKnownHash = SubtleAlgorithmTable::findIn(SubtleOperation::DIGEST, hash.upper()) != nullptr;
  if (isSha && isKnownHash) {
    return ok(hash);
  }
  if (isSha || SubtleAlgorithmTable::isExperimental(hash.upper())) {
    auto list =
        ErrorTextAlgorithm::choices(SubtleAlgorithmTable::getNames(SubtleOperation::DIGEST));
    auto message = ErrorTextAlgorithm::hash(hash, dictionary, list);

    return err(SubtleCallCheckerKnownShaHashError{
        .code = SubtleCallCheckerKnownShaHashErrorCode::UNKNOWN_HASH,
        .message = message,
    });
  }
  auto message = ErrorTextAlgorithm::notSha(hash, dictionary);

  return err(SubtleCallCheckerKnownShaHashError{
      .code = SubtleCallCheckerKnownShaHashErrorCode::NOT_SHA,
      .message = message,
  });
}

SubtleCallChecker::ResultCurveNameKnown
SubtleCallChecker::checkKnownCurve(llvm::StringRef curve) const {
  if (!CurveNameTable::isWebCryptoCurve(curve)) {
    auto message = ErrorTextAlgorithm::curve(curve);

    return err(SubtleCallCheckerKnownCurveError{
        .code = SubtleCallCheckerKnownCurveErrorCode::UNKNOWN_CURVE,
        .message = message,
    });
  }

  return ok(curve);
}

SubtleCallChecker::ResultAesLengthValid SubtleCallChecker::checkAesLength(double bits) const {
  if (bits != 128 && bits != 192 && bits != 256) {
    auto message = ErrorTextAlgorithm::aesLength();

    return err(SubtleCallCheckerAesLengthError{
        .code = SubtleCallCheckerAesLengthErrorCode::BAD_LENGTH,
        .message = message,
    });
  }

  return ok(bits);
}

SubtleCallChecker::ResultGcmTagLengthOk SubtleCallChecker::checkGcmTagLength(double bits) const {
  if (bits != 32 && bits != 64 && bits != 96 && bits != 104 && bits != 112 && bits != 120 &&
      bits != 128) {
    auto message = ErrorTextAlgorithm::gcmTag();

    return err(SubtleCallCheckerGcmTagLengthError{
        .code = SubtleCallCheckerGcmTagLengthErrorCode::BAD_TAG,
        .message = message,
    });
  }

  return ok(bits);
}

const SubtleAlgorithmInfo *
SubtleCallChecker::literalAlgorithm(const Expr *arg, SubtleOperation operation,
                                    std::optional<SubtleOperation> fallback, bool &bad) {
  const auto *literal = dyn_cast_or_null<ObjectLitExpr>(arg);
  const auto *nameExpr = this->algorithmNameExpr(arg, literal);
  if (nameExpr == nullptr) {
    return nullptr;
  }
  auto written = SubtleAlgorithms::stringLiteral(nameExpr);
  if (!written.has_value()) {
    return nullptr;
  }
  auto known = this->checkKnownAlgorithm(*written, operation, fallback);
  if (known.isErr()) {
    bad = true;
    this->state.getDiagnostics().error(nameExpr->loc(), known.error().message);
    return nullptr;
  }
  const SubtleAlgorithmInfo *algorithm = known.value();
  auto dictionary = SubtleAlgorithms::dictionaryOf(operation, *algorithm);
  if (literal == nullptr) {
    auto object = this->checkParamsAsObject(*algorithm, operation, dictionary);
    if (object.isErr()) {
      bad = true;
      this->state.getDiagnostics().error(nameExpr->loc(), object.error().message);
    }
    return bad ? nullptr : algorithm;
  }
  auto present = this->checkMembersPresent(*algorithm, operation, dictionary, literal);
  if (present.isErr()) {
    bad = true;
    this->state.getDiagnostics().error(literal->loc(), present.error().message);
    return nullptr;
  }
  bool isReported = this->reportBadHash(literal, dictionary.name) ||
                    this->reportBadCurve(literal) ||
                    this->reportBadAesLength(literal, *algorithm, operation) ||
                    this->reportBadGcmTag(literal, *algorithm);
  if (isReported) {
    bad = true;
    return nullptr;
  }

  return algorithm;
}

const Expr *SubtleCallChecker::algorithmNameExpr(const Expr *arg,
                                                 const ObjectLitExpr *literal) const {
  if (literal == nullptr) {
    return arg;
  }
  if (literal->hasSpread()) {
    return nullptr;
  }
  const auto *name = this->memberProperty(literal, SubtleMember::NAME);
  if (name == nullptr) {
    return nullptr;
  }

  return name->value;
}

bool SubtleCallChecker::reportBadHash(const ObjectLitExpr *literal, llvm::StringRef dictionary) {
  const auto *hash = this->memberProperty(literal, SubtleMember::HASH);
  if (hash == nullptr) {
    return false;
  }
  const Expr *value = hash->value;
  auto *inner = dyn_cast<ObjectLitExpr>(value);
  if (inner != nullptr) {
    const auto *innerName = this->memberProperty(inner, SubtleMember::NAME);
    if (innerName != nullptr) {
      value = innerName->value;
    }
  }
  auto written = SubtleAlgorithms::stringLiteral(value);
  if (!written.has_value()) {
    return false;
  }
  auto sha = this->checkKnownShaHash(*written, dictionary);
  if (sha.isErr()) {
    this->state.getDiagnostics().error(value->loc(), sha.error().message);
    return true;
  }

  return false;
}

bool SubtleCallChecker::reportBadCurve(const ObjectLitExpr *literal) {
  const auto *curve = this->memberProperty(literal, SubtleMember::NAMED_CURVE);
  if (curve == nullptr) {
    return false;
  }
  auto written = SubtleAlgorithms::stringLiteral(curve->value);
  if (!written.has_value()) {
    return false;
  }
  auto named = this->checkKnownCurve(*written);
  if (named.isErr()) {
    this->state.getDiagnostics().error(curve->value->loc(), named.error().message);
    return true;
  }

  return false;
}

bool SubtleCallChecker::reportBadAesLength(const ObjectLitExpr *literal,
                                           const SubtleAlgorithmInfo &algorithm,
                                           SubtleOperation operation) {
  bool isSizing =
      operation == SubtleOperation::GENERATE_KEY || operation == SubtleOperation::KEY_LENGTH;
  bool isSized = algorithm.family == SubtleFamily::AES && isSizing;
  const ObjectLitExpr::Property *length =
      isSized ? this->memberProperty(literal, SubtleMember::LENGTH) : nullptr;
  if (length == nullptr) {
    return false;
  }
  auto bits = this->numberLiteral(length->value);
  if (!bits.has_value()) {
    return false;
  }
  auto valid = this->checkAesLength(*bits);
  if (valid.isErr()) {
    this->state.getDiagnostics().error(length->value->loc(), valid.error().message);
    return true;
  }

  return false;
}

bool SubtleCallChecker::reportBadGcmTag(const ObjectLitExpr *literal,
                                        const SubtleAlgorithmInfo &algorithm) {
  bool isGcm = algorithm.algorithm == SubtleAlgorithm::AES_GCM;
  const ObjectLitExpr::Property *tag =
      isGcm ? this->memberProperty(literal, SubtleMember::TAG_LENGTH) : nullptr;
  if (tag == nullptr) {
    return false;
  }
  auto bits = this->numberLiteral(tag->value);
  if (!bits.has_value()) {
    return false;
  }
  auto valid = this->checkGcmTagLength(*bits);
  if (valid.isErr()) {
    this->state.getDiagnostics().error(tag->value->loc(), valid.error().message);
    return true;
  }

  return false;
}

Expr *SubtleCallChecker::checkSubtleCall(CallExpr *call, MemberExpr *member, const Type *expected) {
  if (call->hasSpread()) {
    return nullptr;
  }
  auto method = SubtleMethodTable::find(member->name);
  if (!method.has_value()) {
    return nullptr;
  }

  SubtleCall subtle{
      .call = call,
      .member = member,
      .method = *method,
      .checkedArgs = std::vector<bool>(call->args.size(), false),
  };
  this->walkSubtleArguments(subtle);
  if (subtle.bad) {
    return this->checkRejectedSubtleCall(subtle);
  }

  return this->finishSubtleCall(subtle, expected);
}

void SubtleCallChecker::walkSubtleArguments(SubtleCall &subtle) {
  switch (subtle.method) {
    case SubtleMethod::DIGEST:
      return this->walkAlgorithmOnly(subtle, SubtleOperation::DIGEST);
    case SubtleMethod::GENERATE_KEY:
      return this->walkGenerateKey(subtle);
    case SubtleMethod::IMPORT_KEY:
      return this->walkImportKey(subtle);
    case SubtleMethod::EXPORT_KEY:
      return this->walkExportKey(subtle);
    case SubtleMethod::SIGN:
      return this->walkAlgorithmOnly(subtle, SubtleOperation::SIGN);
    case SubtleMethod::VERIFY:
      return this->walkAlgorithmOnly(subtle, SubtleOperation::VERIFY);
    case SubtleMethod::ENCRYPT:
      return this->walkAlgorithmOnly(subtle, SubtleOperation::ENCRYPT);
    case SubtleMethod::DECRYPT:
      return this->walkAlgorithmOnly(subtle, SubtleOperation::DECRYPT);
    case SubtleMethod::DERIVE_BITS:
      return this->walkAlgorithmOnly(subtle, SubtleOperation::DERIVE_BITS);
    case SubtleMethod::DERIVE_KEY:
      return this->walkDeriveKey(subtle);
    case SubtleMethod::WRAP_KEY:
      return this->walkWrapKey(subtle);
    case SubtleMethod::UNWRAP_KEY:
      return this->walkUnwrapKey(subtle);
  }
}

Expr *SubtleCallChecker::argAt(const CallExpr *call, size_t index) const {
  return index < call->args.size() ? call->args[index] : nullptr;
}

const Expr *SubtleCallChecker::algorithmAt(const CallExpr *call, size_t index) const {
  auto *arg = this->argAt(call, index);
  auto *name = dyn_cast_or_null<NameExpr>(arg);
  if (name == nullptr) {
    return arg;
  }
  VarSymbol *variable = this->scopes->lookup(name->name).var();
  if (variable == nullptr) {
    return arg;
  }
  const auto *literal = this->state.getConstantStore().findConstLiteral(variable);

  return literal != nullptr ? literal : arg;
}

void SubtleCallChecker::walkAlgorithmOnly(SubtleCall &subtle, SubtleOperation operation) {
  this->algorithmNameAt(subtle, 0, operation, std::nullopt);
}

const SubtleAlgorithmInfo *
SubtleCallChecker::algorithmNameAt(SubtleCall &subtle, size_t index, SubtleOperation operation,
                                   std::optional<SubtleOperation> fallback) {
  const auto *algorithm = this->algorithmAt(subtle.call, index);

  return this->literalAlgorithm(algorithm, operation, fallback, subtle.bad);
}

std::optional<SubtleKeyFormat> SubtleCallChecker::keyFormatAt(SubtleCall &subtle, size_t index) {
  auto *given = this->argAt(subtle.call, index);
  auto written = SubtleAlgorithms::stringLiteral(given);
  if (!written.has_value()) {
    return std::nullopt;
  }
  auto known = this->checkKnownFormat(*written);
  if (known.isErr()) {
    subtle.bad = true;
    this->state.getDiagnostics().error(given->loc(), known.error().message);
    return std::nullopt;
  }

  return known.value();
}

bool SubtleCallChecker::isSecretAlgorithm(const SubtleAlgorithmInfo *algorithm) const {
  return algorithm != nullptr && SubtleFamilyTable::find(algorithm->family).isSecret;
}

void SubtleCallChecker::walkGenerateKey(SubtleCall &subtle) {
  const auto *algorithm =
      this->algorithmNameAt(subtle, 0, SubtleOperation::GENERATE_KEY, std::nullopt);
  int isPublic = this->isSecretAlgorithm(algorithm) ? 0 : -1;
  this->walkUsages(subtle, 2, algorithm, isPublic, true);
  if (algorithm != nullptr) {
    subtle.target = SubtleFamilyTable::find(algorithm->family).generate;
  }
}

void SubtleCallChecker::walkImportKey(SubtleCall &subtle) {
  auto format = this->keyFormatAt(subtle, 0);
  const auto *algorithm =
      this->algorithmNameAt(subtle, 2, SubtleOperation::IMPORT_KEY, std::nullopt);
  this->walkImportFormat(subtle, format, algorithm, false);
  int isPublic = this->importedPublicness(format, algorithm);
  this->walkUsages(subtle, 4, algorithm, isPublic, isPublic == 0);
  subtle.keyFormat = format;
  if (algorithm != nullptr) {
    subtle.target = SubtleFamilyTable::find(algorithm->family).import;
  }
  bool isJwkText = !format.has_value() || format == SubtleKeyFormat::JWK;
  auto &declarations = this->state.getLibraryDeclarations();
  bool isWrappingJson = isJwkText && this->isJsonValue(this->argAt(subtle.call, 1)) &&
                        isa<NameExpr>(subtle.member->object) &&
                        declarations.findLibraryFunction(LibraryFunction::SUBTLE_JWK_OF) != nullptr;
  if (isWrappingJson) {
    this->wrapJwkArgument(subtle);
  }
}

void SubtleCallChecker::walkImportFormat(SubtleCall &subtle, std::optional<SubtleKeyFormat> format,
                                         const SubtleAlgorithmInfo *algorithm, bool unwraps) {
  if (!format.has_value() || algorithm == nullptr) {
    return;
  }
  const std::vector<SubtleKeyFormat> &formats = SubtleFamilyTable::find(algorithm->family).formats;
  if (llvm::is_contained(formats, *format)) {
    return;
  }
  subtle.bad = true;
  auto formatName = SubtleKeyFormatTable::getName(*format);
  auto names = SubtleAlgorithms::formatNames(formats);
  std::string message = unwraps ? ErrorTextSubtleImport::unwrap(algorithm->name, formatName, names)
                                : ErrorTextSubtleImport::format(algorithm->name, formatName, names);
  this->state.getDiagnostics().error(subtle.call->args[0]->loc(), message);
}

int SubtleCallChecker::importedPublicness(std::optional<SubtleKeyFormat> format,
                                          const SubtleAlgorithmInfo *algorithm) const {
  if (this->isSecretAlgorithm(algorithm)) {
    return 0;
  }
  if (format == SubtleKeyFormat::SPKI || format == SubtleKeyFormat::RAW) {
    return 1;
  }

  return format == SubtleKeyFormat::PKCS8 ? 0 : -1;
}

void SubtleCallChecker::wrapJwkArgument(SubtleCall &subtle) {
  std::vector<Expr *> &args = subtle.call->args;
  auto *json = this->exprChecker->checkExpr(args[1]);
  auto jwkOf = LibraryFunctionTable::getName(LibraryFunction::SUBTLE_JWK_OF);
  auto *callee = this->state.getAst().make<NameExpr>(json->loc(), jwkOf);
  auto *wrap = this->state.getAst().make<CallExpr>(json->loc(), callee, std::vector<Expr *>{json});
  args[1] = this->eventTargetChecker->callLibraryFunction(wrap, jwkOf, {}, {true}, json->loc());
  subtle.checkedArgs[1] = true;
  if (!subtle.target.has_value()) {
    subtle.target = LibraryFunction::SUBTLE_IMPORT_ANY;
  }
}

void SubtleCallChecker::walkExportKey(SubtleCall &subtle) {
  auto format = this->keyFormatAt(subtle, 0);
  if (format.has_value()) {
    subtle.target = format == SubtleKeyFormat::JWK ? LibraryFunction::SUBTLE_EXPORT_JWK
                                                   : LibraryFunction::SUBTLE_EXPORT_BYTES;
  }
}

void SubtleCallChecker::walkDeriveKey(SubtleCall &subtle) {
  this->algorithmNameAt(subtle, 0, SubtleOperation::DERIVE_BITS, std::nullopt);
  const auto *algorithm =
      this->algorithmNameAt(subtle, 2, SubtleOperation::IMPORT_KEY, std::nullopt);
  if (algorithm == nullptr) {
    return;
  }
  if (!this->isSecretAlgorithm(algorithm)) {
    subtle.bad = true;
    auto message = ErrorTextSubtleImport::derived(algorithm->name);
    this->state.getDiagnostics().error(this->argAt(subtle.call, 2)->loc(), message);
    return;
  }
  if (algorithm->family != SubtleFamily::KDF) {
    this->algorithmNameAt(subtle, 2, SubtleOperation::KEY_LENGTH, std::nullopt);
  }

  return this->walkUsages(subtle, 4, algorithm, 0, true);
}

void SubtleCallChecker::walkWrapKey(SubtleCall &subtle) {
  this->keyFormatAt(subtle, 0);
  this->algorithmNameAt(subtle, 3, SubtleOperation::WRAP_KEY, SubtleOperation::ENCRYPT);
}

void SubtleCallChecker::walkUnwrapKey(SubtleCall &subtle) {
  auto format = this->keyFormatAt(subtle, 0);
  this->algorithmNameAt(subtle, 3, SubtleOperation::UNWRAP_KEY, SubtleOperation::DECRYPT);
  const auto *algorithm =
      this->algorithmNameAt(subtle, 4, SubtleOperation::IMPORT_KEY, std::nullopt);
  this->walkImportFormat(subtle, format, algorithm, true);
  int isPublic = this->importedPublicness(format, algorithm);
  this->walkUsages(subtle, 6, algorithm, isPublic, isPublic == 0);
  if (algorithm != nullptr) {
    subtle.target = SubtleFamilyTable::find(algorithm->family).unwrap;
  }
}

void SubtleCallChecker::walkUsages(SubtleCall &subtle, size_t index,
                                   const SubtleAlgorithmInfo *algorithm, int isPublic,
                                   bool creates) {
  auto *literal = dyn_cast_or_null<ArrayLitExpr>(this->argAt(subtle.call, index));
  if (literal == nullptr) {
    return;
  }
  auto allowed = this->allowedUsages(algorithm, isPublic);
  for (const ArrayLitExpr::Element &element : literal->elements) {
    std::optional<std::string> written =
        element.spread ? std::nullopt : SubtleAlgorithms::stringLiteral(element.expr);
    if (!written.has_value()) {
      return;
    }
    auto usage = SubtleKeyUsageTable::find(*written);
    if (!usage.has_value()) {
      subtle.bad = true;
      auto message = ErrorTextSubtleUsage::unknown(*written);
      this->state.getDiagnostics().error(element.expr->loc(), message);
      return;
    }
    if (algorithm != nullptr && !llvm::is_contained(allowed, *usage)) {
      subtle.bad = true;
      auto names = SubtleAlgorithms::usageNames(allowed);
      bool isSecret = this->isSecretAlgorithm(algorithm);
      auto message =
          ErrorTextSubtleUsage::notOf(*written, algorithm->name, isPublic, isSecret, names);
      this->state.getDiagnostics().error(element.expr->loc(), message);
      return;
    }
  }
  bool isOwnNeeded = creates && algorithm != nullptr && isPublic != 1;
  if (isOwnNeeded) {
    this->walkOwnUsage(subtle, literal, *algorithm, isPublic);
  }
}

std::vector<SubtleKeyUsage> SubtleCallChecker::allowedUsages(const SubtleAlgorithmInfo *algorithm,
                                                             int isPublic) const {
  auto allowed = std::vector<SubtleKeyUsage>();
  if (algorithm == nullptr) {
    return allowed;
  }
  if (isPublic != 0) {
    allowed = algorithm->publicUsages;
  }
  if (isPublic != 1) {
    for (SubtleKeyUsage usage : algorithm->privateUsages) {
      if (!llvm::is_contained(allowed, usage)) {
        allowed.push_back(usage);
      }
    }
  }

  return allowed;
}

void SubtleCallChecker::walkOwnUsage(SubtleCall &subtle, const ArrayLitExpr *literal,
                                     const SubtleAlgorithmInfo &algorithm, int isPublic) {
  const std::vector<SubtleKeyUsage> &own = algorithm.privateUsages;
  bool hasAny = false;
  for (const ArrayLitExpr::Element &element : literal->elements) {
    auto written = SubtleAlgorithms::stringLiteral(element.expr);
    auto usage = SubtleKeyUsageTable::find(*written);
    hasAny |= usage.has_value() && llvm::is_contained(own, *usage);
  }
  if (hasAny) {
    return;
  }
  subtle.bad = true;
  bool isSingle = literal->elements.empty() || isPublic == 0;
  auto names = SubtleAlgorithms::usageNames(own);
  std::string message =
      isSingle ? ErrorTextSubtleUsage::empty(names) : ErrorTextSubtleUsage::pairEmpty(names);
  this->state.getDiagnostics().error(literal->loc(), message);
}

Expr *SubtleCallChecker::checkRejectedSubtleCall(SubtleCall &subtle) {
  CallExpr *call = subtle.call;
  std::vector<Expr *> &args = call->args;
  call->type = Type::error();
  const auto *subtleClass =
      this->state.getLibraryDeclarations().findLibraryClass(LibraryClass::SUBTLE_CRYPTO);
  auto methods = this->classDeclarer->methodsNamed(subtleClass, subtle.member->name);
  FuncSymbol *function = methods.size() == 1 ? methods[0]->sym : nullptr;
  for (size_t index = 0; index < args.size(); ++index) {
    if (!subtle.checkedArgs[index]) {
      bool isTyped = function != nullptr && index < function->params.size();
      args[index] = isTyped ? this->argumentChecker->checkArg(args[index], function->params[index])
                            : this->exprChecker->checkExpr(args[index]);
    }
  }

  return call;
}

Expr *SubtleCallChecker::finishSubtleCall(SubtleCall &subtle, const Type *expected) {
  MemberExpr *member = subtle.member;
  auto &declarations = this->state.getLibraryDeclarations();
  bool isLibrary = subtle.target.has_value() &&
                   declarations.findLibraryFunction(*subtle.target) != nullptr &&
                   isa<NameExpr>(member->object);
  Expr *checked = nullptr;
  if (isLibrary) {
    auto targetName = LibraryFunctionTable::getName(*subtle.target);
    checked = this->eventTargetChecker->callLibraryFunction(subtle.call, targetName, {},
                                                            subtle.checkedArgs, member->loc());
  } else {
    checked = this->methodCallChecker->checkMethodCall(subtle.call, member, expected);
  }
  if (!this->matchKeyData(subtle)) {
    checked->type = Type::error();
  }

  return checked;
}

bool SubtleCallChecker::matchKeyData(const SubtleCall &subtle) {
  auto *data = this->argAt(subtle.call, 1);
  bool isUnknown = data == nullptr || !subtle.keyFormat.has_value() || data->type == nullptr ||
                   data->type->isError();
  if (isUnknown) {
    return true;
  }
  const auto *bare = this->withoutImplicitCasts(data);
  const Type *type = bare->type;
  if (type == nullptr || type->isError()) {
    return true;
  }
  bool isJwkFormat = subtle.keyFormat == SubtleKeyFormat::JWK;
  if (isJwkFormat && this->isHoldingBytes(type)) {
    auto message = ErrorTextSubtleKeyData::jwk(type->name());
    this->state.getDiagnostics().error(bare->loc(), message);
    return false;
  }
  if (!isJwkFormat && this->isJwkObject(type)) {
    auto formatName = SubtleKeyFormatTable::getName(*subtle.keyFormat);
    auto message = ErrorTextSubtleKeyData::bytes(formatName, type->name());
    this->state.getDiagnostics().error(bare->loc(), message);
    return false;
  }

  return true;
}

const Expr *SubtleCallChecker::withoutImplicitCasts(const Expr *expr) const {
  const Expr *bare = expr;
  auto *castExpr = dyn_cast<CastExpr>(bare);
  while (castExpr != nullptr && castExpr->isImplicit()) {
    bare = castExpr->operand;
    castExpr = dyn_cast<CastExpr>(bare);
  }

  return bare;
}

bool SubtleCallChecker::isHoldingBytes(const Type *type) const {
  if (type->isBytes()) {
    return true;
  }
  if (!type->isUnion()) {
    return false;
  }
  for (const Type *member : type->members()) {
    if (!member->isBytes()) {
      return false;
    }
  }

  return true;
}

bool SubtleCallChecker::isJwkObject(const Type *type) const {
  return !type->isBytes() && type->isObject() && this->classDeclarer->classOf(type) == nullptr &&
         !type->isArray() && !type->isTuple();
}

bool SubtleCallChecker::isJsonValue(const Expr *expr) const {
  const Expr *root = expr;
  const auto *inner = this->accessInner(root);
  while (inner != nullptr) {
    root = inner;
    inner = this->accessInner(root);
  }
  auto *name = dyn_cast_or_null<NameExpr>(root);
  if (name != nullptr) {
    VarSymbol *variable = this->scopes->lookup(name->name).var();
    return variable != nullptr && variable->type != nullptr &&
           variable->type->nonOptional()->isJson();
  }
  auto *call = dyn_cast_or_null<CallExpr>(root);
  auto *member = call != nullptr ? dyn_cast<MemberExpr>(call->callee) : nullptr;
  if (member == nullptr) {
    return false;
  }

  return JsonStaticTable::find(member->name) == JsonStatic::PARSE && member->typeArgs.empty() &&
         this->callChecker->isGlobalName(member->object, GlobalName::JSON);
}

const Expr *SubtleCallChecker::accessInner(const Expr *expr) const {
  auto *member = dyn_cast_or_null<MemberExpr>(expr);
  if (member != nullptr) {
    return member->object;
  }
  auto *index = dyn_cast_or_null<IndexExpr>(expr);
  if (index != nullptr) {
    return index->object;
  }

  return nullptr;
}

}
