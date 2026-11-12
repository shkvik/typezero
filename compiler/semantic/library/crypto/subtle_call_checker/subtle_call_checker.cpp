#include "compiler/semantic/library/crypto/subtle_call_checker/subtle_call_checker.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/crypto/crypto_api/crypto_api.h"
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
SubtleCallChecker::knownFormat(llvm::StringRef format) const {
  static const llvm::StringLiteral formats[] = {"raw", "pkcs8", "spki", "jwk"};
  if (!llvm::is_contained(formats, format)) {
    auto message = ErrorTextKeyFormat::unknown(format);

    return err(SubtleCallCheckerKnownFormatError{
        .code = SubtleCallCheckerKnownFormatErrorCode::UNKNOWN_FORMAT,
        .message = message,
    });
  }

  return ok(format);
}

std::optional<double> SubtleCallChecker::numberLiteral(const Expr *expr) const {
  if (auto *literal = dyn_cast_or_null<IntLitExpr>(expr)) {
    return double(literal->value);
  }
  if (auto *literal = dyn_cast_or_null<FloatLitExpr>(expr)) {
    return literal->value;
  }

  return std::nullopt;
}

const ObjectLitExpr::Property *SubtleCallChecker::property(const ObjectLitExpr *literal,
                                                           llvm::StringRef name) const {
  for (const ObjectLitExpr::Property &property : literal->props) {
    if (!property.spread() && !property.key() && property.name == name) {
      return &property;
    }
  }

  return nullptr;
}

std::string SubtleCallChecker::algorithmIn(llvm::StringRef op, llvm::StringRef upper) const {
  for (llvm::StringRef known : SubtleAlgorithms::algorithmsOf(op)) {
    if (known.upper() == upper) {
      return known.str();
    }
  }

  return "";
}

SubtleCallChecker::ResultAlgorithmKnown
SubtleCallChecker::knownAlgorithm(llvm::StringRef written, llvm::StringRef op,
                                  llvm::StringRef fallback) const {
  std::string upper = written.upper();
  auto name = this->algorithmIn(op, upper);
  if (name.empty() && !fallback.empty()) {
    name = this->algorithmIn(fallback, upper);
  }
  if (!name.empty()) {
    return ok(name);
  }
  auto list = SubtleAlgorithms::quotedList(SubtleAlgorithms::algorithmsOf(op), "or");
  auto verb = SubtleAlgorithms::verbOf(op);
  if (SubtleAlgorithms::experimentalAlgorithm(upper)) {
    auto message = ErrorTextAlgorithm::experiment(written, verb, list);

    return err(SubtleCallCheckerKnownAlgorithmError{
        .code = SubtleCallCheckerKnownAlgorithmErrorCode::EXPERIMENTAL,
        .message = message,
    });
  }
  bool isKnown = false;
  for (llvm::StringRef other :
       {"digest", "generateKey", "importKey", "encrypt", "sign", "deriveBits", "wrapKey"}) {
    for (llvm::StringRef a : SubtleAlgorithms::algorithmsOf(other)) {
      isKnown |= a.upper() == upper;
    }
  }
  if (isKnown) {
    auto message = ErrorTextAlgorithm::otherOp(written, op, verb, list);

    return err(SubtleCallCheckerKnownAlgorithmError{
        .code = SubtleCallCheckerKnownAlgorithmErrorCode::OTHER_OPERATION,
        .message = message,
    });
  }
  auto message = ErrorTextAlgorithm::unknown(written, op, verb, list);

  return err(SubtleCallCheckerKnownAlgorithmError{
      .code = SubtleCallCheckerKnownAlgorithmErrorCode::UNKNOWN_ALGORITHM,
      .message = message,
  });
}

SubtleCallChecker::ResultParamsAsObject
SubtleCallChecker::paramsAsObject(llvm::StringRef name, llvm::StringRef op,
                                  const Dictionary &dictionary) const {
  if (!dictionary.required.empty()) {
    llvm::StringRef member = dictionary.required[0];
    auto example = SubtleAlgorithms::exampleOf(member);
    auto message = ErrorTextAlgorithm::object(name, op, dictionary.name, member, example);

    return err(SubtleCallCheckerParamsAsObjectError{
        .code = SubtleCallCheckerParamsAsObjectErrorCode::NEEDS_OBJECT,
        .message = message,
    });
  }

  return ok(name);
}

SubtleCallChecker::ResultMembersPresent
SubtleCallChecker::membersPresent(llvm::StringRef name, llvm::StringRef op,
                                  const Dictionary &dictionary,
                                  const ObjectLitExpr *literal) const {
  for (llvm::StringRef member : dictionary.required) {
    if (this->property(literal, member) == nullptr) {
      auto example = SubtleAlgorithms::exampleOf(member);
      auto message = ErrorTextAlgorithm::needs(name, op, member, dictionary.name, example);

      return err(SubtleCallCheckerMembersPresentError{
          .code = SubtleCallCheckerMembersPresentErrorCode::MISSING_MEMBER,
          .message = message,
      });
    }
  }

  return ok(literal);
}

SubtleCallChecker::ResultShaHashIsKnown
SubtleCallChecker::knownShaHash(llvm::StringRef hash, llvm::StringRef dictionary) const {
  bool isSha = llvm::StringRef(hash.lower()).starts_with("sha");
  bool isKnownHash = false;
  for (llvm::StringRef a : SubtleAlgorithms::algorithmsOf("digest")) {
    isKnownHash |= a.upper() == hash.upper();
  }
  if (isSha && isKnownHash) {
    return ok(hash);
  }
  if (isSha || SubtleAlgorithms::experimentalAlgorithm(hash.upper())) {
    auto list = SubtleAlgorithms::quotedList(SubtleAlgorithms::algorithmsOf("digest"), "or");
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

SubtleCallChecker::ResultCurveNameKnown SubtleCallChecker::knownCurve(llvm::StringRef curve) const {
  if (curve != "P-256" && curve != "P-384" && curve != "P-521") {
    auto message = ErrorTextAlgorithm::curve(curve);

    return err(SubtleCallCheckerKnownCurveError{
        .code = SubtleCallCheckerKnownCurveErrorCode::UNKNOWN_CURVE,
        .message = message,
    });
  }

  return ok(curve);
}

SubtleCallChecker::ResultAesLengthValid SubtleCallChecker::aesLength(double bits) const {
  if (bits != 128 && bits != 192 && bits != 256) {
    auto message = ErrorTextAlgorithm::aesLength();

    return err(SubtleCallCheckerAesLengthError{
        .code = SubtleCallCheckerAesLengthErrorCode::BAD_LENGTH,
        .message = message,
    });
  }

  return ok(bits);
}

SubtleCallChecker::ResultGcmTagLengthOk SubtleCallChecker::gcmTagLength(double bits) const {
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

std::string SubtleCallChecker::literalAlgorithm(const Expr *arg, llvm::StringRef op,
                                                llvm::StringRef fallback, bool &bad) {
  const auto *literal = dyn_cast_or_null<ObjectLitExpr>(arg);
  const auto *nameExpr = this->algorithmNameExpr(arg, literal);
  if (nameExpr == nullptr) {
    return "";
  }
  auto written = SubtleAlgorithms::stringLiteral(nameExpr);
  if (!written.has_value()) {
    return "";
  }
  auto known = this->knownAlgorithm(*written, op, fallback);
  if (known.isErr()) {
    bad = true;
    this->state.getDiagnostics().error(nameExpr->loc(), known.error().message);
    return "";
  }
  std::string name = known.value();
  auto dictionary = SubtleAlgorithms::dictionaryOf(op, name);
  if (literal == nullptr) {
    auto object = this->paramsAsObject(name, op, dictionary);
    if (object.isErr()) {
      bad = true;
      this->state.getDiagnostics().error(nameExpr->loc(), object.error().message);
    }
    return bad ? "" : name;
  }
  auto present = this->membersPresent(name, op, dictionary, literal);
  if (present.isErr()) {
    bad = true;
    this->state.getDiagnostics().error(literal->loc(), present.error().message);
    return "";
  }
  bool isReported =
      this->reportsBadHash(literal, dictionary.name) || this->reportsBadCurve(literal) ||
      this->reportsBadAesLength(literal, name, op) || this->reportsBadGcmTag(literal, name);
  if (isReported) {
    bad = true;
    return "";
  }

  return name;
}

const Expr *SubtleCallChecker::algorithmNameExpr(const Expr *arg,
                                                 const ObjectLitExpr *literal) const {
  if (literal == nullptr) {
    return arg;
  }
  if (literal->hasSpread()) {
    return nullptr;
  }
  const auto *name = this->property(literal, "name");
  if (name == nullptr) {
    return nullptr;
  }

  return name->value;
}

bool SubtleCallChecker::reportsBadHash(const ObjectLitExpr *literal, llvm::StringRef dictionary) {
  const auto *hash = this->property(literal, "hash");
  if (hash == nullptr) {
    return false;
  }
  const Expr *value = hash->value;
  auto *inner = dyn_cast<ObjectLitExpr>(value);
  if (inner != nullptr) {
    const auto *innerName = this->property(inner, "name");
    if (innerName != nullptr) {
      value = innerName->value;
    }
  }
  auto written = SubtleAlgorithms::stringLiteral(value);
  if (!written.has_value()) {
    return false;
  }
  auto sha = this->knownShaHash(*written, dictionary);
  if (sha.isErr()) {
    this->state.getDiagnostics().error(value->loc(), sha.error().message);
    return true;
  }

  return false;
}

bool SubtleCallChecker::reportsBadCurve(const ObjectLitExpr *literal) {
  const auto *curve = this->property(literal, "namedCurve");
  if (curve == nullptr) {
    return false;
  }
  auto written = SubtleAlgorithms::stringLiteral(curve->value);
  if (!written.has_value()) {
    return false;
  }
  auto named = this->knownCurve(*written);
  if (named.isErr()) {
    this->state.getDiagnostics().error(curve->value->loc(), named.error().message);
    return true;
  }

  return false;
}

bool SubtleCallChecker::reportsBadAesLength(const ObjectLitExpr *literal, llvm::StringRef name,
                                            llvm::StringRef op) {
  bool isSized = name.starts_with("AES-") && (op == "generateKey" || op == "get key length");
  const ObjectLitExpr::Property *length = isSized ? this->property(literal, "length") : nullptr;
  if (length == nullptr) {
    return false;
  }
  auto bits = this->numberLiteral(length->value);
  if (!bits.has_value()) {
    return false;
  }
  auto valid = this->aesLength(*bits);
  if (valid.isErr()) {
    this->state.getDiagnostics().error(length->value->loc(), valid.error().message);
    return true;
  }

  return false;
}

bool SubtleCallChecker::reportsBadGcmTag(const ObjectLitExpr *literal, llvm::StringRef name) {
  const ObjectLitExpr::Property *tag =
      name == "AES-GCM" ? this->property(literal, "tagLength") : nullptr;
  if (tag == nullptr) {
    return false;
  }
  auto bits = this->numberLiteral(tag->value);
  if (!bits.has_value()) {
    return false;
  }
  auto valid = this->gcmTagLength(*bits);
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
      return this->walkAlgorithmOnly(subtle, "digest");
    case SubtleMethod::GENERATE_KEY:
      return this->walkGenerateKey(subtle);
    case SubtleMethod::IMPORT_KEY:
      return this->walkImportKey(subtle);
    case SubtleMethod::EXPORT_KEY:
      return this->walkExportKey(subtle);
    case SubtleMethod::SIGN:
    case SubtleMethod::VERIFY:
    case SubtleMethod::ENCRYPT:
    case SubtleMethod::DECRYPT:
    case SubtleMethod::DERIVE_BITS:
      return this->walkAlgorithmOnly(subtle, subtle.member->name);
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

void SubtleCallChecker::walkAlgorithmOnly(SubtleCall &subtle, llvm::StringRef op) {
  this->algorithmNameAt(subtle, 0, op, "");
}

std::string SubtleCallChecker::algorithmNameAt(SubtleCall &subtle, size_t index, llvm::StringRef op,
                                               llvm::StringRef fallback) {
  const auto *algorithm = this->algorithmAt(subtle.call, index);

  return this->literalAlgorithm(algorithm, op, fallback, subtle.bad);
}

std::string SubtleCallChecker::keyFormatAt(SubtleCall &subtle, size_t index) {
  auto *given = this->argAt(subtle.call, index);
  auto written = SubtleAlgorithms::stringLiteral(given);
  if (!written.has_value()) {
    return "";
  }
  auto known = this->knownFormat(*written);
  if (known.isErr()) {
    subtle.bad = true;
    this->state.getDiagnostics().error(given->loc(), known.error().message);
    return "";
  }

  return *written;
}

void SubtleCallChecker::walkGenerateKey(SubtleCall &subtle) {
  auto name = this->algorithmNameAt(subtle, 0, "generateKey", "");
  auto family = SubtleAlgorithms::familyOf(name);
  int isPublic = family == "Hmac" || family == "Aes" ? 0 : -1;
  this->walkUsages(subtle, 2, name, isPublic, true);
  if (!name.empty()) {
    subtle.target = "subtleGenerate" + family.str();
  }
}

void SubtleCallChecker::walkImportKey(SubtleCall &subtle) {
  auto format = this->keyFormatAt(subtle, 0);
  auto name = this->algorithmNameAt(subtle, 2, "importKey", "");
  this->walkImportFormat(subtle, format, name, false);
  int isPublic = this->importedPublicness(format, name);
  this->walkUsages(subtle, 4, name, isPublic, isPublic == 0);
  subtle.keyFormat = format;
  if (!name.empty()) {
    subtle.target = "subtleImport" + SubtleAlgorithms::familyOf(name).str();
  }
  bool isJwkText = format.empty() || format == "jwk";
  bool isWrappingJson =
      isJwkText && this->isJsonValue(this->argAt(subtle.call, 1)) &&
      isa<NameExpr>(subtle.member->object) &&
      this->state.getLibraryDeclarations().findLibraryFunction("subtleJwkOf") != nullptr;
  if (isWrappingJson) {
    this->wrapJwkArgument(subtle);
  }
}

void SubtleCallChecker::walkImportFormat(SubtleCall &subtle, const std::string &format,
                                         const std::string &name, bool unwraps) {
  if (format.empty() || name.empty()) {
    return;
  }
  auto formats = SubtleAlgorithms::formatsOf(SubtleAlgorithms::familyOf(name));
  if (llvm::is_contained(formats, format)) {
    return;
  }
  subtle.bad = true;
  std::string message = unwraps ? ErrorTextSubtleImport::unwrap(name, format, formats)
                                : ErrorTextSubtleImport::format(name, format, formats);
  this->state.getDiagnostics().error(subtle.call->args[0]->loc(), message);
}

int SubtleCallChecker::importedPublicness(const std::string &format,
                                          const std::string &name) const {
  auto family = SubtleAlgorithms::familyOf(name);
  if (family == "Hmac" || family == "Aes" || family == "Kdf") {
    return 0;
  }
  if (format == "spki" || format == "raw") {
    return 1;
  }

  return format == "pkcs8" ? 0 : -1;
}

void SubtleCallChecker::wrapJwkArgument(SubtleCall &subtle) {
  std::vector<Expr *> &args = subtle.call->args;
  auto *json = this->exprChecker->checkExpr(args[1]);
  auto *callee = this->state.getAst().make<NameExpr>(json->loc(), "subtleJwkOf");
  auto *wrap = this->state.getAst().make<CallExpr>(json->loc(), callee, std::vector<Expr *>{json});
  args[1] =
      this->eventTargetChecker->callLibraryFunction(wrap, "subtleJwkOf", {}, {true}, json->loc());
  subtle.checkedArgs[1] = true;
  if (subtle.target.empty()) {
    subtle.target = "subtleImportAny";
  }
}

void SubtleCallChecker::walkExportKey(SubtleCall &subtle) {
  auto format = this->keyFormatAt(subtle, 0);
  if (!format.empty()) {
    subtle.target = format == "jwk" ? "subtleExportJwk" : "subtleExportBytes";
  }
}

void SubtleCallChecker::walkDeriveKey(SubtleCall &subtle) {
  this->algorithmNameAt(subtle, 0, "deriveBits", "");
  auto name = this->algorithmNameAt(subtle, 2, "importKey", "");
  if (name.empty()) {
    return;
  }
  auto family = SubtleAlgorithms::familyOf(name);
  if (family != "Hmac" && family != "Aes" && family != "Kdf") {
    subtle.bad = true;
    auto message = ErrorTextSubtleImport::derived(name);
    this->state.getDiagnostics().error(this->argAt(subtle.call, 2)->loc(), message);
    return;
  }
  if (family != "Kdf") {
    this->algorithmNameAt(subtle, 2, "get key length", "");
  }

  return this->walkUsages(subtle, 4, name, 0, true);
}

void SubtleCallChecker::walkWrapKey(SubtleCall &subtle) {
  this->keyFormatAt(subtle, 0);
  this->algorithmNameAt(subtle, 3, "wrapKey", "encrypt");
}

void SubtleCallChecker::walkUnwrapKey(SubtleCall &subtle) {
  auto format = this->keyFormatAt(subtle, 0);
  this->algorithmNameAt(subtle, 3, "unwrapKey", "decrypt");
  auto name = this->algorithmNameAt(subtle, 4, "importKey", "");
  this->walkImportFormat(subtle, format, name, true);
  int isPublic = this->importedPublicness(format, name);
  this->walkUsages(subtle, 6, name, isPublic, isPublic == 0);
  if (!name.empty()) {
    subtle.target = "subtleUnwrap" + SubtleAlgorithms::familyOf(name).str();
  }
}

void SubtleCallChecker::walkUsages(SubtleCall &subtle, size_t index, llvm::StringRef name,
                                   int isPublic, bool creates) {
  auto *literal = dyn_cast_or_null<ArrayLitExpr>(this->argAt(subtle.call, index));
  if (literal == nullptr) {
    return;
  }
  auto allowed = this->allowedUsages(name, isPublic);
  for (const ArrayLitExpr::Element &element : literal->elements) {
    std::optional<std::string> usage =
        element.spread ? std::nullopt : SubtleAlgorithms::stringLiteral(element.expr);
    if (!usage.has_value()) {
      return;
    }
    if (!llvm::is_contained(kUsages, *usage)) {
      subtle.bad = true;
      auto message = ErrorTextSubtleUsage::unknown(*usage);
      this->state.getDiagnostics().error(element.expr->loc(), message);
      return;
    }
    if (!name.empty() && !llvm::is_contained(allowed, *usage)) {
      subtle.bad = true;
      auto message = ErrorTextSubtleUsage::notOf(*usage, name, isPublic, allowed);
      this->state.getDiagnostics().error(element.expr->loc(), message);
      return;
    }
  }
  bool isOwnNeeded = creates && !name.empty() && isPublic != 1;
  if (isOwnNeeded) {
    this->walkOwnUsage(subtle, literal, name, isPublic);
  }
}

std::vector<llvm::StringLiteral> SubtleCallChecker::allowedUsages(llvm::StringRef name,
                                                                  int isPublic) const {
  auto allowed = std::vector<llvm::StringLiteral>();
  if (name.empty()) {
    return allowed;
  }
  if (isPublic != 0) {
    allowed = SubtleAlgorithms::usagesOf(name, true);
  }
  if (isPublic != 1) {
    for (llvm::StringLiteral usage : SubtleAlgorithms::usagesOf(name, false)) {
      if (!llvm::is_contained(allowed, usage)) {
        allowed.push_back(usage);
      }
    }
  }

  return allowed;
}

void SubtleCallChecker::walkOwnUsage(SubtleCall &subtle, const ArrayLitExpr *literal,
                                     llvm::StringRef name, int isPublic) {
  auto own = SubtleAlgorithms::usagesOf(name, false);
  bool hasAny = false;
  for (const ArrayLitExpr::Element &element : literal->elements) {
    hasAny |= llvm::is_contained(own, *SubtleAlgorithms::stringLiteral(element.expr));
  }
  if (hasAny) {
    return;
  }
  subtle.bad = true;
  bool isSingle = literal->elements.empty() || isPublic == 0;
  std::string message =
      isSingle ? ErrorTextSubtleUsage::empty(own) : ErrorTextSubtleUsage::pairEmpty(own);
  this->state.getDiagnostics().error(literal->loc(), message);
}

Expr *SubtleCallChecker::checkRejectedSubtleCall(SubtleCall &subtle) {
  CallExpr *call = subtle.call;
  std::vector<Expr *> &args = call->args;
  call->type = Type::error();
  const auto *subtleClass = this->state.getLibraryDeclarations().findLibraryClass("SubtleCrypto");
  auto methods = this->classDeclarer->methodsNamed(subtleClass, subtle.member->name);
  FuncSymbol *function = methods.size() == 1 ? methods[0]->sym : nullptr;
  for (size_t index = 0; index < args.size(); ++index) {
    if (subtle.checkedArgs[index]) {
      continue;
    }
    bool isTyped = function != nullptr && index < function->params.size();
    args[index] = isTyped ? this->argumentChecker->checkArg(args[index], function->params[index])
                          : this->exprChecker->checkExpr(args[index]);
  }

  return call;
}

Expr *SubtleCallChecker::finishSubtleCall(SubtleCall &subtle, const Type *expected) {
  MemberExpr *member = subtle.member;
  bool isLibrary =
      !subtle.target.empty() &&
      this->state.getLibraryDeclarations().findLibraryFunction(subtle.target) != nullptr &&
      isa<NameExpr>(member->object);
  Expr *checked = nullptr;
  if (isLibrary) {
    checked = this->eventTargetChecker->callLibraryFunction(subtle.call, subtle.target, {},
                                                            subtle.checkedArgs, member->loc());
  } else {
    checked = this->methodCallChecker->checkMethodCall(subtle.call, member, expected);
  }
  if (!this->keyDataFits(subtle)) {
    checked->type = Type::error();
  }

  return checked;
}

bool SubtleCallChecker::keyDataFits(const SubtleCall &subtle) {
  auto *data = this->argAt(subtle.call, 1);
  bool isUnknown =
      data == nullptr || subtle.keyFormat.empty() || data->type == nullptr || data->type->isError();
  if (isUnknown) {
    return true;
  }
  const auto *bare = this->withoutImplicitCasts(data);
  const Type *type = bare->type;
  if (type == nullptr || type->isError()) {
    return true;
  }
  bool isJwkFormat = subtle.keyFormat == "jwk";
  if (isJwkFormat && this->holdsBytes(type)) {
    auto message = ErrorTextSubtleKeyData::jwk(type->name());
    this->state.getDiagnostics().error(bare->loc(), message);
    return false;
  }
  if (!isJwkFormat && this->isJwkObject(type)) {
    auto message = ErrorTextSubtleKeyData::bytes(subtle.keyFormat, type->name());
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

bool SubtleCallChecker::holdsBytes(const Type *type) const {
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

  return member->name == "parse" && member->typeArgs.empty() &&
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
