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
  this->scopes = &all.getScopes();
  this->callChecker = &all.getCallChecker();
  this->exprChecker = &all.getExprChecker();
  this->argumentChecker = &all.getArgumentChecker();
  this->methodCallChecker = &all.getMethodCallChecker();
  this->eventTargetChecker = &all.getEventTargetChecker();
}

SubtleCallChecker::ResultKeyFormatKnown SubtleCallChecker::knownFormat(llvm::StringRef format) {
  static const llvm::StringLiteral formats[] = {"raw", "pkcs8", "spki", "jwk"};
  if (!llvm::is_contained(formats, format)) {
    std::string message = ErrorTextKeyFormat::unknown(format);

    return err(SubtleCallCheckerKnownFormatError{
        .code = SubtleCallCheckerKnownFormatErrorCode::UNKNOWN_FORMAT,
        .message = message,
    });
  }

  return ok(format);
}

std::optional<double> SubtleCallChecker::numberLiteral(const Expr *e) {
  if (auto *lit = dyn_cast_or_null<IntLitExpr>(e)) {
    return double(lit->value);
  }
  if (auto *lit = dyn_cast_or_null<FloatLitExpr>(e)) {
    return lit->value;
  }

  return std::nullopt;
}

const ObjectLitExpr::Property *SubtleCallChecker::property(const ObjectLitExpr *lit,
                                                           llvm::StringRef name) {
  for (const ObjectLitExpr::Property &prop : lit->props) {
    if (!prop.spread() && !prop.key() && prop.name == name) {
      return &prop;
    }
  }

  return nullptr;
}

std::string SubtleCallChecker::algorithmIn(llvm::StringRef op, llvm::StringRef upper) {
  for (llvm::StringRef known : SubtleAlgorithms::algorithmsOf(op)) {
    if (known.upper() == upper) {
      return known.str();
    }
  }

  return "";
}

SubtleCallChecker::ResultAlgorithmKnown
SubtleCallChecker::knownAlgorithm(llvm::StringRef written, llvm::StringRef op,
                                  llvm::StringRef fallback) {
  std::string upper = written.upper();
  std::string name = SubtleCallChecker::algorithmIn(op, upper);
  if (name.empty() && !fallback.empty()) {
    name = SubtleCallChecker::algorithmIn(fallback, upper);
  }
  if (!name.empty()) {
    return ok(name);
  }
  std::string list = SubtleAlgorithms::quotedList(SubtleAlgorithms::algorithmsOf(op), "or");
  std::string verb = SubtleAlgorithms::verbOf(op);
  if (SubtleAlgorithms::experimentalAlgorithm(upper)) {
    std::string message = ErrorTextAlgorithm::experiment(written, verb, list);

    return err(SubtleCallCheckerKnownAlgorithmError{
        .code = SubtleCallCheckerKnownAlgorithmErrorCode::EXPERIMENTAL,
        .message = message,
    });
  }
  bool known = false;
  for (llvm::StringRef other :
       {"digest", "generateKey", "importKey", "encrypt", "sign", "deriveBits", "wrapKey"}) {
    for (llvm::StringRef a : SubtleAlgorithms::algorithmsOf(other)) {
      known |= a.upper() == upper;
    }
  }
  if (known) {
    std::string message = ErrorTextAlgorithm::otherOp(written, op, verb, list);

    return err(SubtleCallCheckerKnownAlgorithmError{
        .code = SubtleCallCheckerKnownAlgorithmErrorCode::OTHER_OPERATION,
        .message = message,
    });
  }
  std::string message = ErrorTextAlgorithm::unknown(written, op, verb, list);

  return err(SubtleCallCheckerKnownAlgorithmError{
      .code = SubtleCallCheckerKnownAlgorithmErrorCode::UNKNOWN_ALGORITHM,
      .message = message,
  });
}

SubtleCallChecker::ResultParamsAsObject
SubtleCallChecker::paramsAsObject(llvm::StringRef name, llvm::StringRef op,
                                  const Dictionary &dictionary) {
  if (!dictionary.required.empty()) {
    llvm::StringRef member = dictionary.required[0];
    std::string example = SubtleAlgorithms::exampleOf(member);
    std::string message = ErrorTextAlgorithm::object(name, op, dictionary.name, member, example);

    return err(SubtleCallCheckerParamsAsObjectError{
        .code = SubtleCallCheckerParamsAsObjectErrorCode::NEEDS_OBJECT,
        .message = message,
    });
  }

  return ok(name);
}

SubtleCallChecker::ResultMembersPresent
SubtleCallChecker::membersPresent(llvm::StringRef name, llvm::StringRef op,
                                  const Dictionary &dictionary, const ObjectLitExpr *lit) {
  for (llvm::StringRef member : dictionary.required) {
    if (!SubtleCallChecker::property(lit, member)) {
      std::string example = SubtleAlgorithms::exampleOf(member);
      std::string message = ErrorTextAlgorithm::needs(name, op, member, dictionary.name, example);

      return err(SubtleCallCheckerMembersPresentError{
          .code = SubtleCallCheckerMembersPresentErrorCode::MISSING_MEMBER,
          .message = message,
      });
    }
  }

  return ok(lit);
}

SubtleCallChecker::ResultShaHashIsKnown
SubtleCallChecker::knownShaHash(llvm::StringRef hash, llvm::StringRef dictionary) {
  bool sha = llvm::StringRef(hash.lower()).starts_with("sha");
  bool knownHash = false;
  for (llvm::StringRef a : SubtleAlgorithms::algorithmsOf("digest")) {
    knownHash |= a.upper() == hash.upper();
  }
  if (sha && knownHash) {
    return ok(hash);
  }
  if (sha || SubtleAlgorithms::experimentalAlgorithm(hash.upper())) {
    std::string list = SubtleAlgorithms::quotedList(SubtleAlgorithms::algorithmsOf("digest"), "or");
    std::string message = ErrorTextAlgorithm::hash(hash, dictionary, list);

    return err(SubtleCallCheckerKnownShaHashError{
        .code = SubtleCallCheckerKnownShaHashErrorCode::UNKNOWN_HASH,
        .message = message,
    });
  }
  std::string message = ErrorTextAlgorithm::notSha(hash, dictionary);

  return err(SubtleCallCheckerKnownShaHashError{
      .code = SubtleCallCheckerKnownShaHashErrorCode::NOT_SHA,
      .message = message,
  });
}

SubtleCallChecker::ResultCurveNameKnown SubtleCallChecker::knownCurve(llvm::StringRef curve) {
  if (curve != "P-256" && curve != "P-384" && curve != "P-521") {
    std::string message = ErrorTextAlgorithm::curve(curve);

    return err(SubtleCallCheckerKnownCurveError{
        .code = SubtleCallCheckerKnownCurveErrorCode::UNKNOWN_CURVE,
        .message = message,
    });
  }

  return ok(curve);
}

SubtleCallChecker::ResultAesLengthValid SubtleCallChecker::aesLength(double bits) {
  if (bits != 128 && bits != 192 && bits != 256) {
    std::string message = ErrorTextAlgorithm::aesLength();

    return err(SubtleCallCheckerAesLengthError{
        .code = SubtleCallCheckerAesLengthErrorCode::BAD_LENGTH,
        .message = message,
    });
  }

  return ok(bits);
}

SubtleCallChecker::ResultGcmTagLengthOk SubtleCallChecker::gcmTagLength(double bits) {
  if (bits != 32 && bits != 64 && bits != 96 && bits != 104 && bits != 112 && bits != 120 &&
      bits != 128) {
    std::string message = ErrorTextAlgorithm::gcmTag();

    return err(SubtleCallCheckerGcmTagLengthError{
        .code = SubtleCallCheckerGcmTagLengthErrorCode::BAD_TAG,
        .message = message,
    });
  }

  return ok(bits);
}

std::string SubtleCallChecker::literalAlgorithm(const Expr *arg, llvm::StringRef op,
                                                llvm::StringRef fallback, bool &bad) {
  const auto *lit = dyn_cast_or_null<ObjectLitExpr>(arg);
  const Expr *nameExpr = SubtleCallChecker::algorithmNameExpr(arg, lit);
  if (nameExpr == nullptr) {
    return "";
  }
  std::optional<std::string> written = SubtleAlgorithms::stringLiteral(nameExpr);
  if (!written.has_value()) {
    return "";
  }
  ResultAlgorithmKnown known = SubtleCallChecker::knownAlgorithm(*written, op, fallback);
  if (known.isErr()) {
    bad = true;
    this->state.getDiagnostics().error(nameExpr->loc(), known.error().message);
    return "";
  }
  std::string name = known.value();
  Dictionary dictionary = SubtleAlgorithms::dictionaryOf(op, name);
  if (lit == nullptr) {
    ResultParamsAsObject object = SubtleCallChecker::paramsAsObject(name, op, dictionary);
    if (object.isErr()) {
      bad = true;
      this->state.getDiagnostics().error(nameExpr->loc(), object.error().message);
    }
    return bad ? "" : name;
  }
  ResultMembersPresent present = SubtleCallChecker::membersPresent(name, op, dictionary, lit);
  if (present.isErr()) {
    bad = true;
    this->state.getDiagnostics().error(lit->loc(), present.error().message);
    return "";
  }
  bool reported = this->reportsBadHash(lit, dictionary.name) || this->reportsBadCurve(lit) ||
                  this->reportsBadAesLength(lit, name, op) || this->reportsBadGcmTag(lit, name);
  if (reported) {
    bad = true;
    return "";
  }

  return name;
}

const Expr *SubtleCallChecker::algorithmNameExpr(const Expr *arg, const ObjectLitExpr *lit) {
  if (lit == nullptr) {
    return arg;
  }
  if (lit->hasSpread()) {
    return nullptr;
  }
  const ObjectLitExpr::Property *name = SubtleCallChecker::property(lit, "name");
  if (name == nullptr) {
    return nullptr;
  }

  return name->value;
}

bool SubtleCallChecker::reportsBadHash(const ObjectLitExpr *lit, llvm::StringRef dictionary) {
  const ObjectLitExpr::Property *hash = SubtleCallChecker::property(lit, "hash");
  if (hash == nullptr) {
    return false;
  }
  const Expr *value = hash->value;
  auto *inner = dyn_cast<ObjectLitExpr>(value);
  if (inner != nullptr) {
    const ObjectLitExpr::Property *innerName = SubtleCallChecker::property(inner, "name");
    if (innerName != nullptr) {
      value = innerName->value;
    }
  }
  std::optional<std::string> written = SubtleAlgorithms::stringLiteral(value);
  if (!written.has_value()) {
    return false;
  }
  ResultShaHashIsKnown sha = SubtleCallChecker::knownShaHash(*written, dictionary);
  if (sha.isErr()) {
    this->state.getDiagnostics().error(value->loc(), sha.error().message);
    return true;
  }

  return false;
}

bool SubtleCallChecker::reportsBadCurve(const ObjectLitExpr *lit) {
  const ObjectLitExpr::Property *curve = SubtleCallChecker::property(lit, "namedCurve");
  if (curve == nullptr) {
    return false;
  }
  std::optional<std::string> written = SubtleAlgorithms::stringLiteral(curve->value);
  if (!written.has_value()) {
    return false;
  }
  ResultCurveNameKnown named = SubtleCallChecker::knownCurve(*written);
  if (named.isErr()) {
    this->state.getDiagnostics().error(curve->value->loc(), named.error().message);
    return true;
  }

  return false;
}

bool SubtleCallChecker::reportsBadAesLength(const ObjectLitExpr *lit, llvm::StringRef name,
                                            llvm::StringRef op) {
  bool sized = name.starts_with("AES-") && (op == "generateKey" || op == "get key length");
  const ObjectLitExpr::Property *length =
      sized ? SubtleCallChecker::property(lit, "length") : nullptr;
  if (length == nullptr) {
    return false;
  }
  std::optional<double> bits = SubtleCallChecker::numberLiteral(length->value);
  if (!bits.has_value()) {
    return false;
  }
  ResultAesLengthValid valid = SubtleCallChecker::aesLength(*bits);
  if (valid.isErr()) {
    this->state.getDiagnostics().error(length->value->loc(), valid.error().message);
    return true;
  }

  return false;
}

bool SubtleCallChecker::reportsBadGcmTag(const ObjectLitExpr *lit, llvm::StringRef name) {
  const ObjectLitExpr::Property *tag =
      name == "AES-GCM" ? SubtleCallChecker::property(lit, "tagLength") : nullptr;
  if (tag == nullptr) {
    return false;
  }
  std::optional<double> bits = SubtleCallChecker::numberLiteral(tag->value);
  if (!bits.has_value()) {
    return false;
  }
  ResultGcmTagLengthOk valid = SubtleCallChecker::gcmTagLength(*bits);
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
  std::optional<SubtleMethod> method = SubtleMethodTable::find(member->name);
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

Expr *SubtleCallChecker::argAt(const CallExpr *call, size_t i) {
  return i < call->args.size() ? call->args[i] : nullptr;
}

const Expr *SubtleCallChecker::algorithmAt(const CallExpr *call, size_t i) const {
  Expr *arg = SubtleCallChecker::argAt(call, i);
  auto *name = dyn_cast_or_null<NameExpr>(arg);
  if (name == nullptr) {
    return arg;
  }
  VarSymbol *var = this->scopes->lookup(name->name).var();
  if (var == nullptr) {
    return arg;
  }
  const Expr *literal = this->state.getConstantStore().findConstLiteral(var);

  return literal != nullptr ? literal : arg;
}

void SubtleCallChecker::walkAlgorithmOnly(SubtleCall &subtle, llvm::StringRef op) {
  this->algorithmNameAt(subtle, 0, op, "");
}

std::string SubtleCallChecker::algorithmNameAt(SubtleCall &subtle, size_t i, llvm::StringRef op,
                                               llvm::StringRef fallback) {
  const Expr *algorithm = this->algorithmAt(subtle.call, i);

  return this->literalAlgorithm(algorithm, op, fallback, subtle.bad);
}

std::string SubtleCallChecker::keyFormatAt(SubtleCall &subtle, size_t i) {
  Expr *given = SubtleCallChecker::argAt(subtle.call, i);
  std::optional<std::string> written = SubtleAlgorithms::stringLiteral(given);
  if (!written.has_value()) {
    return "";
  }
  ResultKeyFormatKnown known = SubtleCallChecker::knownFormat(*written);
  if (known.isErr()) {
    subtle.bad = true;
    this->state.getDiagnostics().error(given->loc(), known.error().message);
    return "";
  }

  return *written;
}

void SubtleCallChecker::walkGenerateKey(SubtleCall &subtle) {
  std::string name = this->algorithmNameAt(subtle, 0, "generateKey", "");
  llvm::StringRef family = SubtleAlgorithms::familyOf(name);
  int isPublic = family == "Hmac" || family == "Aes" ? 0 : -1;
  this->walkUsages(subtle, 2, name, isPublic, true);
  if (!name.empty()) {
    subtle.target = "subtleGenerate" + family.str();
  }
}

void SubtleCallChecker::walkImportKey(SubtleCall &subtle) {
  std::string format = this->keyFormatAt(subtle, 0);
  std::string name = this->algorithmNameAt(subtle, 2, "importKey", "");
  this->walkImportFormat(subtle, format, name, false);
  int isPublic = SubtleCallChecker::importedPublicness(format, name);
  this->walkUsages(subtle, 4, name, isPublic, isPublic == 0);
  subtle.keyFormat = format;
  if (!name.empty()) {
    subtle.target = "subtleImport" + SubtleAlgorithms::familyOf(name).str();
  }
  bool jwkText = format.empty() || format == "jwk";
  bool wrapsJson = jwkText && this->isJsonValue(SubtleCallChecker::argAt(subtle.call, 1)) &&
                   isa<NameExpr>(subtle.member->object) &&
                   this->state.getLibraryDeclarations().findLibraryFunction("subtleJwkOf");
  if (wrapsJson) {
    this->wrapJwkArgument(subtle);
  }
}

void SubtleCallChecker::walkImportFormat(SubtleCall &subtle, const std::string &format,
                                         const std::string &name, bool unwraps) {
  if (format.empty() || name.empty()) {
    return;
  }
  std::vector<llvm::StringLiteral> formats =
      SubtleAlgorithms::formatsOf(SubtleAlgorithms::familyOf(name));
  if (llvm::is_contained(formats, format)) {
    return;
  }
  subtle.bad = true;
  std::string message = unwraps ? ErrorTextSubtleImport::unwrap(name, format, formats)
                                : ErrorTextSubtleImport::format(name, format, formats);
  this->state.getDiagnostics().error(subtle.call->args[0]->loc(), message);
}

int SubtleCallChecker::importedPublicness(const std::string &format, const std::string &name) {
  llvm::StringRef family = SubtleAlgorithms::familyOf(name);
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
  Expr *json = this->exprChecker->checkExpr(args[1]);
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
  std::string format = this->keyFormatAt(subtle, 0);
  if (!format.empty()) {
    subtle.target = format == "jwk" ? "subtleExportJwk" : "subtleExportBytes";
  }
}

void SubtleCallChecker::walkDeriveKey(SubtleCall &subtle) {
  this->algorithmNameAt(subtle, 0, "deriveBits", "");
  std::string name = this->algorithmNameAt(subtle, 2, "importKey", "");
  if (name.empty()) {
    return;
  }
  llvm::StringRef family = SubtleAlgorithms::familyOf(name);
  if (family != "Hmac" && family != "Aes" && family != "Kdf") {
    subtle.bad = true;
    std::string message = ErrorTextSubtleImport::derived(name);
    this->state.getDiagnostics().error(SubtleCallChecker::argAt(subtle.call, 2)->loc(), message);
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
  std::string format = this->keyFormatAt(subtle, 0);
  this->algorithmNameAt(subtle, 3, "unwrapKey", "decrypt");
  std::string name = this->algorithmNameAt(subtle, 4, "importKey", "");
  this->walkImportFormat(subtle, format, name, true);
  int isPublic = SubtleCallChecker::importedPublicness(format, name);
  this->walkUsages(subtle, 6, name, isPublic, isPublic == 0);
  if (!name.empty()) {
    subtle.target = "subtleUnwrap" + SubtleAlgorithms::familyOf(name).str();
  }
}

void SubtleCallChecker::walkUsages(SubtleCall &subtle, size_t i, llvm::StringRef name, int isPublic,
                                   bool creates) {
  auto *lit = dyn_cast_or_null<ArrayLitExpr>(SubtleCallChecker::argAt(subtle.call, i));
  if (lit == nullptr) {
    return;
  }
  std::vector<llvm::StringLiteral> allowed = SubtleCallChecker::allowedUsages(name, isPublic);
  for (const ArrayLitExpr::Element &element : lit->elements) {
    std::optional<std::string> usage =
        element.spread ? std::nullopt : SubtleAlgorithms::stringLiteral(element.expr);
    if (!usage.has_value()) {
      return;
    }
    if (!llvm::is_contained(kUsages, *usage)) {
      subtle.bad = true;
      std::string message = ErrorTextSubtleUsage::unknown(*usage);
      this->state.getDiagnostics().error(element.expr->loc(), message);
      return;
    }
    if (!name.empty() && !llvm::is_contained(allowed, *usage)) {
      subtle.bad = true;
      std::string message = ErrorTextSubtleUsage::notOf(*usage, name, isPublic, allowed);
      this->state.getDiagnostics().error(element.expr->loc(), message);
      return;
    }
  }
  bool ownNeeded = creates && !name.empty() && isPublic != 1;
  if (ownNeeded) {
    this->walkOwnUsage(subtle, lit, name, isPublic);
  }
}

std::vector<llvm::StringLiteral> SubtleCallChecker::allowedUsages(llvm::StringRef name,
                                                                  int isPublic) {
  std::vector<llvm::StringLiteral> allowed;
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

void SubtleCallChecker::walkOwnUsage(SubtleCall &subtle, const ArrayLitExpr *lit,
                                     llvm::StringRef name, int isPublic) {
  std::vector<llvm::StringLiteral> own = SubtleAlgorithms::usagesOf(name, false);
  bool any = false;
  for (const ArrayLitExpr::Element &element : lit->elements) {
    any |= llvm::is_contained(own, *SubtleAlgorithms::stringLiteral(element.expr));
  }
  if (any) {
    return;
  }
  subtle.bad = true;
  bool single = lit->elements.empty() || isPublic == 0;
  std::string message =
      single ? ErrorTextSubtleUsage::empty(own) : ErrorTextSubtleUsage::pairEmpty(own);
  this->state.getDiagnostics().error(lit->loc(), message);
}

Expr *SubtleCallChecker::checkRejectedSubtleCall(SubtleCall &subtle) {
  CallExpr *call = subtle.call;
  std::vector<Expr *> &args = call->args;
  call->type = Type::error();
  const ClassDecl *subtleClass =
      this->state.getLibraryDeclarations().findLibraryClass("SubtleCrypto");
  std::vector<FunctionDecl *> methods =
      ClassDeclarer::methodsNamed(subtleClass, subtle.member->name, false);
  FuncSymbol *sym = methods.size() == 1 ? methods[0]->sym : nullptr;
  for (size_t i = 0; i < args.size(); ++i) {
    if (subtle.checkedArgs[i]) {
      continue;
    }
    bool typed = sym != nullptr && i < sym->params.size();
    args[i] = typed ? this->argumentChecker->checkArg(args[i], sym->params[i])
                    : this->exprChecker->checkExpr(args[i]);
  }

  return call;
}

Expr *SubtleCallChecker::finishSubtleCall(SubtleCall &subtle, const Type *expected) {
  MemberExpr *member = subtle.member;
  bool library = !subtle.target.empty() &&
                 this->state.getLibraryDeclarations().findLibraryFunction(subtle.target) &&
                 isa<NameExpr>(member->object);
  Expr *checked = nullptr;
  if (library) {
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
  Expr *data = SubtleCallChecker::argAt(subtle.call, 1);
  bool unknown =
      data == nullptr || subtle.keyFormat.empty() || data->type == nullptr || data->type->isError();
  if (unknown) {
    return true;
  }
  const Expr *bare = SubtleCallChecker::withoutImplicitCasts(data);
  const Type *type = bare->type;
  if (type == nullptr || type->isError()) {
    return true;
  }
  bool jwkFormat = subtle.keyFormat == "jwk";
  if (jwkFormat && SubtleCallChecker::holdsBytes(type)) {
    std::string message = ErrorTextSubtleKeyData::jwk(type->name());
    this->state.getDiagnostics().error(bare->loc(), message);
    return false;
  }
  if (!jwkFormat && SubtleCallChecker::isJwkObject(type)) {
    std::string message = ErrorTextSubtleKeyData::bytes(subtle.keyFormat, type->name());
    this->state.getDiagnostics().error(bare->loc(), message);
    return false;
  }

  return true;
}

const Expr *SubtleCallChecker::withoutImplicitCasts(const Expr *expr) {
  const Expr *bare = expr;
  auto *cast = dyn_cast<CastExpr>(bare);
  while (cast != nullptr && cast->isImplicit()) {
    bare = cast->operand;
    cast = dyn_cast<CastExpr>(bare);
  }

  return bare;
}

bool SubtleCallChecker::holdsBytes(const Type *type) {
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

bool SubtleCallChecker::isJwkObject(const Type *type) {
  return !type->isBytes() && type->isObject() && ClassDeclarer::classOf(type) == nullptr &&
         !type->isArray() && !type->isTuple();
}

bool SubtleCallChecker::isJsonValue(const Expr *expr) const {
  const Expr *root = expr;
  const Expr *inner = SubtleCallChecker::accessInner(root);
  while (inner != nullptr) {
    root = inner;
    inner = SubtleCallChecker::accessInner(root);
  }
  if (auto *name = dyn_cast_or_null<NameExpr>(root)) {
    VarSymbol *var = this->scopes->lookup(name->name).var();
    return var && var->type && var->type->nonOptional()->isJson();
  }
  auto *call = dyn_cast_or_null<CallExpr>(root);
  auto *member = call != nullptr ? dyn_cast<MemberExpr>(call->callee) : nullptr;
  if (member == nullptr) {
    return false;
  }

  return member->name == "parse" && member->typeArgs.empty() &&
         this->callChecker->isGlobalName(member->object, GlobalName::JSON);
}

const Expr *SubtleCallChecker::accessInner(const Expr *expr) {
  if (auto *member = dyn_cast_or_null<MemberExpr>(expr)) {
    return member->object;
  }
  if (auto *index = dyn_cast_or_null<IndexExpr>(expr)) {
    return index->object;
  }

  return nullptr;
}

}
