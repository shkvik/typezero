#include "compiler/semantic/library/crypto/subtle_call_checker/subtle_call_checker.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/crypto/crypto_api/crypto_api.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/classes/method_call_checker/method_call_checker.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
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
  this->returnInferrer = &all.getReturnInferrer();
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
  const ObjectLitExpr *lit = dyn_cast_or_null<ObjectLitExpr>(arg);
  const Expr *nameExpr = arg;
  if (lit) {
    if (lit->hasSpread()) {
      return "";
    }
    const ObjectLitExpr::Property *name = SubtleCallChecker::property(lit, "name");
    if (!name) {
      return "";
    }
    nameExpr = name->value;
  }
  std::optional<std::string> written = SubtleAlgorithms::stringLiteral(nameExpr);
  if (!written) {
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
  if (!lit) {
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
  if (const ObjectLitExpr::Property *hash = SubtleCallChecker::property(lit, "hash")) {
    const Expr *value = hash->value;
    if (auto *inner = dyn_cast<ObjectLitExpr>(value)) {
      if (const ObjectLitExpr::Property *n = SubtleCallChecker::property(inner, "name")) {
        value = n->value;
      }
    }
    if (std::optional<std::string> h = SubtleAlgorithms::stringLiteral(value)) {
      ResultShaHashIsKnown sha = SubtleCallChecker::knownShaHash(*h, dictionary.name);
      if (sha.isErr()) {
        bad = true;
        this->state.getDiagnostics().error(value->loc(), sha.error().message);
        return "";
      }
    }
  }
  if (const ObjectLitExpr::Property *curve = SubtleCallChecker::property(lit, "namedCurve")) {
    if (std::optional<std::string> c = SubtleAlgorithms::stringLiteral(curve->value)) {
      ResultCurveNameKnown named = SubtleCallChecker::knownCurve(*c);
      if (named.isErr()) {
        bad = true;
        this->state.getDiagnostics().error(curve->value->loc(), named.error().message);
        return "";
      }
    }
  }
  if (name.rfind("AES-", 0) == 0 && (op == "generateKey" || op == "get key length")) {
    if (const ObjectLitExpr::Property *length = SubtleCallChecker::property(lit, "length")) {
      if (std::optional<double> n = SubtleCallChecker::numberLiteral(length->value)) {
        ResultAesLengthValid bits = SubtleCallChecker::aesLength(*n);
        if (bits.isErr()) {
          bad = true;
          this->state.getDiagnostics().error(length->value->loc(), bits.error().message);
          return "";
        }
      }
    }
  }
  if (name == "AES-GCM") {
    if (const ObjectLitExpr::Property *tag = SubtleCallChecker::property(lit, "tagLength")) {
      if (std::optional<double> n = SubtleCallChecker::numberLiteral(tag->value)) {
        ResultGcmTagLengthOk bits = SubtleCallChecker::gcmTagLength(*n);
        if (bits.isErr()) {
          bad = true;
          this->state.getDiagnostics().error(tag->value->loc(), bits.error().message);
          return "";
        }
      }
    }
  }

  return name;
}

Expr *SubtleCallChecker::checkSubtleCall(CallExpr *call, MemberExpr *member, const Type *expected) {
  llvm::StringRef method = member->name;
  std::vector<Expr *> &args = call->args;
  if (call->hasSpread()) {
    return nullptr;
  }
  auto arg = [&](size_t i) -> Expr * { return i < args.size() ? args[i] : nullptr; };
  auto algorithm = [&](size_t i) -> const Expr * {
    Expr *e = arg(i);
    if (auto *name = dyn_cast_or_null<NameExpr>(e)) {
      if (VarSymbol *var = this->scopes->lookup(name->name).var()) {
        if (const Expr *lit = this->state.getConstantStore().findConstLiteral(var)) {
          return lit;
        }
      }
    }

    return e;
  };
  bool bad = false;

  auto format = [&](size_t i) -> std::string {
    std::optional<std::string> f = SubtleAlgorithms::stringLiteral(arg(i));
    if (!f) {
      return "";
    }
    ResultKeyFormatKnown known = SubtleCallChecker::knownFormat(*f);
    if (known.isErr()) {
      bad = true;
      this->state.getDiagnostics().error(arg(i)->loc(), known.error().message);
      return "";
    }

    return *f;
  };

  auto usages = [&](size_t i, llvm::StringRef name, int isPublic, bool creates) {
    auto *lit = dyn_cast_or_null<ArrayLitExpr>(arg(i));
    if (!lit) {
      return;
    }
    std::vector<llvm::StringLiteral> allowed;
    if (!name.empty()) {
      if (isPublic != 0) {
        allowed = SubtleAlgorithms::usagesOf(name, true);
      }
      if (isPublic != 1) {
        for (llvm::StringLiteral u : SubtleAlgorithms::usagesOf(name, false)) {
          if (!llvm::is_contained(allowed, u)) {
            allowed.push_back(u);
          }
        }
      }
    }
    for (const ArrayLitExpr::Element &e : lit->elements) {
      std::optional<std::string> u =
          e.spread ? std::nullopt : SubtleAlgorithms::stringLiteral(e.expr);
      if (!u) {
        return;
      }
      if (!llvm::is_contained(kUsages, *u)) {
        bad = true;
        std::string message = ErrorTextSubtleUsage::unknown(*u);
        this->state.getDiagnostics().error(e.expr->loc(), message);
        return;
      }
      if (!name.empty() && !llvm::is_contained(allowed, *u)) {
        bad = true;
        std::string message = ErrorTextSubtleUsage::notOf(*u, name, isPublic, allowed);
        this->state.getDiagnostics().error(e.expr->loc(), message);
        return;
      }
    }
    if (!creates || name.empty() || isPublic == 1) {
      return;
    }
    std::vector<llvm::StringLiteral> own = SubtleAlgorithms::usagesOf(name, false);
    bool any = false;
    for (const ArrayLitExpr::Element &e : lit->elements) {
      any |= llvm::is_contained(own, *SubtleAlgorithms::stringLiteral(e.expr));
    }
    if (any) {
      return;
    }
    bad = true;
    std::string message = lit->elements.empty() || isPublic == 0
                              ? ErrorTextSubtleUsage::empty(own)
                              : ErrorTextSubtleUsage::pairEmpty(own);
    this->state.getDiagnostics().error(lit->loc(), message);
  };

  std::string keyFormat;
  auto keyDataFits = [&]() {
    Expr *data = arg(1);
    if (!data || keyFormat.empty() || !data->type || data->type->isError()) {
      return true;
    }
    while (auto *conv = dyn_cast<CastExpr>(data)) {
      if (!conv->isImplicit()) {
        break;
      }
      data = conv->operand;
    }
    const Type *type = data->type;
    if (!type || type->isError()) {
      return true;
    }
    bool bytes = type->isBytes() ||
                 (type->isUnion() &&
                  llvm::all_of(type->members(), [](const Type *t) { return t->isBytes(); }));
    bool jwk = !type->isBytes() && type->isObject() && !ClassDeclarer::classOf(type) &&
               !type->isArray() && !type->isTuple();
    if (keyFormat == "jwk" && bytes) {
      std::string message = ErrorTextSubtleKeyData::jwk(type->name());
      this->state.getDiagnostics().error(data->loc(), message);
      return false;
    }
    if (keyFormat != "jwk" && jwk) {
      std::string message = ErrorTextSubtleKeyData::bytes(keyFormat, type->name());
      this->state.getDiagnostics().error(data->loc(), message);
      return false;
    }

    return true;
  };

  std::string target;
  std::vector<bool> checkedArgs(args.size(), false);
  if (method == "digest") {
    this->literalAlgorithm(algorithm(0), "digest", "", bad);
  } else if (method == "generateKey") {
    std::string name = this->literalAlgorithm(algorithm(0), "generateKey", "", bad);
    llvm::StringRef family = SubtleAlgorithms::familyOf(name);
    usages(2, name, family == "Hmac" || family == "Aes" ? 0 : -1, true);
    if (!name.empty()) {
      target = "subtleGenerate" + SubtleAlgorithms::familyOf(name).str();
    }
  } else if (method == "importKey") {
    std::string f = format(0);
    std::string name = this->literalAlgorithm(algorithm(2), "importKey", "", bad);
    if (!f.empty() && !name.empty()) {
      std::vector<llvm::StringLiteral> formats =
          SubtleAlgorithms::formatsOf(SubtleAlgorithms::familyOf(name));
      if (!llvm::is_contained(formats, f)) {
        bad = true;
        std::string message = ErrorTextSubtleImport::format(name, f, formats);
        this->state.getDiagnostics().error(arg(0)->loc(), message);
      }
    }
    int isPublic = f == "spki" || f == "raw" ? 1 : f == "pkcs8" ? 0 : -1;
    if (SubtleAlgorithms::familyOf(name) == "Hmac" || SubtleAlgorithms::familyOf(name) == "Aes" ||
        SubtleAlgorithms::familyOf(name) == "Kdf") {
      isPublic = 0;
    }
    usages(4, name, isPublic, isPublic == 0);
    keyFormat = f;
    if (!name.empty()) {
      target = "subtleImport" + SubtleAlgorithms::familyOf(name).str();
    }
    if ((f.empty() || f == "jwk") && this->isJsonValue(arg(1)) && isa<NameExpr>(member->object) &&
        this->state.getLibraryDeclarations().findLibraryFunction("subtleJwkOf")) {
      Expr *json = this->exprChecker->checkExpr(arg(1));
      auto *callee = this->state.getAst().make<NameExpr>(json->loc(), "subtleJwkOf");
      auto *wrap =
          this->state.getAst().make<CallExpr>(json->loc(), callee, std::vector<Expr *>{json});
      args[1] = this->eventTargetChecker->callLibraryFunction(wrap, "subtleJwkOf", {}, {true},
                                                              json->loc());
      checkedArgs[1] = true;
      if (target.empty()) {
        target = "subtleImportAny";
      }
    }
  } else if (method == "exportKey") {
    std::string f = format(0);
    if (!f.empty()) {
      target = f == "jwk" ? "subtleExportJwk" : "subtleExportBytes";
    }
  } else if (method == "sign" || method == "verify" || method == "encrypt" || method == "decrypt" ||
             method == "deriveBits") {
    this->literalAlgorithm(algorithm(0), method, "", bad);
  } else if (method == "deriveKey") {
    this->literalAlgorithm(algorithm(0), "deriveBits", "", bad);
    std::string name = this->literalAlgorithm(algorithm(2), "importKey", "", bad);
    if (!name.empty()) {
      llvm::StringRef family = SubtleAlgorithms::familyOf(name);
      if (family != "Hmac" && family != "Aes" && family != "Kdf") {
        bad = true;
        std::string message = ErrorTextSubtleImport::derived(name);
        this->state.getDiagnostics().error(arg(2)->loc(), message);
      } else {
        if (family != "Kdf") {
          this->literalAlgorithm(algorithm(2), "get key length", "", bad);
        }
        usages(4, name, 0, true);
      }
    }
  } else if (method == "wrapKey") {
    format(0);
    this->literalAlgorithm(algorithm(3), "wrapKey", "encrypt", bad);
  } else if (method == "unwrapKey") {
    std::string f = format(0);
    this->literalAlgorithm(algorithm(3), "unwrapKey", "decrypt", bad);
    std::string name = this->literalAlgorithm(algorithm(4), "importKey", "", bad);
    if (!f.empty() && !name.empty()) {
      std::vector<llvm::StringLiteral> formats =
          SubtleAlgorithms::formatsOf(SubtleAlgorithms::familyOf(name));
      if (!llvm::is_contained(formats, f)) {
        bad = true;
        std::string message = ErrorTextSubtleImport::unwrap(name, f, formats);
        this->state.getDiagnostics().error(arg(0)->loc(), message);
      }
    }
    int isPublic = f == "spki" || f == "raw" ? 1 : f == "pkcs8" ? 0 : -1;
    if (SubtleAlgorithms::familyOf(name) == "Hmac" || SubtleAlgorithms::familyOf(name) == "Aes" ||
        SubtleAlgorithms::familyOf(name) == "Kdf") {
      isPublic = 0;
    }
    usages(6, name, isPublic, isPublic == 0);
    if (!name.empty()) {
      target = "subtleUnwrap" + SubtleAlgorithms::familyOf(name).str();
    }
  } else {
    return nullptr;
  }

  if (bad) {
    call->type = Type::error();
    std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(
        this->state.getLibraryDeclarations().findLibraryClass("SubtleCrypto"), method, false);
    FuncSymbol *sym = methods.size() == 1 ? methods[0]->sym : nullptr;
    if (sym) {
      this->returnInferrer->resolveParams(sym);
    }
    for (size_t i = 0; i < args.size(); ++i) {
      if (!checkedArgs[i]) {
        args[i] = sym && i < sym->params.size()
                      ? this->argumentChecker->checkArg(args[i], sym->params[i])
                      : this->exprChecker->checkExpr(args[i]);
      }
    }
    return call;
  }
  Expr *checked;
  if (!target.empty() && this->state.getLibraryDeclarations().findLibraryFunction(target) &&
      isa<NameExpr>(member->object)) {
    checked =
        this->eventTargetChecker->callLibraryFunction(call, target, {}, checkedArgs, member->loc());
  } else {
    checked = this->methodCallChecker->checkMethodCall(call, member, expected);
  }
  if (!keyDataFits()) {
    checked->type = Type::error();
  }

  return checked;
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
