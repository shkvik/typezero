#include "compiler/semantic/library_v2/crypto/library_crypto_registry.h"
#include "compiler/program/library/builtin_modules/known_modules/known_modules.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/builtin_names/library_class_table/library_class_table.h"
#include "compiler/program/library/builtin_names/library_function_table/library_function_table.h"
#include "compiler/program/library/bytes/bytes_method_table/bytes_method_table.h"
#include "compiler/program/library/crypto/cipher_name_table/cipher_name_table.h"
#include "compiler/program/library/crypto/crypto_api/crypto_api.h"
#include "compiler/program/library/crypto/crypto_async_function_table/crypto_async_function_table.h"
#include "compiler/program/library/crypto/crypto_choice_table/crypto_choice_table.h"
#include "compiler/program/library/crypto/crypto_class_table/crypto_class_table.h"
#include "compiler/program/library/crypto/crypto_constant_table/crypto_constant_table.h"
#include "compiler/program/library/crypto/crypto_error_types/crypto_error_types.h"
#include "compiler/program/library/crypto/crypto_export_table/crypto_export_table.h"
#include "compiler/program/library/crypto/crypto_method_table/crypto_method_table.h"
#include "compiler/program/library/crypto/crypto_option_table/crypto_option_table.h"
#include "compiler/program/library/crypto/crypto_property_table/crypto_property_table.h"
#include "compiler/program/library/crypto/crypto_stream_method_table/crypto_stream_method_table.h"
#include "compiler/program/library/crypto/crypto_sync_function_table/crypto_sync_function_table.h"
#include "compiler/program/library/crypto/curve_name_table/curve_name_table.h"
#include "compiler/program/library/crypto/hash_name_table/hash_name_table.h"
#include "compiler/program/library/crypto/key_pair_shape/key_pair_shape.h"
#include "compiler/program/library/crypto/subtle_algorithm_table/subtle_algorithm_table.h"
#include "compiler/program/library/crypto/subtle_dictionary_table/subtle_dictionary_table.h"
#include "compiler/program/library/crypto/subtle_family_table/subtle_family_table.h"
#include "compiler/program/library/crypto/subtle_intrinsic_table/subtle_intrinsic_table.h"
#include "compiler/program/library/crypto/subtle_key_format_table/subtle_key_format_table.h"
#include "compiler/program/library/crypto/subtle_key_usage_table/subtle_key_usage_table.h"
#include "compiler/program/library/crypto/subtle_member_table/subtle_member_table.h"
#include "compiler/program/library/crypto/subtle_method_table/subtle_method_table.h"
#include "compiler/program/library/crypto/subtle_operation_table/subtle_operation_table.h"
#include "compiler/program/library/crypto/web_crypto_member_table/web_crypto_member_table.h"
#include "compiler/program/library/crypto/x509_static_table/x509_static_table.h"
#include "compiler/program/library/json/json_static_table/json_static_table.h"
#include "compiler/program/library/library_module_names/library_module_names.h"
#include "compiler/program/to_utf16/to_utf16.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/program/types/types.h"
#include "compiler/semantic/library/bytes/bytes_check_helpers/bytes_check_helpers.h"
#include "compiler/semantic/library/crypto/subtle_algorithms/subtle_algorithms.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FormatVariadic.h"
#include <cmath>
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

using Crypto = Type::Crypto;

constexpr CryptoDataKinds kAnyCryptoData{
    .isString = true,
    .isKey = true,
    .isArrayBuffer = true,
};
constexpr CryptoDataKinds kTextOrBuffers{
    .isString = true,
    .isKey = false,
    .isArrayBuffer = true,
};
constexpr CryptoDataKinds kTextOrViews{
    .isString = true,
    .isKey = false,
    .isArrayBuffer = false,
};
constexpr CryptoDataKinds kBuffersOnly{
    .isString = false,
    .isKey = false,
    .isArrayBuffer = true,
};
constexpr CryptoDataKinds kKeysOrBuffers{
    .isString = false,
    .isKey = true,
    .isArrayBuffer = true,
};

}

LibraryCryptoRegistry::LibraryCryptoRegistry(SemanticState &owner)
    : state(owner) {}

void LibraryCryptoRegistry::checkCryptoValue(const CheckCryptoValue &task) {
  auto &tasks = this->state.walk.tasks;
  if (CryptoSyncFunctionTable::find(task.name) == CryptoSyncFunction::CONSTANTS) {
    auto message = ErrorTextCryptoValue::constants();
    this->state.diagnostics.error(task.expr->loc(), message);

    return tasks.pushChecked(task.expr);
  }
  if (this->state.library.crypto.cryptoTypeNamed(task.name) != nullptr) {
    auto message = ErrorTextCryptoValue::isType(task.name);
    this->state.diagnostics.error(task.expr->loc(), message);

    return tasks.pushChecked(task.expr);
  }

  return tasks.pushChecked(nullptr);
}

void LibraryCryptoRegistry::checkCryptoModuleMember(const CheckCryptoModuleMember &task) {
  MemberExpr *member = task.member;
  auto *object = dyn_cast<NameExpr>(member->object);
  const NodeName *nodeName = object != nullptr ? this->state.findNodeName(object->name) : nullptr;
  bool isConstants = nodeName != nullptr && nodeName->module == NodeModule::Crypto &&
                     CryptoSyncFunctionTable::find(nodeName->name) == CryptoSyncFunction::CONSTANTS;
  if (!isConstants) {
    return this->state.walk.tasks.pushChecked(nullptr);
  }

  return this->pushCryptoConstant(member);
}

void LibraryCryptoRegistry::pushCryptoConstant(MemberExpr *member) {
  auto &tasks = this->state.walk.tasks;
  auto value = CryptoConstantTable::find(member->name);
  if (!value.has_value()) {
    auto message = ErrorTextCryptoValue::constant(member->name);
    this->state.diagnostics.error(member->loc(), message);
    member->type = Type::error();

    return tasks.pushChecked(member);
  }
  int64_t magnitude = *value < 0 ? -*value : *value;
  Expr *literal = this->state.ast.make<IntLitExpr>(member->loc(), uint64_t(magnitude));
  literal->type = Type::intTy();
  if (*value >= 0) {
    return tasks.pushChecked(literal);
  }
  auto *negated = this->state.ast.make<UnaryExpr>(member->loc(), UnaryOp::Neg, literal);

  return tasks.pushTask(CheckExpr{.expr = negated, .expected = nullptr});
}

void LibraryCryptoRegistry::checkCryptoMember(const CheckCryptoMember &task) {
  MemberExpr *member = task.member;
  auto *checked = this->cryptoMember(member);

  return this->state.walk.tasks.pushChecked(checked);
}

void LibraryCryptoRegistry::checkCryptoNew(const CheckCryptoNew &task) {
  NewExpr *expr = task.newExpr;
  auto &tasks = this->state.walk.tasks;
  expr->type = Type::error();
  bool isX509 = CryptoClassTable::find(task.name) == Crypto::X509;
  std::string message =
      isX509 ? ErrorTextCryptoMake::x509() : ErrorTextCryptoMake::notNode(task.name);
  this->state.diagnostics.error(expr->loc(), message);
  tasks.pushChecked(expr);
  for (Expr *&arg : llvm::reverse(expr->args)) {
    tasks.pushTask(CheckExprSlot{.slot = &arg, .expected = nullptr});
  }
}

void LibraryCryptoRegistry::checkX509Parse(const CheckX509Parse &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  call->type = Type::error();
  tasks.pushChecked(call);
  if (X509StaticTable::find(member->name) != X509Static::PARSE) {
    auto message = ErrorTextX509::noStatic(member->name);
    this->state.diagnostics.error(member->loc(), message);

    return this->pushLooseArgs(call);
  }
  if (call->args.size() != 1) {
    auto message = ErrorTextX509::parseArgs();
    this->state.diagnostics.error(call->loc(), message);

    return this->pushLooseArgs(call);
  }
  auto draft = this->functionDraft(call, call->loc());
  draft.fnCode = CryptoFn::X509New;
  draft.resultType = Type::cryptoType(Crypto::X509);
  auto what = ErrorTextX509::data();

  return this->pushPlan(draft, CryptoFinish::CHECKED_RESULT,
                        {this->dataItem(call->args[0], what, kTextOrBuffers)});
}

void LibraryCryptoRegistry::checkWebCryptoCall(const CheckWebCryptoCall &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  llvm::StringRef name = member->name;
  auto webMember = WebCryptoMemberTable::find(name);
  bool isFunction =
      webMember == WebCryptoMember::RANDOM_UUID || webMember == WebCryptoMember::GET_RANDOM_VALUES;
  if (isFunction) {
    return tasks.pushTask(
        CheckCryptoCall{.call = call, .functionName = name, .loc = member->loc()});
  }
  call->type = Type::error();
  std::string message = webMember == WebCryptoMember::SUBTLE
                            ? ErrorTextGlobalCrypto::subtleCall()
                            : ErrorTextGlobalCrypto::noMember(name);
  tasks.pushChecked(call);
  tasks.pushTask(ReportError{.loc = member->loc(), .message = message});

  return this->pushLooseArgs(call);
}

void LibraryCryptoRegistry::checkWebCryptoMember(const CheckWebCryptoMember &task) {
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  member->type = Type::error();
  auto webMember = WebCryptoMemberTable::find(member->name);
  if (webMember == WebCryptoMember::SUBTLE) {
    return this->pushSubtleObject(member);
  }
  bool isFunction =
      webMember == WebCryptoMember::RANDOM_UUID || webMember == WebCryptoMember::GET_RANDOM_VALUES;
  std::string message = isFunction ? ErrorTextGlobalCrypto::onlyCalled(member->name)
                                   : ErrorTextGlobalCrypto::noMember(member->name);
  this->state.diagnostics.error(member->loc(), message);

  return tasks.pushChecked(member);
}

void LibraryCryptoRegistry::pushSubtleObject(MemberExpr *member) {
  auto &tasks = this->state.walk.tasks;
  auto subtle = this->checkSubtleAvailable();
  if (subtle.isErr()) {
    this->state.diagnostics.error(member->loc(), subtle.error().message);
    member->type = Type::error();

    return tasks.pushChecked(member);
  }
  auto *name = this->state.ast.make<NameExpr>(member->loc(), LibraryModuleNames::kSubtleName);

  return tasks.pushTask(CheckExpr{.expr = name, .expected = nullptr});
}

void LibraryCryptoRegistry::pushLooseArgs(CallExpr *call) {
  for (Expr *&arg : llvm::reverse(call->args)) {
    this->state.walk.tasks.pushTask(CheckExprSlot{.slot = &arg, .expected = nullptr});
  }
}

CryptoPlanDraft LibraryCryptoRegistry::functionDraft(CallExpr *call, llvm::SMLoc loc) const {
  return CryptoPlanDraft{
      .call = call,
      .member = nullptr,
      .function = CryptoSyncFunction::CONSTANTS,
      .method = CryptoMethod::HASH_UPDATE,
      .fn = "",
      .shown = "",
      .loc = loc,
      .fnCode = CryptoFn::HashNew,
      .resultType = nullptr,
      .next = CryptoFinish::CALL,
      .option = nullptr,
      .choice = 0,
      .flags = 0,
      .isMethod = false,
  };
}

CryptoValueItem LibraryCryptoRegistry::valueItem(CryptoValueKind kind, Expr *arg) const {
  return CryptoValueItem{
      .kind = kind,
      .arg = arg,
      .what = "",
      .kinds = kAnyCryptoData,
      .use = DigestUse::HASH,
      .type = nullptr,
      .call = nullptr,
      .index = 0,
      .low = 0,
      .high = std::nullopt,
      .subtleKind = 's',
  };
}

CryptoValueItem LibraryCryptoRegistry::literalItem(Expr *expr) const {
  return this->valueItem(CryptoValueKind::LITERAL, expr);
}

CryptoValueItem LibraryCryptoRegistry::dataItem(Expr *arg, llvm::StringRef what,
                                                CryptoDataKinds kinds) const {
  auto item = this->valueItem(CryptoValueKind::DATA, arg);
  item.what = what.str();
  item.kinds = kinds;

  return item;
}

CryptoValueItem LibraryCryptoRegistry::mdItem(Expr *arg, DigestUse use) const {
  auto item = this->valueItem(CryptoValueKind::MD, arg);
  item.use = use;

  return item;
}

CryptoValueItem LibraryCryptoRegistry::cipherItem(Expr *arg) const {
  return this->valueItem(CryptoValueKind::CIPHER, arg);
}

CryptoValueItem LibraryCryptoRegistry::numberItem(Expr *arg) const {
  return this->valueItem(CryptoValueKind::NUMBER, arg);
}

CryptoValueItem LibraryCryptoRegistry::encodingItem(CallExpr *call, size_t index,
                                                    llvm::StringRef name, int fallback) const {
  auto item = this->valueItem(CryptoValueKind::ENCODING, nullptr);
  item.call = call;
  item.index = index;
  item.what = name.str();
  item.low = fallback;

  return item;
}

CryptoValueItem LibraryCryptoRegistry::argItem(Expr *arg, const Type *type) const {
  auto item = this->valueItem(CryptoValueKind::ARG, arg);
  item.type = type;

  return item;
}

CryptoValueItem LibraryCryptoRegistry::checkedItem(Expr *arg, const Type *expected) const {
  auto item = this->valueItem(CryptoValueKind::CHECKED, arg);
  item.type = expected;

  return item;
}

CryptoValueItem LibraryCryptoRegistry::keyObjectItem(Expr *arg, llvm::StringRef what) const {
  auto item = this->valueItem(CryptoValueKind::KEY_OBJECT, arg);
  item.what = what.str();

  return item;
}

CryptoValueItem LibraryCryptoRegistry::provenItem(Expr *arg, int64_t low,
                                                  std::optional<int64_t> high,
                                                  llvm::StringRef what) const {
  auto item = this->valueItem(CryptoValueKind::PROVEN_NUMBER, arg);
  item.low = low;
  item.high = high;
  item.what = what.str();

  return item;
}

CryptoValueItem LibraryCryptoRegistry::toDoubleItem(Expr *checked) const {
  return this->valueItem(CryptoValueKind::TO_DOUBLE, checked);
}

CryptoValueItem LibraryCryptoRegistry::bufferEncodingItem(Expr *arg) const {
  return this->valueItem(CryptoValueKind::BUFFER_ENCODING, arg);
}

CryptoValueItem LibraryCryptoRegistry::intLiteralItem(llvm::SMLoc loc, int64_t value) {
  auto *literal = this->cryptoInt(loc, value);

  return this->literalItem(literal);
}

CryptoValueItem LibraryCryptoRegistry::doubleLiteralItem(llvm::SMLoc loc, double value) {
  auto *literal = this->cryptoDouble(loc, value);

  return this->literalItem(literal);
}

CryptoValueItem LibraryCryptoRegistry::boolLiteralItem(llvm::SMLoc loc, bool value) {
  auto *literal = this->cryptoBool(loc, value);

  return this->literalItem(literal);
}

CryptoValueItem LibraryCryptoRegistry::stringLiteralItem(llvm::SMLoc loc, llvm::StringRef text) {
  auto *literal = this->cryptoString(loc, text);

  return this->literalItem(literal);
}

CryptoValueItem LibraryCryptoRegistry::undefinedItem(llvm::SMLoc loc) {
  auto *literal = this->cryptoUndefined(loc);

  return this->literalItem(literal);
}

void LibraryCryptoRegistry::pushPlan(const CryptoPlanDraft &draft, CryptoFinish finish,
                                     const std::vector<CryptoValueItem> &items) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(CryptoFinishStep{.draft = draft, .finish = finish, .count = items.size()});
  for (const CryptoValueItem &item : llvm::reverse(items)) {
    tasks.pushTask(CryptoValue{.item = item});
  }
}

std::vector<Expr *> LibraryCryptoRegistry::takeCheckedValues(size_t count) {
  auto values = std::vector<Expr *>(count, nullptr);
  for (size_t index = count; index > 0; --index) {
    values[index - 1] = this->state.walk.tasks.takeChecked();
  }

  return values;
}

void LibraryCryptoRegistry::cryptoValue(const CryptoValue &task) {
  const CryptoValueItem &item = task.item;
  auto &tasks = this->state.walk.tasks;
  switch (item.kind) {
    case CryptoValueKind::LITERAL:
      return tasks.pushChecked(item.arg);
    case CryptoValueKind::DATA:
      tasks.pushTask(AfterCryptoData{.what = item.what, .kinds = item.kinds});
      return tasks.pushTask(CheckExpr{.expr = item.arg, .expected = Type::string()});
    case CryptoValueKind::MD:
      return this->pushMd(item.arg, item.use);
    case CryptoValueKind::CIPHER:
      return this->pushCipher(item.arg);
    case CryptoValueKind::NUMBER:
      tasks.pushTask(AfterCryptoNumber{});
      return this->pushArg(item.arg, Type::intTy());
    case CryptoValueKind::ENCODING:
      return this->pushEncoding(item);
    case CryptoValueKind::ARG:
      return this->pushArg(item.arg, item.type);
    case CryptoValueKind::CHECKED:
      return tasks.pushTask(CheckExpr{.expr = item.arg, .expected = item.type});
    case CryptoValueKind::KEY_OBJECT:
      tasks.pushTask(AfterCryptoKeyObject{.what = item.what});
      return tasks.pushTask(CheckExpr{.expr = item.arg, .expected = nullptr});
    case CryptoValueKind::PROVEN_NUMBER:
      tasks.pushTask(AfterProvenNumber{.low = item.low, .high = item.high, .what = item.what});
      return tasks.pushTask(CheckExpr{.expr = item.arg, .expected = Type::intTy()});
    case CryptoValueKind::TO_DOUBLE:
      return this->pushToDouble(item.arg);
    case CryptoValueKind::BUFFER_ENCODING:
      return tasks.pushTask(BufferEncodingValue{.arg = item.arg});
    case CryptoValueKind::SUBTLE_ARG:
      return this->pushSubtleArg(item);
  }
}

void LibraryCryptoRegistry::pushArg(Expr *arg, const Type *type) {
  auto &tasks = this->state.walk.tasks;
  const Type *expected = type->isError() ? nullptr : type;
  tasks.pushTask(Coerce{.to = type, .what = ""});

  return tasks.pushTask(CheckExpr{.expr = arg, .expected = expected});
}

void LibraryCryptoRegistry::pushToDouble(Expr *checked) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushChecked(checked);
  if (checked->type->isFloat()) {
    return;
  }

  return tasks.pushTask(Convert{.to = Type::doubleTy()});
}

void LibraryCryptoRegistry::afterCryptoData(const AfterCryptoData &task) {
  auto &tasks = this->state.walk.tasks;
  Expr *arg = tasks.takeChecked();
  const Type *type = arg->type;
  tasks.pushChecked(arg);
  if (type->isError()) {
    return;
  }
  auto fits = std::bind_front(&LibraryCryptoRegistry::isCryptoData, this, task.kinds);
  bool isOk = fits(type) || (type->isUnion() && llvm::all_of(type->members(), fits));
  if (!isOk) {
    const CryptoDataKinds &kinds = task.kinds;
    auto message = ErrorTextCryptoData::notData(task.what, kinds.isString, kinds.isArrayBuffer,
                                                kinds.isKey, type);
    this->state.diagnostics.error(arg->loc(), message);
    arg->type = Type::error();
  }
  if (!type->isStringLiteral()) {
    return;
  }

  return tasks.pushTask(Coerce{.to = Type::string(), .what = ""});
}

void LibraryCryptoRegistry::afterCryptoNumber() {
  auto &tasks = this->state.walk.tasks;
  Expr *checked = tasks.takeChecked();
  tasks.pushChecked(checked);
  bool isKept = checked == nullptr || checked->type->isError() || checked->type->isFloat();
  if (isKept) {
    return;
  }

  return tasks.pushTask(Convert{.to = Type::doubleTy()});
}

void LibraryCryptoRegistry::afterCryptoKeyObject(const AfterCryptoKeyObject &task) {
  auto &tasks = this->state.walk.tasks;
  Expr *arg = tasks.takeChecked();
  auto key = this->checkKeyObjectType(arg->type, task.what);
  if (key.isErr()) {
    this->state.diagnostics.error(arg->loc(), key.error().message);
    arg->type = Type::error();
  }

  return tasks.pushChecked(arg);
}

void LibraryCryptoRegistry::afterProvenNumber(const AfterProvenNumber &task) {
  auto &tasks = this->state.walk.tasks;
  auto &proofs = this->state.flow.proofs;
  Expr *checked = tasks.takeChecked();
  if (checked->type->isError()) {
    return tasks.pushChecked(nullptr);
  }
  auto between = proofs.requireBetween(checked, task.low, task.high, task.what);
  this->reportUnprovenBetween(between, checked->loc());
  if (between.isErr() && !proofs.isExempt(checked->loc())) {
    return tasks.pushChecked(nullptr);
  }

  return this->pushToDouble(checked);
}

void LibraryCryptoRegistry::reportUnprovenBetween(const FlowProofs::ResultBetweenProven &between,
                                                  llvm::SMLoc at) {
  bool isShown =
      between.isErr() && between.error().code != FlowProofsRequireBetweenErrorCode::EXEMPT;
  if (isShown) {
    this->state.diagnostics.error(at, between.error().message);
  }
}

void LibraryCryptoRegistry::reportUnprovenUpTo(const FlowProofs::ResultUpToWasProven &upTo,
                                               llvm::SMLoc at) {
  bool isShown = upTo.isErr() && upTo.error().code != FlowProofsRequireUpToErrorCode::EXEMPT;
  if (isShown) {
    this->state.diagnostics.error(at, upTo.error().message);
  }
}

void LibraryCryptoRegistry::pushMd(Expr *arg, DigestUse use) {
  auto text = this->literalOf(arg);
  if (!text.has_value()) {
    return this->pushArg(arg, Type::string());
  }
  auto *md = this->literalMd(arg, *text, use);

  return this->state.walk.tasks.pushChecked(md);
}

Expr *LibraryCryptoRegistry::literalMd(Expr *arg, const std::string &text, DigestUse use) {
  std::string name = llvm::StringRef(text).lower();
  const HashNameInfo *hash = HashNameTable::find(name);
  if (hash != nullptr && use == DigestUse::HMAC && hash->isAnyLength) {
    auto message = ErrorTextCryptoHash::anyLength(text);
    this->state.diagnostics.error(arg->loc(), message);

    return nullptr;
  }
  auto direct = hash != nullptr ? this->directMdIndex(hash->name) : std::nullopt;
  if (direct.has_value()) {
    return this->cryptoInt(arg->loc(), int64_t(direct.value()));
  }
  bool isTurnedOff = HashNameTable::isTurnedOff(name);
  std::string message =
      isTurnedOff ? ErrorTextCryptoHash::turnedOff(text) : ErrorTextCryptoHash::unknown(text);
  this->state.diagnostics.error(arg->loc(), message);

  return nullptr;
}

void LibraryCryptoRegistry::pushCipher(Expr *arg) {
  auto &tasks = this->state.walk.tasks;
  auto text = this->literalOf(arg);
  if (!text.has_value()) {
    return this->pushArg(arg, Type::string());
  }
  std::string name = llvm::StringRef(*text).lower();
  llvm::StringRef canonical = CipherNameTable::getCanonical(name);
  for (size_t index = 0; index < std::size(kDirectCiphers); ++index) {
    if (kDirectCiphers[index].name == canonical) {
      auto *direct = this->cryptoInt(arg->loc(), int64_t(index));

      return tasks.pushChecked(direct);
    }
  }
  if (CipherNameTable::isKnown(name)) {
    auto *known = this->cryptoString(arg->loc(), name);

    return tasks.pushChecked(known);
  }
  bool isTurnedOff = CipherNameTable::isTurnedOff(name);
  std::string message =
      isTurnedOff ? ErrorTextCryptoCipher::turnedOff(*text) : ErrorTextCryptoCipher::unknown(*text);
  this->state.diagnostics.error(arg->loc(), message);

  return tasks.pushChecked(nullptr);
}

void LibraryCryptoRegistry::pushEncoding(const CryptoValueItem &item) {
  CallExpr *call = item.call;
  size_t index = item.index;
  auto &tasks = this->state.walk.tasks;
  if (index >= call->args.size() || isa<UndefinedExpr>(call->args[index])) {
    auto *fallback = this->cryptoInt(call->loc(), item.low);

    return tasks.pushChecked(fallback);
  }
  Expr *arg = call->args[index];
  auto text = this->literalOf(arg);
  if (!text.has_value()) {
    auto message = ErrorTextCryptoData::encoding(item.what);
    tasks.pushTask(PushCheckedValue{.expr = nullptr});
    tasks.pushTask(ReportError{.loc = arg->loc(), .message = message});

    return this->pushDroppedCheck(arg);
  }
  if (CryptoChoiceTable::isBufferEncoding(*text)) {
    auto *keep = this->cryptoInt(call->loc(), -1);

    return tasks.pushChecked(keep);
  }
  tasks.pushTask(AfterCryptoEncoding{});

  return tasks.pushTask(BufferEncodingValue{.arg = arg});
}

void LibraryCryptoRegistry::afterCryptoEncoding() {
  auto &tasks = this->state.walk.tasks;
  Expr *number = tasks.takeChecked();
  Expr *answer = number->type->isError() ? nullptr : number;

  return tasks.pushChecked(answer);
}

void LibraryCryptoRegistry::pushDroppedCheck(Expr *arg) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(DiscardChecked{});

  return tasks.pushTask(CheckExpr{.expr = arg, .expected = nullptr});
}

void LibraryCryptoRegistry::cryptoFinishStep(const CryptoFinishStep &task) {
  auto values = this->takeCheckedValues(task.count);

  return this->finishCrypto(task.draft, task.finish, values);
}

void LibraryCryptoRegistry::pushCryptoResult(CallExpr *call, CryptoFn fn, std::vector<Expr *> args,
                                             const Type *type) {
  auto &tasks = this->state.walk.tasks;
  llvm::StringRef name = this->errorNameOf(fn, args);
  tasks.pushTask(FinishCryptoResult{.call = call, .fn = fn, .args = std::move(args), .type = type});

  return tasks.pushTask(NamedModuleType{.name = name});
}

void LibraryCryptoRegistry::finishCryptoResult(const FinishCryptoResult &task) {
  const Type *error = this->state.walk.tasks.takeAnswer();
  const Type *result = Type::result(task.type, error);

  return this->cryptoCall(task.call, task.fn, task.args, result);
}

void LibraryCryptoRegistry::cryptoCall(CallExpr *call, CryptoFn fn, std::vector<Expr *> args,
                                       const Type *type) {
  auto *index = this->state.ast.make<IntLitExpr>(call->loc(), uint64_t(fn));
  index->type = Type::intTy();
  args.insert(args.begin(), index);
  call->args = std::move(args);
  call->spread.clear();
  call->target = BuiltinCall{.builtin = Builtin::Crypto};
  call->type = type;
}

Expr *LibraryCryptoRegistry::cryptoText(Expr *made, Expr *encoding) {
  if (int64_t(cast<IntLitExpr>(encoding)->value) < 0) {
    return made;
  }
  if (made->type->isResult()) {
    auto *crypto = cast<CallExpr>(made);
    crypto->args.push_back(encoding);
    crypto->type = Type::result(Type::string(), made->type->errType());

    return crypto;
  }
  auto toString = BytesMethodTable::getName(BytesMethod::TO_STRING);
  auto *member = this->state.ast.make<MemberExpr>(made->loc(), made, toString);

  return this->state.library.common.builtinCall(made->loc(), member, Builtin::BufferToString,
                                                {encoding}, Type::string());
}

bool LibraryCryptoRegistry::readCryptoOptions(Expr *arg, llvm::StringRef functionName,
                                              llvm::ArrayRef<CryptoOption> allowed) {
  auto &tasks = this->state.walk.tasks;
  auto names = this->optionNames(allowed);
  auto *literal = dyn_cast<ObjectLitExpr>(arg);
  if (literal == nullptr) {
    auto message = ErrorTextCryptoOptions::literal(functionName, names[0]);
    tasks.pushTask(ReportError{.loc = arg->loc(), .message = message});
    this->pushDroppedCheck(arg);

    return false;
  }
  for (const ObjectLitExpr::Property &property : literal->props) {
    if (property.spread() || property.key() || property.method() || property.name.empty()) {
      auto message = ErrorTextCryptoOptions::byName(functionName, names[0]);
      this->state.diagnostics.error(property.loc, message);

      return false;
    }
    auto option = CryptoOptionTable::find(property.name);
    if (!option.has_value() || !llvm::is_contained(allowed, *option)) {
      auto message = ErrorTextCryptoOptions::unknown(functionName, property.name, names);
      this->state.diagnostics.error(property.loc, message);

      return false;
    }
  }

  return true;
}

Expr *LibraryCryptoRegistry::cryptoOption(const Expr *arg, CryptoOption option) const {
  const auto *literal = arg != nullptr ? dyn_cast<ObjectLitExpr>(arg) : nullptr;
  if (literal == nullptr) {
    return nullptr;
  }
  for (const ObjectLitExpr::Property &property : llvm::reverse(literal->props)) {
    if (CryptoOptionTable::find(property.name) == option) {
      return property.value;
    }
  }

  return nullptr;
}

int LibraryCryptoRegistry::cryptoChoice(Expr *arg, llvm::StringRef what, CryptoChoice choice) {
  auto &tasks = this->state.walk.tasks;
  auto values = CryptoChoiceTable::getValues(choice);
  auto text = this->literalOf(arg);
  if (!text.has_value()) {
    auto message = ErrorTextCryptoChoice::literal(what, values);
    tasks.pushTask(ReportError{.loc = arg->loc(), .message = message});
    this->pushDroppedCheck(arg);

    return -1;
  }
  for (size_t index = 0; index < values.size(); ++index) {
    if (values[index] == *text) {
      return int(index);
    }
  }
  auto message = ErrorTextCryptoChoice::unknown(what, *text, values);
  this->state.diagnostics.error(arg->loc(), message);

  return -1;
}

void LibraryCryptoRegistry::startRejectCallback(const CryptoPlanDraft &draft, size_t index,
                                                llvm::StringRef hint) {
  CallExpr *call = draft.call;
  auto &tasks = this->state.walk.tasks;
  if (index >= call->args.size()) {
    return this->continueCrypto(draft);
  }
  tasks.pushTask(AfterRejectCallback{.draft = draft, .index = index, .hint = hint.str()});

  return tasks.pushTask(CheckExpr{.expr = call->args[index], .expected = nullptr});
}

void LibraryCryptoRegistry::afterRejectCallback(const AfterRejectCallback &task) {
  CallExpr *call = task.draft.call;
  auto &tasks = this->state.walk.tasks;
  Expr *arg = tasks.takeChecked();
  if (!arg->type->nonOptional()->isFunction()) {
    return this->continueCrypto(task.draft);
  }
  auto message = ErrorTextCryptoKey::callback(task.draft.fn, task.hint);
  this->state.diagnostics.error(arg->loc(), message);
  call->type = Type::error();
  for (size_t index = call->args.size(); index > 0; --index) {
    if (index - 1 != task.index) {
      tasks.pushTask(CheckExprSlot{.slot = &call->args[index - 1], .expected = nullptr});
    }
  }
}

void LibraryCryptoRegistry::continueCrypto(const CryptoPlanDraft &draft) {
  return this->finishCrypto(draft, draft.next, {});
}

void LibraryCryptoRegistry::checkCryptoCall(const CheckCryptoCall &task) {
  CallExpr *call = task.call;
  llvm::StringRef functionName = task.functionName;
  auto &tasks = this->state.walk.tasks;
  const CryptoAsyncFunctionInfo *async = CryptoAsyncFunctionTable::find(functionName);
  bool isAsync = async != nullptr;
  llvm::StringRef sync = isAsync ? llvm::StringRef(async->syncName) : functionName;
  tasks.pushChecked(call);
  if (isAsync && !call->args.empty() && isa<FunctionExpr>(call->args.back())) {
    auto message = ErrorTextCryptoFunction::callback(functionName);
    this->state.diagnostics.error(call->args.back()->loc(), message);
    call->type = Type::error();

    return this->pushLooseArgs(call);
  }
  if (isAsync) {
    tasks.pushTask(AfterCryptoAsync{.call = call});
  }

  return this->startSyncCall(call, sync, functionName, task.loc);
}

void LibraryCryptoRegistry::afterCryptoAsync(const AfterCryptoAsync &task) {
  CallExpr *call = task.call;
  if (call->type->isError()) {
    return;
  }
  if (call->builtin() == Builtin::CryptoKeyPair) {
    call->target = BuiltinCall{.builtin = Builtin::CryptoKeyPairAsync};
    call->type = Type::promise(call->type);

    return;
  }
  if (call->builtin() != Builtin::Crypto) {
    return;
  }
  auto *index = cast<IntLitExpr>(call->args[0]);
  for (const auto &[from, to] : kAsyncCryptoFns) {
    if (CryptoFn(index->value) == from) {
      index->value = uint64_t(to);
      call->type = Type::promise(call->type);

      return;
    }
  }
}

void LibraryCryptoRegistry::startSyncCall(CallExpr *call, llvm::StringRef functionName,
                                          llvm::StringRef shown, llvm::SMLoc loc) {
  auto &tasks = this->state.walk.tasks;
  call->type = Type::error();
  if (CryptoClassTable::find(functionName).has_value()) {
    auto message = ErrorTextCryptoFunction::isClass(functionName);
    this->state.diagnostics.error(loc, message);

    return this->pushLooseArgs(call);
  }
  auto function = CryptoSyncFunctionTable::find(functionName);
  if (function == CryptoSyncFunction::CONSTANTS) {
    auto message = ErrorTextCryptoValue::constants();
    this->state.diagnostics.error(loc, message);

    return this->pushLooseArgs(call);
  }
  if (this->state.library.common.hasSpread(call)) {
    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  if (!function.has_value()) {
    auto path = KnownModules::getPath(NodeModule::Crypto);
    auto found = this->state.library.modules.checkCryptoExport(shown, path);
    std::string message = found.isErr() ? found.error().message : std::string();
    this->state.diagnostics.error(loc, message);

    return this->pushLooseArgs(call);
  }
  auto draft = this->functionDraft(call, loc);
  draft.function = *function;
  draft.fn = functionName;
  draft.shown = shown;

  return this->startKnownFunction(draft);
}

bool LibraryCryptoRegistry::hasCryptoArity(const CryptoPlanDraft &draft, unsigned min,
                                           unsigned max) {
  auto arity = this->state.library.common.checkArity(draft.call, min, max);
  if (arity.isErr()) {
    this->state.diagnostics.error(draft.loc, arity.error().message);
  }

  return arity.isOk();
}

bool LibraryCryptoRegistry::isPresent(const std::vector<Expr *> &args, size_t index) const {
  return index < args.size() && !isa<UndefinedExpr>(args[index]);
}

bool LibraryCryptoRegistry::readOptionsAt(const CryptoPlanDraft &draft, size_t index,
                                          llvm::ArrayRef<CryptoOption> allowed) {
  std::vector<Expr *> &args = draft.call->args;
  if (!this->isPresent(args, index)) {
    return true;
  }

  return this->readCryptoOptions(args[index], draft.shown, allowed);
}

Expr *LibraryCryptoRegistry::optionAt(const CryptoPlanDraft &draft, size_t index,
                                      CryptoOption option) const {
  std::vector<Expr *> &args = draft.call->args;
  if (!this->isPresent(args, index)) {
    return nullptr;
  }

  return this->cryptoOption(args[index], option);
}

void LibraryCryptoRegistry::startKnownFunction(CryptoPlanDraft draft) {
  CallExpr *call = draft.call;
  std::vector<Expr *> &args = call->args;
  switch (draft.function) {
    case CryptoSyncFunction::CONSTANTS:
      return;
    case CryptoSyncFunction::CREATE_HASH:
      if (!this->hasCryptoArity(draft, 1, 2)) {
        return;
      }
      return this->pushPlan(draft, CryptoFinish::HASH_MD, {this->mdItem(args[0], DigestUse::HASH)});
    case CryptoSyncFunction::CREATE_HMAC: {
      if (!this->hasCryptoArity(draft, 2, 2)) {
        return;
      }
      auto keyWhat = ErrorTextCryptoLabel::key();
      return this->pushPlan(draft, CryptoFinish::HMAC,
                            {this->mdItem(args[0], DigestUse::HMAC),
                             this->dataItem(args[1], keyWhat, kAnyCryptoData)});
    }
    case CryptoSyncFunction::HASH: {
      if (!this->hasCryptoArity(draft, 2, 3)) {
        return;
      }
      auto dataWhat = ErrorTextCryptoLabel::data();
      return this->pushPlan(draft, CryptoFinish::HASH_ONCE,
                            {this->mdItem(args[0], DigestUse::HASH),
                             this->dataItem(args[1], dataWhat, kTextOrViews),
                             this->encodingItem(call, 2, draft.fn, 6)});
    }
    case CryptoSyncFunction::RANDOM_BYTES:
      if (!this->hasCryptoArity(draft, 1, 2)) {
        return;
      }
      draft.next = CryptoFinish::RANDOM_BYTES_START;
      return this->startRejectCallback(draft, 1, "'const bytes = randomBytes(n)'");
    case CryptoSyncFunction::RANDOM_UUID:
      return this->checkRandomUuid(draft);
    case CryptoSyncFunction::RANDOM_INT:
      return this->startRandomInt(draft);
    case CryptoSyncFunction::RANDOM_FILL_SYNC:
    case CryptoSyncFunction::GET_RANDOM_VALUES:
      return this->startRandomFill(draft);
    case CryptoSyncFunction::TIMING_SAFE_EQUAL: {
      if (!this->hasCryptoArity(draft, 2, 2)) {
        return;
      }
      auto firstWhat = ErrorTextCryptoFnLabel::first();
      auto secondWhat = ErrorTextCryptoFnLabel::second();
      return this->pushPlan(draft, CryptoFinish::TIMING_SAFE,
                            {this->dataItem(args[0], firstWhat, kBuffersOnly),
                             this->dataItem(args[1], secondWhat, kBuffersOnly)});
    }
    case CryptoSyncFunction::PBKDF2_SYNC: {
      if (!this->hasCryptoArity(draft, 5, 5)) {
        return;
      }
      auto passwordWhat = ErrorTextCryptoLabel::password();
      auto saltWhat = ErrorTextCryptoLabel::salt();
      return this->pushPlan(draft, CryptoFinish::PBKDF2,
                            {this->dataItem(args[0], passwordWhat, kAnyCryptoData),
                             this->dataItem(args[1], saltWhat, kTextOrBuffers),
                             this->numberItem(args[2]), this->numberItem(args[3]),
                             this->mdItem(args[4], DigestUse::HMAC)});
    }
    case CryptoSyncFunction::HKDF_SYNC: {
      if (!this->hasCryptoArity(draft, 5, 5)) {
        return;
      }
      auto ikmWhat = ErrorTextCryptoLabel::ikm();
      auto saltWhat = ErrorTextCryptoLabel::salt();
      auto infoWhat = ErrorTextCryptoLabel::info();
      return this->pushPlan(
          draft, CryptoFinish::HKDF,
          {this->mdItem(args[0], DigestUse::HMAC), this->dataItem(args[1], ikmWhat, kAnyCryptoData),
           this->dataItem(args[2], saltWhat, kTextOrBuffers),
           this->dataItem(args[3], infoWhat, kTextOrBuffers), this->numberItem(args[4])});
    }
    case CryptoSyncFunction::SCRYPT_SYNC: {
      if (!this->hasCryptoArity(draft, 3, 4)) {
        return;
      }
      auto passwordWhat = ErrorTextCryptoLabel::password();
      auto saltWhat = ErrorTextCryptoLabel::salt();
      return this->pushPlan(draft, CryptoFinish::SCRYPT_OPTIONS,
                            {this->dataItem(args[0], passwordWhat, kAnyCryptoData),
                             this->dataItem(args[1], saltWhat, kTextOrBuffers),
                             this->numberItem(args[2])});
    }
    case CryptoSyncFunction::ARGON2_SYNC:
      return this->startArgon2(draft);
    case CryptoSyncFunction::CREATE_CIPHERIV:
    case CryptoSyncFunction::CREATE_DECIPHERIV: {
      if (!this->hasCryptoArity(draft, 3, 4)) {
        return;
      }
      auto keyWhat = ErrorTextCryptoLabel::key();
      auto ivWhat = ErrorTextCryptoLabel::iv();
      return this->pushPlan(draft, CryptoFinish::CIPHER_OPTIONS,
                            {this->cipherItem(args[0]),
                             this->dataItem(args[1], keyWhat, kAnyCryptoData),
                             this->dataItem(args[2], ivWhat, kTextOrBuffers)});
    }
    case CryptoSyncFunction::GET_HASHES:
    case CryptoSyncFunction::GET_CIPHERS:
    case CryptoSyncFunction::GET_CURVES:
      return this->checkNameList(draft);
    case CryptoSyncFunction::CREATE_SECRET_KEY: {
      if (!this->hasCryptoArity(draft, 1, 2)) {
        return;
      }
      auto keyWhat = ErrorTextCryptoLabel::key();
      return this->pushPlan(draft, CryptoFinish::SECRET_KEY,
                            {this->dataItem(args[0], keyWhat, kTextOrBuffers),
                             this->encodingItem(call, 1, draft.fn, 0)});
    }
    case CryptoSyncFunction::CREATE_PUBLIC_KEY:
    case CryptoSyncFunction::CREATE_PRIVATE_KEY:
      return this->startCreateKey(draft);
    case CryptoSyncFunction::GENERATE_KEY_PAIR_SYNC:
      return this->startGenerateKeyPair(draft);
    case CryptoSyncFunction::GENERATE_KEY_SYNC:
      return this->startGenerateKey(draft);
    case CryptoSyncFunction::SIGN:
    case CryptoSyncFunction::VERIFY:
      return this->startSignOnce(draft);
    case CryptoSyncFunction::CREATE_SIGN:
    case CryptoSyncFunction::CREATE_VERIFY:
      return this->startCreateSigner(draft);
    case CryptoSyncFunction::CREATE_ECDH:
      return this->startCreateEcdh(draft);
    case CryptoSyncFunction::DIFFIE_HELLMAN:
      return this->startDiffieHellman(draft);
    case CryptoSyncFunction::GENERATE_PRIME_SYNC:
      return this->startGeneratePrime(draft);
    case CryptoSyncFunction::CHECK_PRIME_SYNC:
      return this->startCheckPrime(draft);
  }
}

void LibraryCryptoRegistry::checkRandomUuid(const CryptoPlanDraft &draft) {
  auto &tasks = this->state.walk.tasks;
  if (!this->hasCryptoArity(draft, 0, 1)) {
    return;
  }
  if (!this->readOptionsAt(draft, 0, {CryptoOption::DISABLE_ENTROPY_CACHE})) {
    return;
  }
  this->cryptoCall(draft.call, CryptoFn::RandomUuid, {}, Type::string());
  Expr *cache = this->optionAt(draft, 0, CryptoOption::DISABLE_ENTROPY_CACHE);
  if (cache == nullptr) {
    return;
  }
  tasks.pushTask(DiscardChecked{});

  return this->pushArg(cache, Type::boolTy());
}

void LibraryCryptoRegistry::startRandomInt(CryptoPlanDraft draft) {
  CallExpr *call = draft.call;
  if (!this->hasCryptoArity(draft, 1, 3)) {
    return;
  }
  draft.next = CryptoFinish::RANDOM_INT_START;
  if (call->args.size() <= 1) {
    return this->continueCrypto(draft);
  }

  return this->startRejectCallback(draft, call->args.size() - 1, "'const n = randomInt(max)'");
}

void LibraryCryptoRegistry::startRandomFill(const CryptoPlanDraft &draft) {
  CallExpr *call = draft.call;
  auto &tasks = this->state.walk.tasks;
  unsigned maxArgs = draft.function == CryptoSyncFunction::RANDOM_FILL_SYNC ? 3 : 1;
  if (!this->hasCryptoArity(draft, 1, maxArgs)) {
    return;
  }
  tasks.pushTask(AfterFillTarget{.draft = draft});

  return tasks.pushTask(CheckExpr{.expr = call->args[0], .expected = nullptr});
}

void LibraryCryptoRegistry::afterFillTarget(const AfterFillTarget &task) {
  CryptoPlanDraft draft = task.draft;
  CallExpr *call = draft.call;
  auto &tasks = this->state.walk.tasks;
  Expr *target = tasks.takeChecked();
  const Type *type = target->type;
  if (type->isError()) {
    return;
  }
  auto filled = this->checkFilledTarget(type, draft.function, draft.shown);
  if (filled.isErr()) {
    return this->state.diagnostics.error(target->loc(), filled.error().message);
  }
  if (draft.function == CryptoSyncFunction::GET_RANDOM_VALUES) {
    return this->cryptoCall(call, CryptoFn::GetRandomValues, {target}, type);
  }
  bool isOffsetGiven = this->isPresent(call->args, 1);
  bool isSizeGiven = this->isPresent(call->args, 2);
  draft.option = target;
  draft.flags = (isOffsetGiven ? 1 : 0) | (isSizeGiven ? 2 : 0);
  if (!isOffsetGiven) {
    auto *zero = this->cryptoDouble(draft.loc, 0);

    return this->finishRandomFill(draft, {zero, zero});
  }
  tasks.pushTask(AfterFillOffset{.draft = draft});

  return tasks.pushTask(CheckExpr{.expr = call->args[1], .expected = Type::intTy()});
}

void LibraryCryptoRegistry::afterFillOffset(const AfterFillOffset &task) {
  const CryptoPlanDraft &draft = task.draft;
  CallExpr *call = draft.call;
  auto &tasks = this->state.walk.tasks;
  Expr *at = tasks.takeChecked();
  if (!this->isProvenFillOffset(at, draft.option)) {
    return;
  }
  bool isSizeGiven = (draft.flags & 2) != 0;
  if (!isSizeGiven) {
    auto offset = this->toDoubleItem(at);
    auto size = this->doubleLiteralItem(draft.loc, 0);

    return this->pushPlan(draft, CryptoFinish::RANDOM_FILL, {offset, size});
  }
  tasks.pushTask(AfterFillSize{.draft = draft, .at = at});

  return tasks.pushTask(CheckExpr{.expr = call->args[2], .expected = Type::intTy()});
}

void LibraryCryptoRegistry::afterFillSize(const AfterFillSize &task) {
  const CryptoPlanDraft &draft = task.draft;
  Expr *count = this->state.walk.tasks.takeChecked();
  if (!this->isProvenFillSize(count, task.at, draft.option)) {
    return;
  }

  auto offset = this->toDoubleItem(task.at);
  auto size = this->toDoubleItem(count);

  return this->pushPlan(draft, CryptoFinish::RANDOM_FILL, {offset, size});
}

bool LibraryCryptoRegistry::isProvenFillOffset(const Expr *at, const Expr *target) {
  auto &proofs = this->state.flow.proofs;
  bool isLibrary = proofs.isExempt(at->loc());
  if (at->type->isError()) {
    return false;
  }
  if (isLibrary) {
    return true;
  }
  auto offsetWhat = ErrorTextCryptoFnLabel::offset();
  auto between = proofs.requireBetween(at, 0, {}, offsetWhat);
  this->reportUnprovenBetween(between, at->loc());
  if (between.isErr()) {
    return false;
  }
  auto upTo = proofs.requireUpTo(at, target, 0, offsetWhat);
  this->reportUnprovenUpTo(upTo, at->loc());

  return upTo.isOk();
}

bool LibraryCryptoRegistry::isProvenFillSize(const Expr *count, const Expr *at,
                                             const Expr *target) {
  auto &proofs = this->state.flow.proofs;
  auto &measurements = this->state.flow.measurements;
  if (count->type->isError()) {
    return false;
  }
  bool isLibrary = proofs.isExempt(at->loc());
  if (!isLibrary) {
    auto sizeWhat = ErrorTextCryptoFnLabel::size();
    auto between = proofs.requireBetween(count, 0, {}, sizeWhat);
    this->reportUnprovenBetween(between, count->loc());
    if (between.isErr()) {
      return false;
    }
  }
  auto start = measurements.linear(at);
  auto length = measurements.linear(count);
  bool isFitting = proofs.canProveSumUpTo(start, length, target) || proofs.isExempt(count->loc());
  if (isFitting) {
    return true;
  }
  auto countText = BytesCheckHelpers::valueText(count);
  auto atText = BytesCheckHelpers::valueText(at);
  auto message = ErrorTextCryptoFunction::fillFits(countText, atText);
  this->state.diagnostics.error(count->loc(), message);

  return false;
}

void LibraryCryptoRegistry::finishRandomFill(const CryptoPlanDraft &draft,
                                             const std::vector<Expr *> &values) {
  Expr *target = draft.option;
  auto *has = this->cryptoInt(draft.loc, draft.flags);
  auto fillArgs = std::vector<Expr *>{target, values[0], values[1], has};

  return this->cryptoCall(draft.call, CryptoFn::RandomFill, std::move(fillArgs), target->type);
}

void LibraryCryptoRegistry::startArgon2(const CryptoPlanDraft &draft) {
  CallExpr *call = draft.call;
  llvm::SMLoc loc = draft.loc;
  if (!this->hasCryptoArity(draft, 2, 2)) {
    return;
  }
  if (!this->literalOf(call->args[0]).has_value()) {
    auto type = this->intLiteralItem(loc, 0xffffffff);
    auto name = this->argItem(call->args[0], Type::string());

    return this->pushPlan(draft, CryptoFinish::ARGON_OPTIONS, {type, name});
  }
  auto argon = ErrorTextCryptoFnLabel::argon();
  int which = this->cryptoChoice(call->args[0], argon, CryptoChoice::ARGON2_ALGORITHM);
  if (which < 0) {
    return;
  }
  auto *type = this->cryptoInt(loc, which);
  auto *name = this->cryptoString(loc, "");

  return this->finishCrypto(draft, CryptoFinish::ARGON_OPTIONS, {type, name});
}

bool LibraryCryptoRegistry::readArgonOptions(const CryptoPlanDraft &draft) {
  Expr *options = draft.call->args[1];
  bool isRead = this->readCryptoOptions(options, draft.shown,
                                        {CryptoOption::MESSAGE, CryptoOption::NONCE,
                                         CryptoOption::PARALLELISM, CryptoOption::TAG_LENGTH,
                                         CryptoOption::MEMORY, CryptoOption::PASSES,
                                         CryptoOption::SECRET, CryptoOption::ASSOCIATED_DATA});
  if (!isRead) {
    return false;
  }
  for (CryptoOption required :
       {CryptoOption::MESSAGE, CryptoOption::NONCE, CryptoOption::PARALLELISM,
        CryptoOption::TAG_LENGTH, CryptoOption::MEMORY, CryptoOption::PASSES}) {
    if (this->cryptoOption(options, required) == nullptr) {
      auto name = CryptoOptionTable::getName(required);
      auto message = ErrorTextCryptoFunction::needs(draft.shown, name);
      this->state.diagnostics.error(options->loc(), message);

      return false;
    }
  }

  return true;
}

void LibraryCryptoRegistry::checkNameList(const CryptoPlanDraft &draft) {
  if (!this->hasCryptoArity(draft, 0, 0)) {
    return;
  }
  CryptoFn fn = draft.function == CryptoSyncFunction::GET_HASHES    ? CryptoFn::GetHashes
                : draft.function == CryptoSyncFunction::GET_CIPHERS ? CryptoFn::GetCiphers
                                                                    : CryptoFn::GetCurves;
  const Type *names = Type::array(Type::string());

  return this->cryptoCall(draft.call, fn, {}, names);
}

void LibraryCryptoRegistry::startCreateKey(CryptoPlanDraft draft) {
  CallExpr *call = draft.call;
  Expr *given = call->args[0];
  if (!this->hasCryptoArity(draft, 1, 1)) {
    return;
  }
  bool isPriv = draft.function == CryptoSyncFunction::CREATE_PRIVATE_KEY;
  draft.choice = 0;
  draft.flags = 0;
  draft.option = this->cryptoString(draft.loc, "");
  if (!isa<ObjectLitExpr>(given)) {
    auto keyWhat = ErrorTextCryptoLabel::key();
    auto kinds = isPriv ? kTextOrBuffers : kAnyCryptoData;
    auto key = this->dataItem(given, keyWhat, kinds);
    auto passphrase = this->undefinedItem(draft.loc);
    auto encoding = this->intLiteralItem(draft.loc, 0);

    return this->pushPlan(draft, CryptoFinish::CREATE_KEY, {key, passphrase, encoding});
  }
  bool isRead =
      this->readCryptoOptions(given, draft.fn,
                              {CryptoOption::KEY, CryptoOption::FORMAT, CryptoOption::TYPE,
                               CryptoOption::PASSPHRASE, CryptoOption::ENCODING});
  if (!isRead) {
    return;
  }
  Expr *format = this->cryptoOption(given, CryptoOption::FORMAT);
  if (format != nullptr) {
    auto keyFormat = ErrorTextCryptoFnLabel::keyFormat();
    draft.choice = this->cryptoChoice(format, keyFormat, CryptoChoice::KEY_FORMAT);
    if (draft.choice < 0) {
      return;
    }
  }
  Expr *key = this->cryptoOption(given, CryptoOption::KEY);
  if (key == nullptr) {
    auto message = ErrorTextCryptoOptions::needsKey(draft.fn);

    return this->state.diagnostics.error(given->loc(), message);
  }
  bool isJwk = draft.choice == int(CryptoKeyFormat::JWK);
  auto keyWhat = CryptoOptionTable::getName(CryptoOption::KEY);
  auto kinds = isPriv ? kTextOrBuffers : kAnyCryptoData;
  auto item = isJwk ? this->argItem(key, Type::json()) : this->dataItem(key, keyWhat, kinds);

  return this->pushPlan(draft, CryptoFinish::KEY_DETAILS, {item});
}

void LibraryCryptoRegistry::startGenerateKey(CryptoPlanDraft draft) {
  CallExpr *call = draft.call;
  std::vector<Expr *> &args = call->args;
  if (!this->hasCryptoArity(draft, 2, 2)) {
    return;
  }
  auto keyType = ErrorTextCryptoFnLabel::keyType();
  int which = this->cryptoChoice(args[0], keyType, CryptoChoice::SECRET_KEY_TYPE);
  if (which < 0 || !this->readCryptoOptions(args[1], draft.shown, {CryptoOption::LENGTH})) {
    return;
  }
  Expr *length = this->cryptoOption(args[1], CryptoOption::LENGTH);
  if (length == nullptr) {
    auto message = ErrorTextCryptoFunction::keyLength(draft.shown);

    return this->state.diagnostics.error(args[1]->loc(), message);
  }
  draft.choice = which;

  auto bits = this->checkedItem(length, Type::intTy());

  return this->pushPlan(draft, CryptoFinish::GENERATE_KEY, {bits});
}

void LibraryCryptoRegistry::startSignOnce(CryptoPlanDraft draft) {
  bool isVerify = draft.function == CryptoSyncFunction::VERIFY;
  size_t count = isVerify ? 4 : 3;
  if (!this->hasCryptoArity(draft, unsigned(count), unsigned(count + 1))) {
    return;
  }
  draft.next = CryptoFinish::SIGN_ONCE_START;
  llvm::StringRef hint = isVerify ? "'const ok = verify(algorithm, data, key, signature)'"
                                  : "'const signature = sign(algorithm, data, key)'";

  return this->startRejectCallback(draft, count, hint);
}

void LibraryCryptoRegistry::startCreateSigner(const CryptoPlanDraft &draft) {
  CallExpr *call = draft.call;
  if (!this->hasCryptoArity(draft, 1, 1)) {
    return;
  }
  Expr *name = call->args[0];
  auto text = this->literalOf(name);
  if (text.has_value() && this->literalMd(name, *text, DigestUse::HMAC) == nullptr) {
    return;
  }
  bool isVerify = draft.function == CryptoSyncFunction::CREATE_VERIFY;
  auto result = draft;
  result.fnCode = CryptoFn::SignNew;
  result.resultType = Type::cryptoType(isVerify ? Crypto::Verify : Crypto::Sign);
  auto nameItem = this->argItem(name, Type::string());
  auto verifying = this->boolLiteralItem(draft.loc, isVerify);

  return this->pushPlan(result, CryptoFinish::RESULT, {nameItem, verifying});
}

void LibraryCryptoRegistry::startCreateEcdh(const CryptoPlanDraft &draft) {
  CallExpr *call = draft.call;
  if (!this->hasCryptoArity(draft, 1, 1)) {
    return;
  }
  auto curve = this->literalOf(call->args[0]);
  auto known = curve.has_value() ? this->checkCurveIsKnown(*curve) : ok(llvm::StringRef());
  if (known.isErr()) {
    this->state.diagnostics.error(call->args[0]->loc(), known.error().message);

    return this->pushLooseArgs(call);
  }
  auto result = draft;
  result.fnCode = CryptoFn::EcdhNew;
  result.resultType = Type::cryptoType(Crypto::Ecdh);

  auto curve = this->argItem(call->args[0], Type::string());

  return this->pushPlan(result, CryptoFinish::RESULT, {curve});
}

void LibraryCryptoRegistry::startDiffieHellman(const CryptoPlanDraft &draft) {
  CallExpr *call = draft.call;
  if (!this->hasCryptoArity(draft, 1, 1)) {
    return;
  }
  Expr *options = call->args[0];
  bool isRead = this->readCryptoOptions(options, draft.fn,
                                        {CryptoOption::PRIVATE_KEY, CryptoOption::PUBLIC_KEY});
  if (!isRead) {
    return;
  }
  Expr *privateKey = this->cryptoOption(options, CryptoOption::PRIVATE_KEY);
  Expr *publicKey = this->cryptoOption(options, CryptoOption::PUBLIC_KEY);
  if (privateKey == nullptr || publicKey == nullptr) {
    auto message = ErrorTextCryptoFunction::dhKeys();

    return this->state.diagnostics.error(options->loc(), message);
  }
  auto privateWhat = ErrorTextCryptoFnLabel::privateKey();
  auto publicWhat = ErrorTextCryptoFnLabel::publicKey();
  auto privateItem = this->keyObjectItem(privateKey, privateWhat);
  auto publicItem = this->keyObjectItem(publicKey, publicWhat);

  return this->pushPlan(draft, CryptoFinish::DIFFIE_HELLMAN, {privateItem, publicItem});
}

void LibraryCryptoRegistry::startGeneratePrime(const CryptoPlanDraft &draft) {
  CallExpr *call = draft.call;
  llvm::SMLoc loc = draft.loc;
  if (!this->hasCryptoArity(draft, 1, 2)) {
    return;
  }
  bool isRead = this->readOptionsAt(
      draft, 1,
      {CryptoOption::ADDEND, CryptoOption::REMAINDER, CryptoOption::SAFE, CryptoOption::BIGINT});
  if (!isRead) {
    return;
  }
  Expr *big = this->optionAt(draft, 1, CryptoOption::BIGINT);
  if (big != nullptr) {
    auto message = ErrorTextCryptoFunction::bigint();

    return this->state.diagnostics.error(big->loc(), message);
  }
  Expr *safe = this->optionAt(draft, 1, CryptoOption::SAFE);
  Expr *add = this->optionAt(draft, 1, CryptoOption::ADDEND);
  Expr *rem = this->optionAt(draft, 1, CryptoOption::REMAINDER);
  auto addName = CryptoOptionTable::getName(CryptoOption::ADDEND);
  auto remName = CryptoOptionTable::getName(CryptoOption::REMAINDER);
  auto size = this->numberItem(call->args[0]);
  auto safeItem =
      safe != nullptr ? this->argItem(safe, Type::boolTy()) : this->boolLiteralItem(loc, false);
  auto addItem =
      add != nullptr ? this->dataItem(add, addName, kBuffersOnly) : this->undefinedItem(loc);
  auto remItem =
      rem != nullptr ? this->dataItem(rem, remName, kBuffersOnly) : this->undefinedItem(loc);

  return this->pushPlan(draft, CryptoFinish::GENERATE_PRIME, {size, safeItem, addItem, remItem});
}

void LibraryCryptoRegistry::startCheckPrime(const CryptoPlanDraft &draft) {
  CallExpr *call = draft.call;
  llvm::SMLoc loc = draft.loc;
  if (!this->hasCryptoArity(draft, 1, 2)) {
    return;
  }
  if (!this->readOptionsAt(draft, 1, {CryptoOption::CHECKS})) {
    return;
  }
  auto candidateWhat = ErrorTextCryptoLabel::candidate();
  Expr *checks = this->optionAt(draft, 1, CryptoOption::CHECKS);
  auto candidate = this->dataItem(call->args[0], candidateWhat, kBuffersOnly);
  auto checksItem = checks != nullptr ? this->numberItem(checks) : this->doubleLiteralItem(loc, 0);

  return this->pushPlan(draft, CryptoFinish::CHECK_PRIME, {candidate, checksItem});
}

void LibraryCryptoRegistry::startSignKey(CryptoPlanDraft draft, std::vector<Expr *> prefix,
                                         Expr *keyArg, bool isPriv, CryptoFinish after) {
  llvm::SMLoc loc = keyArg->loc();
  draft.option = keyArg;
  draft.next = after;
  draft.choice = int(prefix.size());
  draft.flags = 0;
  auto items = this->literalItems(prefix);
  if (!isa<ObjectLitExpr>(keyArg)) {
    auto keyWhat = isPriv ? ErrorTextCryptoLabel::privateKey() : ErrorTextCryptoLabel::key();
    auto key = this->dataItem(keyArg, keyWhat, kAnyCryptoData);
    items.push_back(key);
    items.push_back(this->intLiteralItem(loc, 0));
    items.push_back(this->intLiteralItem(loc, 0));

    return this->pushPlan(draft, CryptoFinish::SIGN_KEY_READY, items);
  }
  bool isRead = this->readCryptoOptions(keyArg, draft.fn,
                                        {CryptoOption::KEY, CryptoOption::PADDING,
                                         CryptoOption::SALT_LENGTH, CryptoOption::DSA_ENCODING});
  if (!isRead) {
    return this->failCrypto(draft);
  }
  Expr *key = this->cryptoOption(keyArg, CryptoOption::KEY);
  if (key == nullptr) {
    auto message = ErrorTextCryptoOptions::needsKey(draft.fn);
    this->state.diagnostics.error(keyArg->loc(), message);

    return this->failCrypto(draft);
  }
  auto keyWhat = ErrorTextCryptoLabel::key();
  Expr *padding = this->cryptoOption(keyArg, CryptoOption::PADDING);
  Expr *salt = this->cryptoOption(keyArg, CryptoOption::SALT_LENGTH);
  draft.flags = (padding != nullptr ? 1 : 0) | (salt != nullptr ? 2 : 0);
  auto keyItem = this->dataItem(key, keyWhat, kAnyCryptoData);
  auto paddingItem =
      padding != nullptr ? this->argItem(padding, Type::intTy()) : this->intLiteralItem(loc, 0);
  auto saltItem =
      salt != nullptr ? this->argItem(salt, Type::intTy()) : this->intLiteralItem(loc, 0);
  items.push_back(keyItem);
  items.push_back(paddingItem);
  items.push_back(saltItem);

  return this->pushPlan(draft, CryptoFinish::SIGN_KEY_READY, items);
}

void LibraryCryptoRegistry::finishSignKey(const CryptoPlanDraft &draft,
                                          const std::vector<Expr *> &values) {
  Expr *keyArg = draft.option;
  llvm::SMLoc loc = keyArg->loc();
  size_t prefixCount = size_t(draft.choice);
  int p1363 = 0;
  Expr *dsa = this->cryptoOption(keyArg, CryptoOption::DSA_ENCODING);
  if (dsa != nullptr) {
    auto signature = ErrorTextCryptoLabel::signatureEncoding();
    p1363 = this->cryptoChoice(dsa, signature, CryptoChoice::DSA_ENCODING);
    if (p1363 < 0) {
      return this->failCrypto(draft);
    }
  }
  Expr *key = values[prefixCount];
  if (key->type->isError()) {
    return this->failCrypto(draft);
  }
  auto signArgs = values;
  signArgs.push_back(this->cryptoInt(loc, p1363));
  signArgs.push_back(this->cryptoInt(loc, draft.flags));

  return this->finishCrypto(draft, draft.next, signArgs);
}

void LibraryCryptoRegistry::failCrypto(const CryptoPlanDraft &draft) {
  if (!draft.isMethod) {
    return;
  }

  return this->state.walk.tasks.pushChecked(draft.call);
}

void LibraryCryptoRegistry::startGenerateKeyPair(const CryptoPlanDraft &draft) {
  CallExpr *call = draft.call;
  llvm::SMLoc loc = draft.loc;
  if (!this->hasCryptoArity(draft, 1, 2)) {
    return;
  }
  auto kind = draft;
  kind.choice = -1;
  if (!this->literalOf(call->args[0]).has_value()) {
    auto name = this->argItem(call->args[0], Type::string());

    return this->pushPlan(kind, CryptoFinish::PAIR_OPTIONS, {name});
  }
  auto label = ErrorTextKeyPairLabel::pairType();
  kind.choice = this->cryptoChoice(call->args[0], label, CryptoChoice::KEY_PAIR_TYPE);
  if (kind.choice < 0) {
    return;
  }
  auto *name = this->cryptoString(loc, "");

  return this->finishCrypto(kind, CryptoFinish::PAIR_OPTIONS, {name});
}

bool LibraryCryptoRegistry::readKeyPairOptions(const CryptoPlanDraft &draft) {
  CallExpr *call = draft.call;
  bool isRead =
      call->args.size() <= 1 ||
      this->readCryptoOptions(call->args[1], draft.shown,
                              {CryptoOption::MODULUS_LENGTH, CryptoOption::PUBLIC_EXPONENT,
                               CryptoOption::NAMED_CURVE, CryptoOption::PUBLIC_KEY_ENCODING,
                               CryptoOption::PRIVATE_KEY_ENCODING, CryptoOption::DIVISOR_LENGTH,
                               CryptoOption::HASH_ALGORITHM, CryptoOption::MGF1_HASH_ALGORITHM,
                               CryptoOption::SALT_LENGTH});
  if (!isRead) {
    return false;
  }
  for (CryptoOption unsupported : {CryptoOption::HASH_ALGORITHM, CryptoOption::MGF1_HASH_ALGORITHM,
                                   CryptoOption::SALT_LENGTH}) {
    Expr *expr = this->pairOption(draft, unsupported);
    if (expr != nullptr) {
      auto name = CryptoOptionTable::getName(unsupported);
      auto message = ErrorTextKeyPair::option(name);
      this->state.diagnostics.error(expr->loc(), message);

      return false;
    }
  }

  return true;
}

Expr *LibraryCryptoRegistry::pairOption(const CryptoPlanDraft &draft, CryptoOption option) const {
  CallExpr *call = draft.call;
  Expr *options = call->args.size() > 1 ? call->args[1] : nullptr;

  return this->cryptoOption(options, option);
}

void LibraryCryptoRegistry::finishPairOptions(const CryptoPlanDraft &draft,
                                              const std::vector<Expr *> &values) {
  llvm::SMLoc loc = draft.loc;
  if (!this->readKeyPairOptions(draft)) {
    return;
  }
  auto types = CryptoChoiceTable::getValues(CryptoChoice::KEY_PAIR_TYPE);
  int which = draft.choice;
  llvm::StringRef typeName = which >= 0 ? types[which] : llvm::StringRef();
  auto given = this->checkKeyOptions(which, typeName, draft);
  if (given.isErr()) {
    return this->state.diagnostics.error(loc, given.error().message);
  }
  Expr *modulus = this->pairOption(draft, CryptoOption::MODULUS_LENGTH);
  auto name = this->literalItem(values[0]);
  auto modulusItem =
      modulus != nullptr ? this->numberItem(modulus) : this->doubleLiteralItem(loc, 0);

  return this->pushPlan(draft, CryptoFinish::PAIR_MODULUS, {name, modulusItem});
}

void LibraryCryptoRegistry::finishPairModulus(const CryptoPlanDraft &draft,
                                              const std::vector<Expr *> &values) {
  llvm::SMLoc loc = draft.loc;
  int which = draft.choice;
  Expr *modulusOption = this->pairOption(draft, CryptoOption::MODULUS_LENGTH);
  if (modulusOption != nullptr) {
    auto bits = this->knownNumber(values[1]);
    bool isRsa = which == int(KeyPairType::RSA) || which == int(KeyPairType::RSA_PSS);
    if (isRsa && bits.has_value() && *bits < 512) {
      auto shown = this->numberText(*bits);
      auto message = ErrorTextKeyPair::small(shown);

      return this->state.diagnostics.error(modulusOption->loc(), message);
    }
  }
  Expr *exponent = this->pairOption(draft, CryptoOption::PUBLIC_EXPONENT);
  Expr *divisor = this->pairOption(draft, CryptoOption::DIVISOR_LENGTH);
  Expr *curve = this->pairOption(draft, CryptoOption::NAMED_CURVE);
  auto items = this->literalItems(values);
  auto exponentItem =
      exponent != nullptr ? this->numberItem(exponent) : this->doubleLiteralItem(loc, 65537);
  auto divisorItem = divisor != nullptr ? this->numberItem(divisor) : this->undefinedItem(loc);
  auto curveItem =
      curve != nullptr ? this->argItem(curve, Type::string()) : this->stringLiteralItem(loc, "");
  items.push_back(exponentItem);
  items.push_back(divisorItem);
  items.push_back(curveItem);

  return this->pushPlan(draft, CryptoFinish::PAIR_NUMBERS, items);
}

void LibraryCryptoRegistry::finishPairNumbers(CryptoPlanDraft draft,
                                              const std::vector<Expr *> &values) {
  CallExpr *call = draft.call;
  llvm::SMLoc loc = draft.loc;
  int has = (this->pairOption(draft, CryptoOption::NAMED_CURVE) != nullptr ? 1 : 0) |
            (this->pairOption(draft, CryptoOption::PUBLIC_EXPONENT) != nullptr ? 2 : 0);
  auto *generate = this->state.ast.make<CallExpr>(loc, call->callee, std::vector<Expr *>{});
  auto *which = this->cryptoInt(loc, draft.choice < 0 ? 0xffffffff : draft.choice);
  auto *hasBits = this->cryptoInt(loc, has);
  auto generateArgs =
      std::vector<Expr *>{which, values[0], values[1], values[2], hasBits, values[4]};
  this->cryptoCall(generate, CryptoFn::GenerateKeyPair, std::move(generateArgs),
                   Type::cryptoType(Crypto::KeyObject));
  auto publicEncoding = this->readKeyEncoding(draft, false);
  if (!publicEncoding.has_value()) {
    return;
  }
  auto privateEncoding = this->readKeyEncoding(draft, true);
  if (!privateEncoding.has_value()) {
    return;
  }
  auto items = std::vector<CryptoValueItem>{this->literalItem(generate)};
  this->addEncodingItems(*publicEncoding, items);
  this->addEncodingItems(*privateEncoding, items);
  draft.choice = publicEncoding->format;
  draft.flags = privateEncoding->format;

  return this->pushPlan(draft, CryptoFinish::PAIR_FINISH, items);
}

void LibraryCryptoRegistry::addEncodingItems(const KeyEncodingDraft &encoding,
                                             std::vector<CryptoValueItem> &items) {
  llvm::SMLoc loc = encoding.loc;
  auto *format = this->cryptoInt(loc, encoding.format < 0 ? 0xffffffff : encoding.format);
  auto *flags = this->cryptoInt(loc, encoding.flags);
  auto passphraseName = CryptoOptionTable::getName(CryptoOption::PASSPHRASE);
  auto cipher = encoding.cipher != nullptr ? this->argItem(encoding.cipher, Type::string())
                                           : this->stringLiteralItem(loc, "");
  auto passphrase = encoding.passphrase != nullptr
                        ? this->dataItem(encoding.passphrase, passphraseName, kTextOrBuffers)
                        : this->undefinedItem(loc);
  items.push_back(this->literalItem(format));
  items.push_back(this->literalItem(flags));
  items.push_back(this->literalItem(encoding.type));
  items.push_back(cipher);
  items.push_back(passphrase);
}

std::optional<KeyEncodingDraft> LibraryCryptoRegistry::readKeyEncoding(const CryptoPlanDraft &draft,
                                                                       bool isPrivate) {
  llvm::SMLoc loc = draft.loc;
  CryptoOption option =
      isPrivate ? CryptoOption::PRIVATE_KEY_ENCODING : CryptoOption::PUBLIC_KEY_ENCODING;
  auto optionName = CryptoOptionTable::getName(option);
  Expr *encoding = this->pairOption(draft, option);
  auto chosen = KeyEncodingDraft{
      .loc = loc,
      .format = -1,
      .flags = 0,
      .type = this->cryptoString(loc, ""),
      .cipher = nullptr,
      .passphrase = nullptr,
  };
  if (encoding == nullptr) {
    return chosen;
  }
  auto allowed = llvm::SmallVector<CryptoOption, 4>{CryptoOption::TYPE, CryptoOption::FORMAT};
  if (isPrivate) {
    allowed.append({CryptoOption::CIPHER, CryptoOption::PASSPHRASE});
  }
  if (!this->readCryptoOptions(encoding, optionName, allowed)) {
    return std::nullopt;
  }
  Expr *format = this->cryptoOption(encoding, CryptoOption::FORMAT);
  if (format == nullptr) {
    auto message = ErrorTextKeyPair::noFormat(optionName);
    this->state.diagnostics.error(encoding->loc(), message);

    return std::nullopt;
  }
  auto formatLabel = ErrorTextKeyPairLabel::format();
  chosen.format = this->cryptoChoice(format, formatLabel, CryptoChoice::KEY_FORMAT);
  if (chosen.format < 0) {
    return std::nullopt;
  }
  Expr *type = this->cryptoOption(encoding, CryptoOption::TYPE);
  bool isTypeNeeded = type != nullptr || chosen.format != int(CryptoKeyFormat::JWK);
  if (isTypeNeeded && type == nullptr) {
    auto message = ErrorTextKeyPair::noType(optionName, !isPrivate);
    this->state.diagnostics.error(encoding->loc(), message);

    return std::nullopt;
  }
  if (type != nullptr) {
    auto typeLabel = ErrorTextKeyPairLabel::keyType();
    int typeIndex = this->cryptoChoice(type, typeLabel, CryptoChoice::KEY_ENCODING_TYPE);
    if (typeIndex < 0) {
      return std::nullopt;
    }
    auto names = CryptoChoiceTable::getValues(CryptoChoice::KEY_ENCODING_TYPE);
    chosen.type = this->cryptoString(type->loc(), names[typeIndex]);
    chosen.flags |= 1;
  }
  chosen.cipher = this->cryptoOption(encoding, CryptoOption::CIPHER);
  chosen.passphrase = this->cryptoOption(encoding, CryptoOption::PASSPHRASE);
  chosen.flags |= (chosen.cipher != nullptr ? 2 : 0) | (chosen.passphrase != nullptr ? 4 : 0);
  if (chosen.cipher != nullptr && chosen.passphrase == nullptr) {
    auto &tasks = this->state.walk.tasks;
    auto message = ErrorTextKeyPair::passphrase();
    tasks.pushTask(ReportError{.loc = encoding->loc(), .message = message});
    tasks.pushTask(DiscardChecked{});
    this->pushArg(chosen.cipher, Type::string());

    return std::nullopt;
  }

  return chosen;
}

void LibraryCryptoRegistry::finishPair(const CryptoPlanDraft &draft,
                                       const std::vector<Expr *> &values) {
  CallExpr *call = draft.call;
  Expr *passphrase = values.back();
  if (passphrase->type->isError()) {
    return;
  }
  call->args = values;
  call->spread.clear();
  call->target = BuiltinCall{.builtin = Builtin::CryptoKeyPair};
  const Type *publicKey = this->encodedKeyType(draft.choice);
  const Type *privateKey = this->encodedKeyType(draft.flags);
  auto publicName = KeyPairShape::getFieldName(KeyPairField::PUBLIC_KEY);
  auto privateName = KeyPairShape::getFieldName(KeyPairField::PRIVATE_KEY);
  const auto *pair =
      Type::object({{publicName.str(), publicKey, false}, {privateName.str(), privateKey, false}},
                   KeyPairShape::getTypeName());
  auto errorName = CryptoErrorTypes::callErrorName(CryptoFn::GenerateKeyPair);
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(FinishKeyPair{.call = call, .pair = pair});

  return tasks.pushTask(NamedModuleType{.name = *errorName});
}

void LibraryCryptoRegistry::finishKeyPair(const FinishKeyPair &task) {
  const Type *error = this->state.walk.tasks.takeAnswer();
  task.call->type = Type::result(task.pair, error);
}

void LibraryCryptoRegistry::finishCrypto(const CryptoPlanDraft &draft, CryptoFinish finish,
                                         const std::vector<Expr *> &values) {
  switch (finish) {
    case CryptoFinish::CALL:
      return this->finishCall(draft, values);
    case CryptoFinish::RESULT:
      return this->finishResult(draft, values);
    case CryptoFinish::CHECKED_RESULT:
      if (values[0]->type->isError()) {
        return this->failCrypto(draft);
      }
      return this->finishResult(draft, values);
    case CryptoFinish::HASH_MD:
      return this->finishHashMd(draft, values);
    case CryptoFinish::HMAC:
      return this->finishHmac(draft, values);
    case CryptoFinish::HASH_ONCE:
      return this->finishHashOnce(draft, values);
    case CryptoFinish::RANDOM_BYTES_START:
      return this->continueRandomBytes(draft);
    case CryptoFinish::RANDOM_BYTES:
      return this->finishRandomBytes(draft, values);
    case CryptoFinish::RANDOM_INT_START:
      return this->continueRandomInt(draft);
    case CryptoFinish::RANDOM_INT:
      return this->finishRandomInt(draft, values);
    case CryptoFinish::RANDOM_FILL:
      return this->finishRandomFill(draft, values);
    case CryptoFinish::TIMING_SAFE:
      return this->finishTimingSafe(draft, values);
    case CryptoFinish::PBKDF2:
      return this->finishPbkdf2(draft, values);
    case CryptoFinish::HKDF:
      return this->finishHkdf(draft, values);
    case CryptoFinish::SCRYPT_OPTIONS:
      return this->finishScryptOptions(draft, values);
    case CryptoFinish::SCRYPT_COST:
      return this->finishScryptCost(draft, values);
    case CryptoFinish::ARGON_OPTIONS:
      return this->finishArgonOptions(draft, values);
    case CryptoFinish::ARGON:
      return this->finishArgon(draft, values);
    case CryptoFinish::CIPHER_OPTIONS:
      return this->finishCipherOptions(draft, values);
    case CryptoFinish::SECRET_KEY:
      return this->finishSecretKey(draft, values);
    case CryptoFinish::KEY_DETAILS:
      return this->finishKeyDetails(draft, values);
    case CryptoFinish::CREATE_KEY:
      return this->finishCreateKey(draft, values);
    case CryptoFinish::GENERATE_KEY:
      return this->finishGenerateKey(draft, values);
    case CryptoFinish::SIGN_ONCE_START:
      return this->continueSignOnce(draft);
    case CryptoFinish::SIGN_ONCE_KEY:
      return this->startSignOnceKey(draft, values);
    case CryptoFinish::SIGN_KEY_READY:
      return this->finishSignKey(draft, values);
    case CryptoFinish::SIGN_ONCE_DONE:
      return this->finishSignOnceKey(draft, values);
    case CryptoFinish::SIGN_ONCE_FINISH:
      return this->finishSignOnce(draft, values);
    case CryptoFinish::DIFFIE_HELLMAN:
      return this->finishDiffieHellman(draft, values);
    case CryptoFinish::GENERATE_PRIME:
      return this->finishGeneratePrime(draft, values);
    case CryptoFinish::CHECK_PRIME:
      return this->finishCheckPrime(draft, values);
    case CryptoFinish::PAIR_OPTIONS:
      return this->finishPairOptions(draft, values);
    case CryptoFinish::PAIR_MODULUS:
      return this->finishPairModulus(draft, values);
    case CryptoFinish::PAIR_NUMBERS:
      return this->finishPairNumbers(draft, values);
    case CryptoFinish::PAIR_FINISH:
      return this->finishPair(draft, values);
    case CryptoFinish::METHOD_DATA_ENCODING:
      return this->finishDataEncoding(draft, values);
    case CryptoFinish::METHOD_DIGEST:
      return this->finishDigest(draft, values);
    case CryptoFinish::METHOD_SIGN:
      return this->startBytesOrText(draft, values, 1, true);
    case CryptoFinish::METHOD_VERIFY_KEY:
      return this->continueMethodVerify(draft, values);
    case CryptoFinish::METHOD_VERIFY:
      return this->finishMethodVerify(draft, values);
    case CryptoFinish::METHOD_SET_AAD:
      return this->finishSetAad(draft, values);
    case CryptoFinish::METHOD_EXPORT:
      return this->finishEncodedExport(draft, values);
    case CryptoFinish::METHOD_KEY_EQUALS:
      return this->finishKeyEquals(draft, values);
    case CryptoFinish::METHOD_X509_NAME:
      return this->finishX509Name(draft, values);
    case CryptoFinish::METHOD_X509_VERIFY:
      return this->finishX509Verify(draft, values);
    case CryptoFinish::BYTES_OR_TEXT:
      return this->finishBytesOrText(draft, values);
    case CryptoFinish::SUBTLE_INTRINSIC:
      return this->finishSubtleIntrinsic(draft, values);
  }
}

void LibraryCryptoRegistry::finishCall(const CryptoPlanDraft &draft,
                                       const std::vector<Expr *> &values) {
  this->cryptoCall(draft.call, draft.fnCode, values, draft.resultType);
  if (!draft.isMethod) {
    return;
  }

  return this->state.walk.tasks.pushChecked(draft.call);
}

void LibraryCryptoRegistry::finishResult(const CryptoPlanDraft &draft,
                                         const std::vector<Expr *> &values) {
  if (draft.isMethod) {
    this->state.walk.tasks.pushChecked(draft.call);
  }

  return this->pushCryptoResult(draft.call, draft.fnCode, values, draft.resultType);
}

CryptoPlanDraft LibraryCryptoRegistry::withResult(const CryptoPlanDraft &draft, CryptoFn fn,
                                                  const Type *type) const {
  CryptoPlanDraft result = draft;
  result.fnCode = fn;
  result.resultType = type;

  return result;
}

std::vector<CryptoValueItem>
LibraryCryptoRegistry::literalItems(const std::vector<Expr *> &values) const {
  auto items = std::vector<CryptoValueItem>();
  for (Expr *value : values) {
    items.push_back(this->literalItem(value));
  }

  return items;
}

void LibraryCryptoRegistry::finishHashMd(const CryptoPlanDraft &draft,
                                         const std::vector<Expr *> &values) {
  Expr *md = values[0];
  if (!this->readOptionsAt(draft, 1, {CryptoOption::OUTPUT_LENGTH}) || md == nullptr) {
    return;
  }
  Expr *length = this->optionAt(draft, 1, CryptoOption::OUTPUT_LENGTH);
  auto result = this->withResult(draft, CryptoFn::HashNew, Type::cryptoType(Crypto::Hash));
  auto mdItem = this->literalItem(md);
  auto lengthItem =
      length != nullptr ? this->numberItem(length) : this->doubleLiteralItem(draft.loc, -1);

  return this->pushPlan(result, CryptoFinish::RESULT, {mdItem, lengthItem});
}

void LibraryCryptoRegistry::finishHmac(const CryptoPlanDraft &draft,
                                       const std::vector<Expr *> &values) {
  Expr *md = values[0];
  Expr *key = values[1];
  if (md == nullptr || key->type->isError()) {
    return;
  }
  auto *zero = this->cryptoInt(draft.loc, 0);
  auto hmacArgs = std::vector<Expr *>{md, key, zero};

  return this->pushCryptoResult(draft.call, CryptoFn::HmacNew, std::move(hmacArgs),
                                Type::cryptoType(Crypto::Hmac));
}

void LibraryCryptoRegistry::finishHashOnce(const CryptoPlanDraft &draft,
                                           const std::vector<Expr *> &values) {
  Expr *md = values[0];
  Expr *data = values[1];
  Expr *encoding = values[2];
  if (md == nullptr || data->type->isError() || encoding == nullptr) {
    return;
  }
  if (int64_t(cast<IntLitExpr>(encoding)->value) < 0) {
    const auto *buffer = Type::bytesType(Type::Bytes::Buffer);

    return this->pushCryptoResult(draft.call, CryptoFn::HashOnce, {md, data}, buffer);
  }

  return this->pushCryptoResult(draft.call, CryptoFn::HashOnceText, {md, data, encoding},
                                Type::string());
}

void LibraryCryptoRegistry::continueRandomBytes(const CryptoPlanDraft &draft) {
  auto size = this->provenItem(draft.call->args[0], 0, 2147483647, "the size");

  return this->pushPlan(draft, CryptoFinish::RANDOM_BYTES, {size});
}

void LibraryCryptoRegistry::finishRandomBytes(const CryptoPlanDraft &draft,
                                              const std::vector<Expr *> &values) {
  Expr *size = values[0];
  if (size == nullptr) {
    return;
  }
  const auto *buffer = Type::bytesType(Type::Bytes::Buffer);

  return this->cryptoCall(draft.call, CryptoFn::RandomBytes, {size}, buffer);
}

void LibraryCryptoRegistry::continueRandomInt(const CryptoPlanDraft &draft) {
  CallExpr *call = draft.call;
  std::vector<Expr *> &args = call->args;
  if (args.size() == 3) {
    auto message = ErrorTextCryptoFunction::randomArgs();
    this->state.diagnostics.error(args[2]->loc(), message);

    return this->pushLooseArgs(call);
  }
  auto minItem = args.size() == 2 ? this->checkedItem(args[0], Type::intTy())
                                  : this->intLiteralItem(draft.loc, 0);
  auto maxItem = this->checkedItem(args.back(), Type::intTy());

  return this->pushPlan(draft, CryptoFinish::RANDOM_INT, {minItem, maxItem});
}

void LibraryCryptoRegistry::finishRandomInt(const CryptoPlanDraft &draft,
                                            const std::vector<Expr *> &values) {
  CallExpr *call = draft.call;
  Expr *min = values[0];
  Expr *max = values[1];
  if (min->type->isError() || max->type->isError()) {
    return;
  }
  for (Expr *bound : values) {
    if (!bound->type->isInt()) {
      auto message = ErrorTextCryptoFunction::randomInt(bound->type->name());

      return this->state.diagnostics.error(bound->loc(), message);
    }
  }
  auto greater = ErrorTextCryptoFunction::greater(call->args.size() == 2);
  auto less = this->state.flow.proofs.requireLess(min, max, greater);
  if (less.isErr()) {
    return this->state.diagnostics.error(max->loc(), less.error().message);
  }

  return this->cryptoCall(call, CryptoFn::RandomInt32, {min, max}, Type::intTy());
}

void LibraryCryptoRegistry::finishTimingSafe(const CryptoPlanDraft &draft,
                                             const std::vector<Expr *> &values) {
  if (values[0]->type->isError() || values[1]->type->isError()) {
    return;
  }

  return this->cryptoCall(draft.call, CryptoFn::TimingSafeEqual, values, Type::boolTy());
}

void LibraryCryptoRegistry::finishPbkdf2(const CryptoPlanDraft &draft,
                                         const std::vector<Expr *> &values) {
  Expr *password = values[0];
  Expr *salt = values[1];
  Expr *md = values[4];
  auto iterationsWhat = ErrorTextCryptoLabel::iterations();
  auto keylenWhat = ErrorTextCryptoLabel::keylen();
  bool isFailed = md == nullptr || password->type->isError() || salt->type->isError() ||
                  !this->matchWhole(values[2], iterationsWhat, 1) ||
                  !this->matchWhole(values[3], keylenWhat, 1);
  if (isFailed) {
    return;
  }
  const auto *buffer = Type::bytesType(Type::Bytes::Buffer);

  return this->pushCryptoResult(draft.call, CryptoFn::Pbkdf2, values, buffer);
}

void LibraryCryptoRegistry::finishHkdf(const CryptoPlanDraft &draft,
                                       const std::vector<Expr *> &values) {
  auto keylenWhat = ErrorTextCryptoLabel::keylen();
  bool isFailed = values[0] == nullptr || values[1]->type->isError() ||
                  values[2]->type->isError() || values[3]->type->isError() ||
                  !this->matchWhole(values[4], keylenWhat, 1);
  if (isFailed) {
    return;
  }
  const auto *arrayBuffer = Type::bytesType(Type::Bytes::ArrayBuffer);

  return this->pushCryptoResult(draft.call, CryptoFn::Hkdf, values, arrayBuffer);
}

void LibraryCryptoRegistry::finishScryptOptions(const CryptoPlanDraft &draft,
                                                const std::vector<Expr *> &values) {
  bool isRead =
      this->readOptionsAt(draft, 3,
                          {CryptoOption::SCRYPT_N, CryptoOption::SCRYPT_R, CryptoOption::SCRYPT_P,
                           CryptoOption::MAX_MEMORY, CryptoOption::COST, CryptoOption::BLOCK_SIZE,
                           CryptoOption::PARALLELIZATION});
  if (!isRead) {
    return;
  }
  static const std::pair<CryptoOption, CryptoOption> pairs[] = {
      {CryptoOption::SCRYPT_N, CryptoOption::COST},
      {CryptoOption::SCRYPT_R, CryptoOption::BLOCK_SIZE},
      {CryptoOption::SCRYPT_P, CryptoOption::PARALLELIZATION}};
  auto items = this->literalItems(values);
  for (const auto &[first, second] : pairs) {
    Expr *a = this->optionAt(draft, 3, first);
    Expr *b = this->optionAt(draft, 3, second);
    if (a != nullptr && b != nullptr) {
      auto firstName = CryptoOptionTable::getName(first);
      auto secondName = CryptoOptionTable::getName(second);
      auto message = ErrorTextCryptoFunction::together(firstName, secondName);

      return this->reportAfterValues(items, values.size(), b->loc(), message);
    }
    Expr *given = a != nullptr ? a : b;
    auto item = given != nullptr ? this->numberItem(given) : this->doubleLiteralItem(draft.loc, 0);
    items.push_back(item);
  }

  return this->pushPlan(draft, CryptoFinish::SCRYPT_COST, items);
}

void LibraryCryptoRegistry::reportAfterValues(const std::vector<CryptoValueItem> &items,
                                              size_t skipped, llvm::SMLoc loc,
                                              const std::string &message) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(ReportError{.loc = loc, .message = message});
  for (size_t index = items.size(); index > skipped; --index) {
    tasks.pushTask(DiscardChecked{});
    tasks.pushTask(CryptoValue{.item = items[index - 1]});
  }
}

void LibraryCryptoRegistry::finishScryptCost(const CryptoPlanDraft &draft,
                                             const std::vector<Expr *> &values) {
  Expr *costValue = values[3];
  auto cost = this->knownNumber(costValue);
  if (cost.has_value()) {
    auto n = int64_t(*cost);
    bool isBad = *cost != 0 && (double(n) != *cost || n < 2 || (n & (n - 1)) != 0);
    if (isBad) {
      auto costShown = this->numberText(*cost);
      auto message = ErrorTextCryptoFunction::costN(costShown);

      return this->state.diagnostics.error(costValue->loc(), message);
    }
  }
  Expr *maxmem = this->optionAt(draft, 3, CryptoOption::MAX_MEMORY);
  if (values[0]->type->isError() || values[1]->type->isError()) {
    return;
  }
  auto result = this->withResult(draft, CryptoFn::Scrypt, Type::bytesType(Type::Bytes::Buffer));
  auto items = this->literalItems(values);
  auto maxmemItem =
      maxmem != nullptr ? this->numberItem(maxmem) : this->doubleLiteralItem(draft.loc, 0);
  items.push_back(maxmemItem);

  return this->pushPlan(result, CryptoFinish::RESULT, items);
}

void LibraryCryptoRegistry::finishArgonOptions(const CryptoPlanDraft &draft,
                                               const std::vector<Expr *> &values) {
  if (!this->readArgonOptions(draft)) {
    return;
  }
  Expr *options = draft.call->args[1];
  auto items = this->literalItems(values);
  for (CryptoOption option : {CryptoOption::MESSAGE, CryptoOption::NONCE}) {
    Expr *given = this->cryptoOption(options, option);
    auto name = CryptoOptionTable::getName(option);
    items.push_back(this->dataItem(given, name, kTextOrBuffers));
  }
  for (CryptoOption option : {CryptoOption::PARALLELISM, CryptoOption::TAG_LENGTH,
                              CryptoOption::MEMORY, CryptoOption::PASSES}) {
    Expr *given = this->cryptoOption(options, option);
    items.push_back(this->numberItem(given));
  }
  for (CryptoOption option : {CryptoOption::SECRET, CryptoOption::ASSOCIATED_DATA}) {
    Expr *given = this->cryptoOption(options, option);
    auto name = CryptoOptionTable::getName(option);
    auto item = given != nullptr ? this->dataItem(given, name, kTextOrBuffers)
                                 : this->undefinedItem(draft.loc);
    items.push_back(item);
  }

  return this->pushPlan(draft, CryptoFinish::ARGON, items);
}

void LibraryCryptoRegistry::finishArgon(const CryptoPlanDraft &draft,
                                        const std::vector<Expr *> &values) {
  for (size_t index : {size_t(2), size_t(3), size_t(8), size_t(9)}) {
    if (values[index]->type->isError()) {
      return;
    }
  }
  const auto *buffer = Type::bytesType(Type::Bytes::Buffer);

  return this->pushCryptoResult(draft.call, CryptoFn::Argon2, values, buffer);
}

void LibraryCryptoRegistry::finishCipherOptions(const CryptoPlanDraft &draft,
                                                const std::vector<Expr *> &values) {
  CallExpr *call = draft.call;
  Expr *cipher = values[0];
  Expr *key = values[1];
  Expr *iv = values[2];
  bool isFailed = !this->readOptionsAt(draft, 3, {CryptoOption::AUTH_TAG_LENGTH}) ||
                  cipher == nullptr || key->type->isError() || iv->type->isError() ||
                  !this->matchCipherLengths(cipher, key, iv);
  if (isFailed) {
    return;
  }
  auto name = this->literalOf(call->args[0]);
  Expr *tag = this->optionAt(draft, 3, CryptoOption::AUTH_TAG_LENGTH);
  if (tag == nullptr && name.has_value() && CipherNameTable::needsAuthTagLength(*name)) {
    auto message = ErrorTextCryptoFunction::tagLength(*name);

    return this->state.diagnostics.error(call->args[0]->loc(), message);
  }
  bool isDecrypt = draft.function == CryptoSyncFunction::CREATE_DECIPHERIV;
  const Type *made = Type::cryptoType(isDecrypt ? Crypto::Decipheriv : Crypto::Cipheriv);
  auto result = this->withResult(draft, CryptoFn::CipherNew, made);
  auto items = this->literalItems(values);
  auto tagItem = tag != nullptr ? this->numberItem(tag) : this->doubleLiteralItem(draft.loc, -1);
  items.push_back(tagItem);
  items.push_back(this->boolLiteralItem(draft.loc, isDecrypt));

  return this->pushPlan(result, CryptoFinish::RESULT, items);
}

void LibraryCryptoRegistry::finishSecretKey(const CryptoPlanDraft &draft,
                                            const std::vector<Expr *> &values) {
  Expr *key = values[0];
  Expr *encoding = values[1];
  if (key->type->isError() || encoding == nullptr) {
    return;
  }
  if (int64_t(cast<IntLitExpr>(encoding)->value) < 0) {
    encoding = this->cryptoInt(draft.loc, 0);
  }
  const auto *keyObject = Type::cryptoType(Crypto::KeyObject);

  return this->cryptoCall(draft.call, CryptoFn::SecretKey, {key, encoding}, keyObject);
}

void LibraryCryptoRegistry::finishKeyDetails(CryptoPlanDraft draft,
                                             const std::vector<Expr *> &values) {
  Expr *given = draft.call->args[0];
  Expr *type = this->cryptoOption(given, CryptoOption::TYPE);
  if (type != nullptr) {
    auto keyType = ErrorTextCryptoFnLabel::keyType();
    int which = this->cryptoChoice(type, keyType, CryptoChoice::KEY_ENCODING_TYPE);
    if (which < 0) {
      return;
    }
    auto types = CryptoChoiceTable::getValues(CryptoChoice::KEY_ENCODING_TYPE);
    draft.option = this->cryptoString(type->loc(), types[which]);
    draft.flags |= 1;
  }
  Expr *passphrase = this->cryptoOption(given, CryptoOption::PASSPHRASE);
  Expr *encoding = this->cryptoOption(given, CryptoOption::ENCODING);
  auto passphraseName = CryptoOptionTable::getName(CryptoOption::PASSPHRASE);
  auto items = this->literalItems(values);
  if (passphrase != nullptr) {
    draft.flags |= 2;
    items.push_back(this->dataItem(passphrase, passphraseName, kTextOrBuffers));
  } else {
    items.push_back(this->undefinedItem(draft.loc));
  }
  if (encoding == nullptr) {
    items.push_back(this->intLiteralItem(draft.loc, 0));
  } else if (this->literalOf(encoding).has_value()) {
    items.push_back(this->bufferEncodingItem(encoding));
  } else {
    items.push_back(this->literalItem(nullptr));
  }

  return this->pushPlan(draft, CryptoFinish::CREATE_KEY, items);
}

void LibraryCryptoRegistry::finishCreateKey(const CryptoPlanDraft &draft,
                                            const std::vector<Expr *> &values) {
  CallExpr *call = draft.call;
  llvm::SMLoc loc = draft.loc;
  Expr *key = values[0];
  Expr *passphrase = values[1];
  Expr *encoding = values[2];
  Expr *given = call->args[0];
  Expr *encodingOption = this->cryptoOption(given, CryptoOption::ENCODING);
  bool isBadEncoding =
      encodingOption != nullptr && (encoding == nullptr || !isa<IntLitExpr>(encoding));
  if (isBadEncoding) {
    auto message = ErrorTextCryptoFunction::keyCoding();

    return this->state.diagnostics.error(encodingOption->loc(), message);
  }
  if (key->type->isError() || passphrase->type->isError()) {
    return;
  }
  bool isPriv = draft.function == CryptoSyncFunction::CREATE_PRIVATE_KEY;
  auto *priv = this->cryptoBool(loc, isPriv);
  auto *format = this->cryptoInt(loc, draft.choice);
  auto *has = this->cryptoInt(loc, draft.flags);
  auto keyArgs = std::vector<Expr *>{priv, key, encoding, format, has, draft.option, passphrase};
  const auto *keyObject = Type::cryptoType(Crypto::KeyObject);

  return this->pushCryptoResult(call, CryptoFn::CreateKey, std::move(keyArgs), keyObject);
}

void LibraryCryptoRegistry::finishGenerateKey(const CryptoPlanDraft &draft,
                                              const std::vector<Expr *> &values) {
  auto &proofs = this->state.flow.proofs;
  Expr *bits = values[0];
  if (bits->type->isError()) {
    return;
  }
  bool isAes = draft.choice == int(SecretKeyType::AES);
  if (isAes) {
    auto known = this->state.code.constants.intConstant(bits);
    bool isAesLength = known.has_value() && (*known == 128 || *known == 192 || *known == 256);
    if (!isAesLength) {
      auto message = ErrorTextCryptoFunction::aesLength();

      return this->state.diagnostics.error(bits->loc(), message);
    }
  } else {
    auto keyBits = ErrorTextCryptoFnLabel::keyBits();
    auto between = proofs.requireBetween(bits, 8, {}, keyBits);
    this->reportUnprovenBetween(between, bits->loc());
    if (between.isErr() && !proofs.isExempt(bits->loc())) {
      return;
    }
  }
  auto result =
      this->withResult(draft, CryptoFn::GenerateSecretKey, Type::cryptoType(Crypto::KeyObject));

  auto aes = this->boolLiteralItem(draft.loc, isAes);
  auto length = this->toDoubleItem(bits);

  return this->pushPlan(result, CryptoFinish::CALL, {aes, length});
}

void LibraryCryptoRegistry::continueSignOnce(const CryptoPlanDraft &draft) {
  std::vector<Expr *> &args = draft.call->args;
  auto dataWhat = ErrorTextCryptoLabel::data();
  auto algorithm = isa<UndefinedExpr>(args[0]) ? this->undefinedItem(draft.loc)
                                               : this->argItem(args[0], Type::string());

  auto data = this->dataItem(args[1], dataWhat, kTextOrBuffers);

  return this->pushPlan(draft, CryptoFinish::SIGN_ONCE_KEY, {algorithm, data});
}

void LibraryCryptoRegistry::startSignOnceKey(const CryptoPlanDraft &draft,
                                             const std::vector<Expr *> &values) {
  Expr *keyArg = draft.call->args[2];
  bool isPriv = draft.function != CryptoSyncFunction::VERIFY;

  return this->startSignKey(draft, values, keyArg, isPriv, CryptoFinish::SIGN_ONCE_DONE);
}

void LibraryCryptoRegistry::finishSignOnceKey(const CryptoPlanDraft &draft,
                                              const std::vector<Expr *> &values) {
  bool isVerify = draft.function == CryptoSyncFunction::VERIFY;
  if (isVerify) {
    auto signatureWhat = ErrorTextCryptoLabel::signature();
    auto items = this->literalItems(values);
    auto signature = this->dataItem(draft.call->args[3], signatureWhat, kTextOrBuffers);
    items.push_back(signature);

    return this->pushPlan(draft, CryptoFinish::SIGN_ONCE_FINISH, items);
  }
  if (values[1]->type->isError()) {
    return;
  }
  const auto *buffer = Type::bytesType(Type::Bytes::Buffer);

  return this->pushCryptoResult(draft.call, CryptoFn::Sign, values, buffer);
}

void LibraryCryptoRegistry::finishSignOnce(const CryptoPlanDraft &draft,
                                           const std::vector<Expr *> &values) {
  Expr *data = values[1];
  Expr *signature = values.back();
  if (signature->type->isError() || data->type->isError()) {
    return;
  }
  auto signArgs = std::vector<Expr *>(values.begin(), values.end() - 1);
  signArgs.insert(signArgs.begin() + 3, signature);

  return this->pushCryptoResult(draft.call, CryptoFn::Verify, std::move(signArgs), Type::boolTy());
}

void LibraryCryptoRegistry::finishDiffieHellman(const CryptoPlanDraft &draft,
                                                const std::vector<Expr *> &values) {
  if (values[0]->type->isError() || values[1]->type->isError()) {
    return;
  }
  const auto *buffer = Type::bytesType(Type::Bytes::Buffer);

  return this->pushCryptoResult(draft.call, CryptoFn::DiffieHellman, values, buffer);
}

void LibraryCryptoRegistry::finishGeneratePrime(const CryptoPlanDraft &draft,
                                                const std::vector<Expr *> &values) {
  Expr *size = values[0];
  auto sizeWhat = ErrorTextCryptoLabel::size();
  bool isFailed = size->type->isError() || values[1]->type->isError() ||
                  values[2]->type->isError() || values[3]->type->isError() ||
                  !this->matchWhole(size, sizeWhat, 1);
  if (isFailed) {
    return;
  }
  const auto *arrayBuffer = Type::bytesType(Type::Bytes::ArrayBuffer);

  return this->pushCryptoResult(draft.call, CryptoFn::GeneratePrime, values, arrayBuffer);
}

void LibraryCryptoRegistry::finishCheckPrime(const CryptoPlanDraft &draft,
                                             const std::vector<Expr *> &values) {
  Expr *checks = values[1];
  auto checksWhat = CryptoOptionTable::getName(CryptoOption::CHECKS);
  bool isFailed = values[0]->type->isError() || checks->type->isError() ||
                  !this->matchWhole(checks, checksWhat, 0);
  if (isFailed) {
    return;
  }

  return this->pushCryptoResult(draft.call, CryptoFn::CheckPrime, values, Type::boolTy());
}

void LibraryCryptoRegistry::checkCryptoMethod(const CheckCryptoMethod &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  call->type = Type::error();
  if (this->state.library.common.hasSpread(call)) {
    tasks.pushChecked(call);

    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  Crypto kind = member->object->type->crypto();
  const auto *found = CryptoMethodTable::find(kind, member->name);
  if (found == nullptr) {
    return this->reportNoMethod(call, member);
  }
  auto draft = this->functionDraft(call, member->loc());
  draft.member = member;
  draft.method = found->method;
  draft.fn = member->name;
  draft.shown = member->name;
  draft.isMethod = true;

  return this->startCryptoMethod(draft);
}

void LibraryCryptoRegistry::reportNoMethod(CallExpr *call, MemberExpr *member) {
  auto &tasks = this->state.walk.tasks;
  const Type *type = member->object->type;
  Crypto kind = type->crypto();
  llvm::StringRef name = member->name;
  bool isHash = kind == Crypto::Hash || kind == Crypto::Hmac || kind == Crypto::Sign ||
                kind == Crypto::Verify;
  bool isCipher = kind == Crypto::Cipheriv || kind == Crypto::Decipheriv;
  std::string message;
  if ((isHash || isCipher) && CryptoStreamMethodTable::isStreamMethod(name)) {
    message = ErrorTextCryptoMethod::stream(name, type->name(), kind, isCipher);
  } else {
    auto methods = ErrorTextCryptoMethods::of(kind);
    message = ErrorTextCryptoMethod::noMethod(name, type->name(), methods);
  }
  this->state.diagnostics.error(member->loc(), message);
  tasks.pushChecked(call);

  return this->pushLooseArgs(call);
}

bool LibraryCryptoRegistry::hasMethodArity(const CryptoPlanDraft &draft, unsigned min,
                                           unsigned max) {
  bool isMatching = this->hasCryptoArity(draft, min, max);
  if (!isMatching) {
    this->state.walk.tasks.pushChecked(draft.call);
  }

  return isMatching;
}

void LibraryCryptoRegistry::startCryptoMethod(const CryptoPlanDraft &draft) {
  switch (draft.method) {
    case CryptoMethod::HASH_UPDATE:
    case CryptoMethod::CIPHER_UPDATE:
    case CryptoMethod::SET_AUTH_TAG:
    case CryptoMethod::ECDH_SET_PRIVATE_KEY:
    case CryptoMethod::ECDH_COMPUTE_SECRET:
      return this->startDataEncoding(draft);
    case CryptoMethod::DIGEST:
      return this->startDigest(draft);
    case CryptoMethod::HASH_COPY:
      return this->startHashCopy(draft);
    case CryptoMethod::SIGN:
    case CryptoMethod::VERIFY:
      return this->startMethodSign(draft);
    case CryptoMethod::CIPHER_FINAL:
    case CryptoMethod::ECDH_GET_PRIVATE_KEY:
      return this->startFinalBytes(draft);
    case CryptoMethod::SET_AAD:
      return this->startSetAad(draft);
    case CryptoMethod::GET_AUTH_TAG:
    case CryptoMethod::X509_PEM:
      return this->startSelfOnly(draft);
    case CryptoMethod::SET_AUTO_PADDING:
      return this->startAutoPadding(draft);
    case CryptoMethod::KEY_EXPORT:
      return this->startKeyExport(draft);
    case CryptoMethod::KEY_EQUALS:
      return this->startKeyEquals(draft);
    case CryptoMethod::ECDH_GENERATE_KEYS:
    case CryptoMethod::ECDH_GET_PUBLIC_KEY:
      return this->startEcdhPublicKey(draft);
    case CryptoMethod::X509_CHECK_HOST:
    case CryptoMethod::X509_CHECK_EMAIL:
    case CryptoMethod::X509_CHECK_IP:
      return this->startX509Name(draft);
    case CryptoMethod::X509_CHECK_ISSUED:
    case CryptoMethod::X509_CHECK_PRIVATE_KEY:
    case CryptoMethod::X509_VERIFY:
      return this->startX509Verify(draft);
  }
}

void LibraryCryptoRegistry::startDataEncoding(const CryptoPlanDraft &draft) {
  CallExpr *call = draft.call;
  std::vector<Expr *> &args = call->args;
  llvm::StringRef name = draft.member->name;
  bool isHash = draft.method == CryptoMethod::HASH_UPDATE;
  bool isTwoAtMost = isHash || draft.method == CryptoMethod::SET_AUTH_TAG ||
                     draft.method == CryptoMethod::ECDH_SET_PRIVATE_KEY;
  unsigned most = isTwoAtMost ? 2 : 3;
  if (!this->hasMethodArity(draft, 1, most)) {
    return;
  }
  auto kinds = isHash ? kTextOrViews : kTextOrBuffers;
  std::string what = ErrorTextCryptoLabel::data();
  if (draft.method == CryptoMethod::SET_AUTH_TAG) {
    what = ErrorTextCryptoLabel::tag();
  } else if (draft.method == CryptoMethod::ECDH_SET_PRIVATE_KEY) {
    what = ErrorTextCryptoLabel::privateKey();
  } else if (draft.method == CryptoMethod::ECDH_COMPUTE_SECRET) {
    what = ErrorTextCryptoLabel::publicKey();
  }
  auto data = this->dataItem(args[0], what, kinds);
  auto encoding = this->encodingItem(call, 1, name, 0);

  return this->pushPlan(draft, CryptoFinish::METHOD_DATA_ENCODING, {data, encoding});
}

void LibraryCryptoRegistry::finishDataEncoding(CryptoPlanDraft draft,
                                               const std::vector<Expr *> &values) {
  Expr *self = draft.member->object;
  Expr *data = values[0];
  Expr *encoding = values[1];
  if (data->type->isError() || encoding == nullptr) {
    return this->failCrypto(draft);
  }
  if (int64_t(cast<IntLitExpr>(encoding)->value) < 0) {
    encoding = this->cryptoInt(draft.loc, 0);
  }
  auto fnArgs = std::vector<Expr *>{self, data, encoding};
  const auto *buffer = Type::bytesType(Type::Bytes::Buffer);
  bool isHash = draft.method == CryptoMethod::HASH_UPDATE;
  if (isHash || draft.method == CryptoMethod::SET_AUTH_TAG) {
    draft.fnCode = isHash ? CryptoFn::HashUpdate : CryptoFn::CipherSetAuthTag;
    draft.resultType = self->type;

    return this->finishCall(draft, fnArgs);
  }
  if (draft.method == CryptoMethod::ECDH_SET_PRIVATE_KEY) {
    draft.fnCode = CryptoFn::EcdhSetPrivateKey;
    draft.resultType = Type::voidTy();

    return this->finishResult(draft, fnArgs);
  }
  bool isCipher = draft.method == CryptoMethod::CIPHER_UPDATE;
  draft.fnCode = isCipher ? CryptoFn::CipherUpdate : CryptoFn::EcdhComputeSecret;
  draft.resultType = buffer;

  return this->startBytesOrText(draft, fnArgs, 2, !isCipher);
}

void LibraryCryptoRegistry::startDigest(const CryptoPlanDraft &draft) {
  if (!this->hasMethodArity(draft, 0, 1)) {
    return;
  }
  auto encoding = this->encodingItem(draft.call, 0, draft.member->name, -1);

  return this->pushPlan(draft, CryptoFinish::METHOD_DIGEST, {encoding});
}

void LibraryCryptoRegistry::finishDigest(CryptoPlanDraft draft, const std::vector<Expr *> &values) {
  Expr *self = draft.member->object;
  Expr *encoding = values[0];
  if (encoding == nullptr) {
    return this->failCrypto(draft);
  }
  if (int64_t(cast<IntLitExpr>(encoding)->value) < 0) {
    draft.fnCode = CryptoFn::HashDigest;
    draft.resultType = Type::bytesType(Type::Bytes::Buffer);

    return this->finishCall(draft, {self});
  }
  draft.fnCode = CryptoFn::HashDigestText;
  draft.resultType = Type::string();

  return this->finishCall(draft, {self, encoding});
}

void LibraryCryptoRegistry::startMethodSign(CryptoPlanDraft draft) {
  Expr *self = draft.member->object;
  Expr *key = draft.call->args.empty() ? nullptr : draft.call->args[0];
  bool isSign = draft.method == CryptoMethod::SIGN;
  unsigned least = isSign ? 1 : 2;
  if (!this->hasMethodArity(draft, least, least + 1)) {
    return;
  }
  if (!isSign) {
    return this->startSignKey(draft, {self}, key, false, CryptoFinish::METHOD_VERIFY_KEY);
  }
  draft.fnCode = CryptoFn::SignFinal;
  draft.resultType = Type::bytesType(Type::Bytes::Buffer);

  return this->startSignKey(draft, {self}, key, true, CryptoFinish::METHOD_SIGN);
}

void LibraryCryptoRegistry::startFinalBytes(CryptoPlanDraft draft) {
  Expr *self = draft.member->object;
  if (!this->hasMethodArity(draft, 0, 1)) {
    return;
  }
  bool isCipher = draft.method == CryptoMethod::CIPHER_FINAL;
  draft.fnCode = isCipher ? CryptoFn::CipherFinal : CryptoFn::EcdhGetPrivateKey;
  draft.resultType = Type::bytesType(Type::Bytes::Buffer);

  return this->startBytesOrText(draft, {self}, 0, true);
}

void LibraryCryptoRegistry::startSetAad(const CryptoPlanDraft &draft) {
  if (!this->hasMethodArity(draft, 1, 2)) {
    return;
  }
  auto aadWhat = ErrorTextCryptoLabel::aad();
  auto data = this->dataItem(draft.call->args[0], aadWhat, kBuffersOnly);

  return this->pushPlan(draft, CryptoFinish::METHOD_SET_AAD, {data});
}

void LibraryCryptoRegistry::startSelfOnly(CryptoPlanDraft draft) {
  Expr *self = draft.member->object;
  if (!this->hasMethodArity(draft, 0, 0)) {
    return;
  }
  if (draft.method == CryptoMethod::X509_PEM) {
    draft.fnCode = CryptoFn::X509Pem;
    draft.resultType = Type::string();

    return this->finishCall(draft, {self});
  }
  draft.fnCode = CryptoFn::CipherGetAuthTag;
  draft.resultType = Type::bytesType(Type::Bytes::Buffer);

  return this->finishResult(draft, {self});
}

void LibraryCryptoRegistry::startAutoPadding(CryptoPlanDraft draft) {
  Expr *self = draft.member->object;
  std::vector<Expr *> &args = draft.call->args;
  if (!this->hasMethodArity(draft, 0, 1)) {
    return;
  }
  draft.fnCode = CryptoFn::CipherSetAutoPadding;
  draft.resultType = self->type;
  auto padding = this->isPresent(args, 0) ? this->argItem(args[0], Type::boolTy())
                                          : this->boolLiteralItem(draft.loc, true);
  auto selfItem = this->literalItem(self);

  return this->pushPlan(draft, CryptoFinish::CALL, {selfItem, padding});
}

void LibraryCryptoRegistry::startKeyEquals(const CryptoPlanDraft &draft) {
  if (!this->hasMethodArity(draft, 1, 1)) {
    return;
  }
  auto otherWhat = ErrorTextCryptoLabel::otherKey();
  auto other = this->keyObjectItem(draft.call->args[0], otherWhat);

  return this->pushPlan(draft, CryptoFinish::METHOD_KEY_EQUALS, {other});
}

void LibraryCryptoRegistry::startX509Name(const CryptoPlanDraft &draft) {
  unsigned most = draft.method == CryptoMethod::X509_CHECK_IP ? 1 : 2;
  if (!this->hasMethodArity(draft, 1, most)) {
    return;
  }
  auto value = this->argItem(draft.call->args[0], Type::string());

  return this->pushPlan(draft, CryptoFinish::METHOD_X509_NAME, {value});
}

void LibraryCryptoRegistry::startX509Verify(const CryptoPlanDraft &draft) {
  Expr *given = draft.call->args.empty() ? nullptr : draft.call->args[0];
  if (!this->hasMethodArity(draft, 1, 1)) {
    return;
  }
  bool isIssued = draft.method == CryptoMethod::X509_CHECK_ISSUED;
  auto keyWhat = ErrorTextCryptoLabel::key();
  auto other = isIssued ? this->checkedItem(given, nullptr) : this->keyObjectItem(given, keyWhat);

  return this->pushPlan(draft, CryptoFinish::METHOD_X509_VERIFY, {other});
}

void LibraryCryptoRegistry::startHashCopy(const CryptoPlanDraft &draft) {
  CallExpr *call = draft.call;
  Expr *self = draft.member->object;
  if (!this->hasMethodArity(draft, 0, 1)) {
    return;
  }
  bool isGiven = this->isPresent(call->args, 0);
  bool isRead =
      !isGiven || this->readCryptoOptions(call->args[0], draft.fn, {CryptoOption::OUTPUT_LENGTH});
  if (!isRead) {
    return this->failCrypto(draft);
  }
  Expr *length = this->optionAt(draft, 0, CryptoOption::OUTPUT_LENGTH);
  auto result = this->withResult(draft, CryptoFn::HashCopy, self->type);
  if (length != nullptr) {
    auto selfItem = this->literalItem(self);
    auto lengthItem = this->numberItem(length);

    return this->pushPlan(result, CryptoFinish::RESULT, {selfItem, lengthItem});
  }
  auto *noLength = this->cryptoDouble(draft.loc, -1);

  return this->finishCall(result, {self, noLength});
}

void LibraryCryptoRegistry::continueMethodVerify(const CryptoPlanDraft &draft,
                                                 const std::vector<Expr *> &values) {
  CallExpr *call = draft.call;
  auto signatureWhat = ErrorTextCryptoLabel::signature();
  auto items = this->literalItems(values);
  auto signature = this->dataItem(call->args[1], signatureWhat, kTextOrBuffers);
  auto encoding = this->encodingItem(call, 2, draft.member->name, 0);
  items.push_back(signature);
  items.push_back(encoding);

  return this->pushPlan(draft, CryptoFinish::METHOD_VERIFY, items);
}

void LibraryCryptoRegistry::finishMethodVerify(const CryptoPlanDraft &draft,
                                               const std::vector<Expr *> &values) {
  size_t count = values.size();
  Expr *signature = values[count - 2];
  Expr *encoding = values[count - 1];
  if (signature->type->isError() || encoding == nullptr) {
    return this->failCrypto(draft);
  }
  if (int64_t(cast<IntLitExpr>(encoding)->value) < 0) {
    encoding = this->cryptoInt(draft.loc, 0);
  }
  auto verifyArgs = std::vector<Expr *>(values.begin(), values.end() - 2);
  verifyArgs.insert(verifyArgs.begin() + 2, {signature, encoding});
  auto result = this->withResult(draft, CryptoFn::VerifyFinal, Type::boolTy());

  return this->finishResult(result, verifyArgs);
}

void LibraryCryptoRegistry::finishSetAad(const CryptoPlanDraft &draft,
                                         const std::vector<Expr *> &values) {
  CallExpr *call = draft.call;
  Expr *self = draft.member->object;
  Expr *data = values[0];
  bool isGiven = this->isPresent(call->args, 1);
  bool isRead = !isGiven ||
                this->readCryptoOptions(call->args[1], draft.fn, {CryptoOption::PLAINTEXT_LENGTH});
  if (!isRead || data->type->isError()) {
    return this->failCrypto(draft);
  }
  Expr *length = this->optionAt(draft, 1, CryptoOption::PLAINTEXT_LENGTH);
  auto result = this->withResult(draft, CryptoFn::CipherSetAad, self->type);
  auto selfItem = this->literalItem(self);
  auto dataItem = this->literalItem(data);
  auto lengthItem =
      length != nullptr ? this->numberItem(length) : this->doubleLiteralItem(draft.loc, -1);

  return this->pushPlan(result, CryptoFinish::CALL, {selfItem, dataItem, lengthItem});
}

void LibraryCryptoRegistry::startKeyExport(const CryptoPlanDraft &draft) {
  CallExpr *call = draft.call;
  if (!this->hasMethodArity(draft, 0, 1)) {
    return;
  }
  if (!this->isPresent(call->args, 0)) {
    return this->finishKeyExport(draft, int(CryptoKeyFormat::BUFFER));
  }
  Expr *options = call->args[0];
  bool isRead = this->readCryptoOptions(
      options, draft.fn,
      {CryptoOption::FORMAT, CryptoOption::TYPE, CryptoOption::CIPHER, CryptoOption::PASSPHRASE});
  if (!isRead) {
    return this->failCrypto(draft);
  }
  int format = int(CryptoKeyFormat::BUFFER);
  Expr *formatOption = this->cryptoOption(options, CryptoOption::FORMAT);
  if (formatOption != nullptr) {
    auto keyFormat = ErrorTextCryptoFnLabel::keyFormat();
    format = this->cryptoChoice(formatOption, keyFormat, CryptoChoice::EXPORT_FORMAT);
    if (format < 0) {
      return this->failCrypto(draft);
    }
  }
  bool isEncoded = format != int(CryptoKeyFormat::JWK) && format != int(CryptoKeyFormat::BUFFER);
  if (!isEncoded) {
    return this->finishKeyExport(draft, format);
  }

  return this->startEncodedExport(draft, options, format);
}

void LibraryCryptoRegistry::finishKeyExport(CryptoPlanDraft draft, int format) {
  Expr *self = draft.member->object;
  bool isJwk = format == int(CryptoKeyFormat::JWK);
  draft.fnCode = isJwk ? CryptoFn::KeyExportJwk : CryptoFn::KeyExportSecret;
  draft.resultType = isJwk ? Type::json() : Type::bytesType(Type::Bytes::Buffer);

  return this->finishResult(draft, {self});
}

void LibraryCryptoRegistry::startEncodedExport(CryptoPlanDraft draft, Expr *options, int format) {
  llvm::SMLoc loc = draft.loc;
  draft.flags = 0;
  draft.option = this->cryptoString(loc, "");
  draft.choice = format;
  Expr *typeOption = this->cryptoOption(options, CryptoOption::TYPE);
  if (typeOption != nullptr) {
    auto typeWhat = ErrorTextCryptoFnLabel::keyType();
    int which = this->cryptoChoice(typeOption, typeWhat, CryptoChoice::KEY_ENCODING_TYPE);
    if (which < 0) {
      return this->failCrypto(draft);
    }
    auto types = CryptoChoiceTable::getValues(CryptoChoice::KEY_ENCODING_TYPE);
    draft.option = this->cryptoString(typeOption->loc(), types[which]);
    draft.flags |= 1;
  }
  Expr *cipher = this->cryptoOption(options, CryptoOption::CIPHER);
  Expr *passphrase = this->cryptoOption(options, CryptoOption::PASSPHRASE);
  draft.flags |= (cipher != nullptr ? 2 : 0) | (passphrase != nullptr ? 4 : 0);
  auto passphraseName = CryptoOptionTable::getName(CryptoOption::PASSPHRASE);
  auto cipherItem =
      cipher != nullptr ? this->argItem(cipher, Type::string()) : this->stringLiteralItem(loc, "");
  auto passphraseItem = passphrase != nullptr
                            ? this->dataItem(passphrase, passphraseName, kTextOrBuffers)
                            : this->undefinedItem(loc);

  return this->pushPlan(draft, CryptoFinish::METHOD_EXPORT, {cipherItem, passphraseItem});
}

void LibraryCryptoRegistry::finishEncodedExport(CryptoPlanDraft draft,
                                                const std::vector<Expr *> &values) {
  Expr *self = draft.member->object;
  Expr *cipher = values[0];
  Expr *passphrase = values[1];
  bool hasPassphrase = (draft.flags & 4) != 0;
  if (hasPassphrase && passphrase->type->isError()) {
    return this->failCrypto(draft);
  }
  bool isPem = draft.choice == int(CryptoKeyFormat::PEM);
  draft.fnCode = isPem ? CryptoFn::KeyExportPem : CryptoFn::KeyExportDer;
  draft.resultType = isPem ? Type::string() : Type::bytesType(Type::Bytes::Buffer);
  auto *has = this->cryptoInt(draft.loc, draft.flags);
  auto exportArgs = std::vector<Expr *>{self, has, draft.option, cipher, passphrase};

  return this->finishResult(draft, exportArgs);
}

void LibraryCryptoRegistry::finishKeyEquals(const CryptoPlanDraft &draft,
                                            const std::vector<Expr *> &values) {
  Expr *self = draft.member->object;
  Expr *other = values[0];
  if (other->type->isError()) {
    return this->failCrypto(draft);
  }
  auto result = this->withResult(draft, CryptoFn::KeyEquals, Type::boolTy());

  return this->finishCall(result, {self, other});
}

void LibraryCryptoRegistry::startEcdhPublicKey(const CryptoPlanDraft &draft) {
  CallExpr *call = draft.call;
  Expr *self = draft.member->object;
  if (!this->hasMethodArity(draft, 0, 2)) {
    return;
  }
  int which = 0;
  if (this->isPresent(call->args, 1)) {
    auto point = ErrorTextCryptoPoint::format();
    which = this->cryptoChoice(call->args[1], point, CryptoChoice::POINT_CONVERSION);
    if (which < 0) {
      return this->failCrypto(draft);
    }
  }
  auto *format = this->cryptoInt(draft.loc, which);
  bool isGenerate = draft.method == CryptoMethod::ECDH_GENERATE_KEYS;
  CryptoFn fn = isGenerate ? CryptoFn::EcdhGenerateKeys : CryptoFn::EcdhGetPublicKey;
  const auto *buffer = Type::bytesType(Type::Bytes::Buffer);
  auto result = this->withResult(draft, fn, buffer);

  return this->startBytesOrText(result, {self, format}, 0, !isGenerate);
}

void LibraryCryptoRegistry::startBytesOrText(CryptoPlanDraft draft,
                                             const std::vector<Expr *> &fnArgs, size_t index,
                                             bool isFallible) {
  draft.flags = isFallible ? 1 : 0;
  auto items = this->literalItems(fnArgs);
  auto encoding = this->encodingItem(draft.call, index, draft.member->name, -1);
  items.push_back(encoding);

  return this->pushPlan(draft, CryptoFinish::BYTES_OR_TEXT, items);
}

void LibraryCryptoRegistry::finishBytesOrText(const CryptoPlanDraft &draft,
                                              const std::vector<Expr *> &values) {
  CallExpr *call = draft.call;
  auto &tasks = this->state.walk.tasks;
  Expr *encoding = values.back();
  if (encoding == nullptr) {
    return tasks.pushChecked(call);
  }
  auto fnArgs = std::vector<Expr *>(values.begin(), values.end() - 1);
  auto noArgs = std::vector<Expr *>();
  auto *made = this->state.ast.make<CallExpr>(call->loc(), call->callee, noArgs);
  const auto *buffer = Type::bytesType(Type::Bytes::Buffer);
  bool isFallible = (draft.flags & 1) != 0;
  if (!isFallible) {
    this->cryptoCall(made, draft.fnCode, std::move(fnArgs), buffer);
    auto *text = this->cryptoText(made, encoding);

    return tasks.pushChecked(text);
  }
  tasks.pushTask(FinishBytesText{.made = made, .encoding = encoding});

  return this->pushCryptoResult(made, draft.fnCode, std::move(fnArgs), buffer);
}

void LibraryCryptoRegistry::finishBytesText(const FinishBytesText &task) {
  auto *text = this->cryptoText(task.made, task.encoding);

  return this->state.walk.tasks.pushChecked(text);
}

void LibraryCryptoRegistry::finishX509Name(CryptoPlanDraft draft,
                                           const std::vector<Expr *> &values) {
  CallExpr *call = draft.call;
  Expr *self = draft.member->object;
  llvm::SMLoc loc = draft.loc;
  Expr *value = values[0];
  static const CryptoOption hostFlags[] = {CryptoOption::WILDCARDS, CryptoOption::PARTIAL_WILDCARDS,
                                           CryptoOption::MULTI_LABEL_WILDCARDS,
                                           CryptoOption::SINGLE_LABEL_SUBDOMAINS};
  auto allowed = std::vector<CryptoOption>{CryptoOption::SUBJECT};
  if (draft.method == CryptoMethod::X509_CHECK_HOST) {
    allowed.insert(allowed.end(), std::begin(hostFlags), std::end(hostFlags));
  }
  bool isGiven = this->isPresent(call->args, 1);
  if (isGiven && !this->readCryptoOptions(call->args[1], draft.fn, allowed)) {
    return this->failCrypto(draft);
  }
  int subject = 0;
  Expr *subjectOption = this->optionAt(draft, 1, CryptoOption::SUBJECT);
  if (subjectOption != nullptr) {
    auto subjectWhat = ErrorTextCryptoLabel::subjectOption();
    subject = this->cryptoChoice(subjectOption, subjectWhat, CryptoChoice::CHECK_SUBJECT);
    if (subject < 0) {
      return this->failCrypto(draft);
    }
  }
  draft.resultType = Type::optional(Type::string());
  auto *subjectValue = this->cryptoInt(loc, subject);
  if (draft.method == CryptoMethod::X509_CHECK_IP) {
    draft.fnCode = CryptoFn::X509CheckIp;

    return this->finishCall(draft, {self, value});
  }
  if (draft.method == CryptoMethod::X509_CHECK_EMAIL) {
    draft.fnCode = CryptoFn::X509CheckEmail;

    return this->finishCall(draft, {self, value, subjectValue});
  }
  int has = 0;
  auto flagItems = std::vector<CryptoValueItem>();
  for (int index = 0; index < 4; ++index) {
    Expr *flag = this->optionAt(draft, 1, hostFlags[index]);
    has |= flag != nullptr ? 1 << index : 0;
    auto item =
        flag != nullptr ? this->argItem(flag, Type::boolTy()) : this->boolLiteralItem(loc, false);
    flagItems.push_back(item);
  }
  auto *hasValue = this->cryptoInt(loc, has);
  auto items = this->literalItems({self, value, subjectValue, hasValue});
  items.insert(items.end(), flagItems.begin(), flagItems.end());
  draft.fnCode = CryptoFn::X509CheckHost;

  return this->pushPlan(draft, CryptoFinish::CALL, items);
}

void LibraryCryptoRegistry::finishX509Verify(CryptoPlanDraft draft,
                                             const std::vector<Expr *> &values) {
  Expr *self = draft.member->object;
  Expr *other = values[0];
  bool isIssued = draft.method == CryptoMethod::X509_CHECK_ISSUED;
  if (isIssued) {
    auto issued = this->checkIssuer(other);
    if (issued.isErr()) {
      this->state.diagnostics.error(other->loc(), issued.error().message);

      return this->failCrypto(draft);
    }
  }
  if (other->type->isError()) {
    return this->failCrypto(draft);
  }
  draft.resultType = Type::boolTy();
  if (isIssued) {
    draft.fnCode = CryptoFn::X509CheckIssued;

    return this->finishCall(draft, {self, other});
  }
  bool isPrivateKey = draft.method == CryptoMethod::X509_CHECK_PRIVATE_KEY;
  draft.fnCode = isPrivateKey ? CryptoFn::X509CheckPrivateKey : CryptoFn::X509Verify;

  return this->finishResult(draft, {self, other});
}

void LibraryCryptoRegistry::checkSubtleIntrinsic(const CheckSubtleIntrinsic &task) {
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  call->type = Type::error();
  tasks.pushChecked(call);
  auto shape = this->checkSubtleShape(call, task.intrinsicName);
  if (shape.isErr()) {
    this->state.diagnostics.error(task.name->loc(), shape.error().message);

    return this->pushLooseArgs(call);
  }
  const SubtleIntrinsic *intrinsic = shape.value();
  auto draft = this->functionDraft(call, call->loc());
  draft.fnCode = intrinsic->fn;
  draft.fn = task.intrinsicName;
  draft.resultType = Type::promise(this->subtleResult(intrinsic->result));
  auto items = std::vector<CryptoValueItem>();
  for (size_t index = 0; index < call->args.size(); ++index) {
    auto item = this->valueItem(CryptoValueKind::SUBTLE_ARG, call->args[index]);
    item.subtleKind = intrinsic->params[index];
    item.what = task.intrinsicName.str();
    items.push_back(item);
  }

  return this->pushPlan(draft, CryptoFinish::SUBTLE_INTRINSIC, items);
}

void LibraryCryptoRegistry::pushSubtleArg(const CryptoValueItem &item) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(AfterSubtleArg{.arg = item.arg, .subtleKind = item.subtleKind, .name = item.what});
  switch (item.subtleKind) {
    case 'm':
      return this->pushMd(item.arg, DigestUse::HMAC);
    case 'c':
      return this->pushCipher(item.arg);
    case 'd': {
      auto dataWhat = ErrorTextCryptoLabel::data();
      auto data = this->dataItem(item.arg, dataWhat, kKeysOrBuffers);
      return tasks.pushTask(CryptoValue{.item = data});
    }
    case 'o': {
      auto keyWhat = ErrorTextCryptoLabel::key();
      auto key = this->keyObjectItem(item.arg, keyWhat);
      return tasks.pushTask(CryptoValue{.item = key});
    }
    case 'b':
      return this->pushArg(item.arg, Type::boolTy());
    case 'i':
      return this->pushArg(item.arg, Type::intTy());
    case 'f':
      return this->pushArg(item.arg, Type::doubleTy());
  }

  return this->pushArg(item.arg, Type::string());
}

void LibraryCryptoRegistry::afterSubtleArg(const AfterSubtleArg &task) {
  auto &tasks = this->state.walk.tasks;
  Expr *checked = tasks.takeChecked();
  auto literal = this->checkLiteralAlgorithm(checked, task.subtleKind, task.name);
  if (literal.isErr()) {
    this->state.diagnostics.error(task.arg->loc(), literal.error().message);

    return tasks.pushChecked(nullptr);
  }

  return tasks.pushChecked(checked);
}

void LibraryCryptoRegistry::finishSubtleIntrinsic(const CryptoPlanDraft &draft,
                                                  const std::vector<Expr *> &values) {
  for (Expr *value : values) {
    if (value == nullptr || value->type->isError()) {
      return;
    }
  }

  return this->cryptoCall(draft.call, draft.fnCode, values, draft.resultType);
}

void LibraryCryptoRegistry::checkSubtleCall(const CheckSubtleCall &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  auto method = SubtleMethodTable::find(member->name);
  if (call->hasSpread() || !method.has_value()) {
    return tasks.pushChecked(nullptr);
  }
  auto subtle = SubtleDraft{
      .call = call,
      .member = member,
      .method = *method,
      .checkedArgs = std::vector<bool>(call->args.size(), false),
      .keyFormat = std::nullopt,
      .target = std::nullopt,
      .isBad = false,
      .isJwkWrapped = false,
      .expected = task.expected,
  };
  this->walkSubtleArguments(subtle);
  if (!subtle.isJwkWrapped) {
    return this->continueSubtleCall(subtle);
  }
  tasks.pushTask(AfterJwkArgument{.subtle = subtle});

  return tasks.pushTask(CheckExpr{.expr = call->args[1], .expected = nullptr});
}

void LibraryCryptoRegistry::afterJwkArgument(const AfterJwkArgument &task) {
  SubtleDraft subtle = task.subtle;
  auto &tasks = this->state.walk.tasks;
  Expr *json = tasks.takeChecked();
  auto jwkOf = LibraryFunctionTable::getName(LibraryFunction::SUBTLE_JWK_OF);
  auto *callee = this->state.ast.make<NameExpr>(json->loc(), jwkOf);
  auto *wrap = this->state.ast.make<CallExpr>(json->loc(), callee, std::vector<Expr *>{json});
  subtle.checkedArgs[1] = true;
  if (!subtle.target.has_value()) {
    subtle.target = LibraryFunction::SUBTLE_IMPORT_ANY;
  }
  tasks.pushTask(AfterJwkWrap{.subtle = subtle});

  return tasks.pushTask(CallLibraryFunction{
      .call = wrap,
      .name = jwkOf,
      .bound = {},
      .given = {true},
      .loc = json->loc(),
  });
}

void LibraryCryptoRegistry::afterJwkWrap(const AfterJwkWrap &task) {
  SubtleDraft subtle = task.subtle;
  subtle.call->args[1] = this->state.walk.tasks.takeChecked();

  return this->continueSubtleCall(subtle);
}

void LibraryCryptoRegistry::continueSubtleCall(const SubtleDraft &subtle) {
  CallExpr *call = subtle.call;
  MemberExpr *member = subtle.member;
  auto &tasks = this->state.walk.tasks;
  if (subtle.isBad) {
    return this->checkRejectedSubtleCall(subtle);
  }
  auto &declarations = this->state.library.declarations;
  bool isLibrary = subtle.target.has_value() &&
                   declarations.findLibraryFunction(*subtle.target) != nullptr &&
                   isa<NameExpr>(member->object);
  tasks.pushTask(AfterSubtleFinish{.subtle = subtle});
  if (!isLibrary) {
    return tasks.pushTask(
        CheckMethodCall{.call = call, .member = member, .expected = subtle.expected});
  }
  auto targetName = LibraryFunctionTable::getName(*subtle.target);

  return tasks.pushTask(CallLibraryFunction{
      .call = call,
      .name = targetName,
      .bound = {},
      .given = subtle.checkedArgs,
      .loc = member->loc(),
  });
}

void LibraryCryptoRegistry::afterSubtleFinish(const AfterSubtleFinish &task) {
  auto &tasks = this->state.walk.tasks;
  Expr *checked = tasks.takeChecked();
  if (!this->matchKeyData(task.subtle)) {
    checked->type = Type::error();
  }

  return tasks.pushChecked(checked);
}

void LibraryCryptoRegistry::checkRejectedSubtleCall(const SubtleDraft &subtle) {
  CallExpr *call = subtle.call;
  std::vector<Expr *> &args = call->args;
  auto &tasks = this->state.walk.tasks;
  call->type = Type::error();
  tasks.pushChecked(call);
  const auto *subtleClass =
      this->state.library.declarations.findLibraryClass(LibraryClass::SUBTLE_CRYPTO);
  auto methods = subtleClass->findMethodsNamed(subtle.member->name);
  FuncSymbol *function = methods.size() == 1 ? methods[0]->sym : nullptr;
  for (size_t index = args.size(); index > 0; --index) {
    size_t at = index - 1;
    if (subtle.checkedArgs[at]) {
      continue;
    }
    bool isTyped = function != nullptr && at < function->params.size();
    if (isTyped) {
      tasks.pushTask(CheckArgSlot{.slot = &args[at], .to = function->params[at]});
    } else {
      tasks.pushTask(CheckExprSlot{.slot = &args[at], .expected = nullptr});
    }
  }
}

Expr *LibraryCryptoRegistry::cryptoMember(MemberExpr *member) {
  const auto *found = CryptoPropertyTable::find(member->object->type->crypto(), member->name);
  if (found == nullptr) {
    return this->reportOnlyCalled(member);
  }
  switch (found->property) {
    case CryptoProperty::KEY_TYPE:
      return this->readCryptoProperty(member, CryptoFn::KeyType, Type::string());
    case CryptoProperty::KEY_ASYMMETRIC_KEY_TYPE:
      return this->readCryptoProperty(member, CryptoFn::KeyAsymmetricType,
                                      Type::optional(Type::string()));
    case CryptoProperty::KEY_SYMMETRIC_KEY_SIZE:
      return this->readCryptoProperty(member, CryptoFn::KeySymmetricSize,
                                      Type::optional(Type::intTy()));
    case CryptoProperty::KEY_ASYMMETRIC_KEY_DETAILS:
      return this->reportCryptoMember(member, ErrorTextCryptoMember::keyDetails());
    case CryptoProperty::X509_SUBJECT:
      return this->readCryptoProperty(member, CryptoFn::X509Subject, Type::string());
    case CryptoProperty::X509_ISSUER:
      return this->readCryptoProperty(member, CryptoFn::X509Issuer, Type::string());
    case CryptoProperty::X509_VALID_FROM:
      return this->readCryptoProperty(member, CryptoFn::X509ValidFrom, Type::string());
    case CryptoProperty::X509_VALID_TO:
      return this->readCryptoProperty(member, CryptoFn::X509ValidTo, Type::string());
    case CryptoProperty::X509_VALID_FROM_DATE:
      return this->readCryptoProperty(member, CryptoFn::X509ValidFromDate, Type::date());
    case CryptoProperty::X509_VALID_TO_DATE:
      return this->readCryptoProperty(member, CryptoFn::X509ValidToDate, Type::date());
    case CryptoProperty::X509_SERIAL_NUMBER:
      return this->readCryptoProperty(member, CryptoFn::X509SerialNumber, Type::string());
    case CryptoProperty::X509_FINGERPRINT:
      return this->readFingerprint(member, 1);
    case CryptoProperty::X509_FINGERPRINT256:
      return this->readFingerprint(member, 256);
    case CryptoProperty::X509_FINGERPRINT512:
      return this->readFingerprint(member, 512);
    case CryptoProperty::X509_SUBJECT_ALT_NAME:
      return this->readCryptoProperty(member, CryptoFn::X509SubjectAltName,
                                      Type::optional(Type::string()));
    case CryptoProperty::X509_INFO_ACCESS:
      return this->readCryptoProperty(member, CryptoFn::X509InfoAccess,
                                      Type::optional(Type::string()));
    case CryptoProperty::X509_KEY_USAGE:
      return this->readCryptoProperty(member, CryptoFn::X509KeyUsage,
                                      Type::optional(Type::array(Type::string())));
    case CryptoProperty::X509_CA:
      return this->readCryptoProperty(member, CryptoFn::X509Ca, Type::boolTy());
    case CryptoProperty::X509_RAW:
      return this->readCryptoProperty(member, CryptoFn::X509Raw,
                                      Type::bytesType(Type::Bytes::Buffer));
    case CryptoProperty::X509_PUBLIC_KEY:
      return this->readCryptoProperty(member, CryptoFn::X509PublicKey,
                                      Type::cryptoType(Type::Crypto::KeyObject));
    case CryptoProperty::X509_ISSUER_CERTIFICATE:
      return this->reportCryptoMember(member, ErrorTextCryptoMember::issuerCert());
  }

  return this->reportOnlyCalled(member);
}

Expr *LibraryCryptoRegistry::readCryptoProperty(MemberExpr *member, CryptoFn fn,
                                                const Type *result) {
  std::vector<Expr *> args{member->object};
  auto noArgs = std::vector<Expr *>();
  auto *call = this->state.ast.make<CallExpr>(member->loc(), member, noArgs);
  this->cryptoCall(call, fn, std::move(args), result);

  return call;
}

Expr *LibraryCryptoRegistry::readFingerprint(MemberExpr *member, int bits) {
  auto *size = this->cryptoInt(member->loc(), bits);
  std::vector<Expr *> args{member->object, size};
  auto noArgs = std::vector<Expr *>();
  auto *call = this->state.ast.make<CallExpr>(member->loc(), member, noArgs);
  this->cryptoCall(call, CryptoFn::X509Fingerprint, std::move(args), Type::string());

  return call;
}

Expr *LibraryCryptoRegistry::reportCryptoMember(MemberExpr *member, const std::string &message) {
  this->state.diagnostics.error(member->loc(), message);
  member->type = Type::error();

  return member;
}

Expr *LibraryCryptoRegistry::reportOnlyCalled(MemberExpr *member) {
  const Type *type = member->object->type;
  Type::Crypto kind = type->crypto();
  auto methods = ErrorTextCryptoMethods::of(kind);
  auto message = ErrorTextCryptoMember::onlyCalled(member->name, type->name(), methods);

  return this->reportCryptoMember(member, message);
}

std::optional<std::string> LibraryCryptoRegistry::literalOf(const Expr *expr) const {
  auto *conversion = dyn_cast<CastExpr>(expr);
  if (conversion != nullptr && conversion->isImplicit()) {
    expr = conversion->operand;
  }
  auto *literal = dyn_cast<StringLitExpr>(expr);
  if (literal != nullptr) {
    return ToUtf8::toUtf8(literal->value);
  }
  auto *templateExpr = dyn_cast<TemplateExpr>(expr);
  if (templateExpr != nullptr && templateExpr->exprs.empty()) {
    return ToUtf8::toUtf8(templateExpr->quasis[0]);
  }

  return std::nullopt;
}

Expr *LibraryCryptoRegistry::cryptoInt(llvm::SMLoc loc, int64_t value) {
  auto *literal = this->state.ast.make<IntLitExpr>(loc, uint64_t(value));
  literal->type = Type::intTy();

  return literal;
}

Expr *LibraryCryptoRegistry::cryptoDouble(llvm::SMLoc loc, double value) {
  auto *literal = this->state.ast.make<FloatLitExpr>(loc, value);
  literal->type = Type::doubleTy();

  return literal;
}

Expr *LibraryCryptoRegistry::cryptoBool(llvm::SMLoc loc, bool value) {
  auto *literal = this->state.ast.make<BoolLitExpr>(loc, value);
  literal->type = Type::boolTy();

  return literal;
}

Expr *LibraryCryptoRegistry::cryptoString(llvm::SMLoc loc, llvm::StringRef text) {
  auto *literal = this->state.ast.make<StringLitExpr>(loc, ToUtf16::toUtf16(text));
  literal->type = Type::string();

  return literal;
}

Expr *LibraryCryptoRegistry::cryptoUndefined(llvm::SMLoc loc) {
  auto *undef = this->state.ast.make<UndefinedExpr>(loc);
  undef->type = Type::undefinedTy();

  return undef;
}

std::vector<llvm::StringRef>
LibraryCryptoRegistry::optionNames(llvm::ArrayRef<CryptoOption> options) const {
  auto names = std::vector<llvm::StringRef>();
  for (CryptoOption option : options) {
    names.push_back(CryptoOptionTable::getName(option));
  }

  return names;
}

std::string LibraryCryptoRegistry::numberText(double value) const {
  if (value == std::floor(value) && std::fabs(value) < 1e15) {
    return std::to_string(int64_t(value));
  }

  return llvm::formatv("{0:g}", value).str();
}

std::optional<double> LibraryCryptoRegistry::knownNumber(const Expr *arg) const {
  auto negations = std::vector<const Expr *>();
  const Expr *inner = arg;
  auto *negation = dyn_cast<UnaryExpr>(inner);
  while (negation != nullptr && negation->op == UnaryOp::Neg) {
    negations.push_back(inner);
    inner = negation->operand;
    negation = dyn_cast<UnaryExpr>(inner);
  }
  auto value = this->plainNumber(inner);
  for (const Expr *negated : llvm::reverse(negations)) {
    if (value.has_value()) {
      value = -*value;
    } else {
      value = this->plainNumber(negated);
    }
  }

  return value;
}

std::optional<double> LibraryCryptoRegistry::plainNumber(const Expr *arg) const {
  const auto *fraction = dyn_cast<FloatLitExpr>(arg);
  if (fraction != nullptr) {
    return fraction->value;
  }
  const auto *integer = dyn_cast<IntLitExpr>(arg);
  if (integer != nullptr) {
    return double(integer->value);
  }
  const auto *conversion = dyn_cast<CastExpr>(arg);
  auto converted = conversion != nullptr
                       ? this->state.code.constants.intConstant(conversion->operand)
                       : std::nullopt;
  if (converted.has_value()) {
    return double(*converted);
  }
  auto value = this->state.code.constants.intConstant(arg);
  if (!value.has_value()) {
    return std::nullopt;
  }

  return double(*value);
}

bool LibraryCryptoRegistry::matchWhole(const Expr *arg, llvm::StringRef name, double least) {
  auto value = this->knownNumber(arg);
  if (!value || (*value == std::floor(*value) && *value >= least)) {
    return true;
  }
  auto leastShown = this->numberText(least);
  auto valueShown = this->numberText(*value);
  auto message = ErrorTextCryptoNumber::whole(name, leastShown, valueShown);
  this->state.diagnostics.error(arg->loc(), message);

  return false;
}

bool LibraryCryptoRegistry::matchCipherLengths(const Expr *cipher, const Expr *key,
                                               const Expr *iv) {
  const auto *index = dyn_cast<IntLitExpr>(cipher);
  if (index == nullptr) {
    return true;
  }
  const DirectCipher &direct = kDirectCiphers[index->value];
  llvm::StringRef name = direct.name;
  bool isOk = true;
  std::optional<int64_t> have = this->bytesOf(key);
  if (have.has_value() && *have != direct.keyBytes) {
    auto message = ErrorTextCryptoCipher::keyBytes(name, direct.keyBytes, *have);
    this->state.diagnostics.error(key->loc(), message);
    isOk = false;
  }
  std::optional<int64_t> ivHave = this->bytesOf(iv);
  if (ivHave.has_value() && direct.isAnyIv && *ivHave == 0) {
    auto message = ErrorTextCryptoCipher::emptyIv(name);
    this->state.diagnostics.error(iv->loc(), message);
    isOk = false;
  } else if (ivHave.has_value() && !direct.isAnyIv && *ivHave != direct.ivBytes) {
    auto message = ErrorTextCryptoCipher::ivBytes(name, direct.ivBytes, *ivHave);
    this->state.diagnostics.error(iv->loc(), message);
    isOk = false;
  }

  return isOk;
}

std::optional<int64_t> LibraryCryptoRegistry::bytesOf(const Expr *expr) const {
  Type::Bytes kind = expr->type->bytes();
  if (kind != Type::Bytes::Buffer && kind != Type::Bytes::Uint8) {
    return std::nullopt;
  }

  return this->state.flow.measurements.exactLength(expr);
}

LibraryCryptoRegistry::ResultKeyObjectType
LibraryCryptoRegistry::checkKeyObjectType(const Type *type, llvm::StringRef what) const {
  if (!type->isError() && type->crypto() != Crypto::KeyObject) {
    auto message = ErrorTextCryptoKey::keyObject(what, type->name());

    return err(LibraryCryptoRegistryKeyObjectTypeError{
        .code = LibraryCryptoRegistryKeyObjectTypeErrorCode::NOT_A_KEY_OBJECT,
        .message = message,
    });
  }

  return ok(type);
}

bool LibraryCryptoRegistry::isCryptoData(CryptoDataKinds kinds, const Type *candidate) const {
  bool isText = candidate->isString() || candidate->isStringLiteral();

  return (kinds.isString && isText) || candidate->isByteView() ||
         (kinds.isArrayBuffer && candidate->bytes() == Type::Bytes::ArrayBuffer) ||
         (kinds.isKey && candidate->crypto() == Crypto::KeyObject);
}

std::optional<size_t> LibraryCryptoRegistry::directMdIndex(llvm::StringRef name) const {
  for (size_t index = 0; index < std::size(kDirectMds); ++index) {
    if (kDirectMds[index].name == name) {
      return index;
    }
  }

  return std::nullopt;
}

llvm::StringRef LibraryCryptoRegistry::errorNameOf(CryptoFn fn, llvm::ArrayRef<Expr *> args) const {
  if (fn == CryptoFn::CipherNew) {
    return this->flag(args, 4) ? "CreateDecipherError" : "CreateCipherError";
  }
  if (fn == CryptoFn::CipherFinal) {
    return this->isDecipher(args) ? "DecipherFinalError" : "CipherFinalError";
  }
  if (fn == CryptoFn::CreateKey) {
    return this->flag(args, 0) ? "CreatePrivateKeyError" : "CreatePublicKeyError";
  }
  if (fn == CryptoFn::SignNew) {
    return this->flag(args, 1) ? "CreateVerifyError" : "CreateSignError";
  }
  auto plain = CryptoErrorTypes::callErrorName(fn);
  if (!plain.has_value()) {
    return "CreateHashError";
  }

  return *plain;
}

bool LibraryCryptoRegistry::isDecipher(llvm::ArrayRef<Expr *> args) const {
  const Type *self = args.empty() ? nullptr : args[0]->type;

  return self != nullptr && self->crypto() == Type::Crypto::Decipheriv;
}

bool LibraryCryptoRegistry::flag(llvm::ArrayRef<Expr *> args, size_t i) const {
  const BoolLitExpr *literal = i < args.size() ? dyn_cast<BoolLitExpr>(args[i]) : nullptr;

  return literal != nullptr && literal->value;
}

LibraryCryptoRegistry::ResultFilledTarget
LibraryCryptoRegistry::checkFilledTarget(const Type *type, CryptoSyncFunction function,
                                         llvm::StringRef shown) const {
  if (function == CryptoSyncFunction::GET_RANDOM_VALUES) {
    static const Type::Bytes floats[] = {Type::Bytes::Float16, Type::Bytes::Float32,
                                         Type::Bytes::Float64};
    if (!type->isTypedArray() || llvm::is_contained(floats, type->bytes())) {
      auto message = ErrorTextCryptoFunction::randomVals(type->name());

      return err(LibraryCryptoRegistryFilledTargetError{
          .code = LibraryCryptoRegistryFilledTargetErrorCode::NOT_INTEGERS,
          .message = message,
      });
    }

    return ok(type);
  }
  if (!type->isByteView() && type->bytes() != Type::Bytes::ArrayBuffer) {
    auto message = ErrorTextCryptoFunction::fillsBytes(shown, type->name());

    return err(LibraryCryptoRegistryFilledTargetError{
        .code = LibraryCryptoRegistryFilledTargetErrorCode::NOT_BYTES,
        .message = message,
    });
  }

  return ok(type);
}

LibraryCryptoRegistry::ResultCurveIsKnown
LibraryCryptoRegistry::checkCurveIsKnown(llvm::StringRef curve) const {
  if (!CurveNameTable::isKnown(curve)) {
    auto message = ErrorTextCryptoFunction::curve(curve);

    return err(LibraryCryptoRegistryCurveIsKnownError{
        .code = LibraryCryptoRegistryCurveIsKnownErrorCode::UNKNOWN_CURVE,
        .message = message,
    });
  }

  return ok(curve);
}

LibraryCryptoRegistry::ResultIssuerChecked
LibraryCryptoRegistry::checkIssuer(const Expr *other) const {
  if (!other->type->isError() && other->type->crypto() != Crypto::X509) {
    auto message = ErrorTextCryptoMethod::issuer(other->type->name());

    return err(LibraryCryptoRegistryIssuerError{
        .code = LibraryCryptoRegistryIssuerErrorCode::NOT_A_CERTIFICATE,
        .message = message,
    });
  }

  return ok(other);
}

LibraryCryptoRegistry::ResultKeyOptionsGiven
LibraryCryptoRegistry::checkKeyOptions(int which, llvm::StringRef type,
                                       const CryptoPlanDraft &draft) const {
  bool isModulusNeeded = which >= int(KeyPairType::RSA) && which <= int(KeyPairType::DSA);
  Expr *modulus = this->pairOption(draft, CryptoOption::MODULUS_LENGTH);
  Expr *curve = this->pairOption(draft, CryptoOption::NAMED_CURVE);
  if (isModulusNeeded && modulus == nullptr) {
    auto message = ErrorTextKeyPair::modulus(type);

    return err(LibraryCryptoRegistryKeyOptionsError{
        .code = LibraryCryptoRegistryKeyOptionsErrorCode::NO_MODULUS_LENGTH,
        .message = message,
    });
  }
  if (which == int(KeyPairType::EC) && curve == nullptr) {
    auto message = ErrorTextKeyPair::curve();

    return err(LibraryCryptoRegistryKeyOptionsError{
        .code = LibraryCryptoRegistryKeyOptionsErrorCode::NO_NAMED_CURVE,
        .message = message,
    });
  }

  return ok(which);
}

const Type *LibraryCryptoRegistry::encodedKeyType(int format) const {
  if (format < 0) {
    return Type::cryptoType(Crypto::KeyObject);
  }
  if (format == int(CryptoKeyFormat::PEM)) {
    return Type::string();
  }
  if (format == int(CryptoKeyFormat::DER)) {
    return Type::bytesType(Type::Bytes::Buffer);
  }

  return Type::json();
}

LibraryCryptoRegistry::ResultKeyFormatKnown
LibraryCryptoRegistry::checkKnownFormat(llvm::StringRef format) const {
  auto known = SubtleKeyFormatTable::find(format);
  if (!known.has_value()) {
    auto message = ErrorTextKeyFormat::unknown(format);

    return err(LibraryCryptoRegistryKnownFormatError{
        .code = LibraryCryptoRegistryKnownFormatErrorCode::UNKNOWN_FORMAT,
        .message = message,
    });
  }

  return ok(*known);
}

std::optional<double> LibraryCryptoRegistry::numberLiteral(const Expr *expr) const {
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

const ObjectLitExpr::Property *LibraryCryptoRegistry::propertyNamed(const ObjectLitExpr *literal,
                                                                    llvm::StringRef name) const {
  for (const ObjectLitExpr::Property &property : literal->props) {
    if (!property.spread() && !property.key() && property.name == name) {
      return &property;
    }
  }

  return nullptr;
}

const ObjectLitExpr::Property *LibraryCryptoRegistry::memberProperty(const ObjectLitExpr *literal,
                                                                     SubtleMember member) const {
  auto name = SubtleMemberTable::getName(member);

  return this->propertyNamed(literal, name);
}

LibraryCryptoRegistry::ResultAlgorithmKnown
LibraryCryptoRegistry::checkKnownAlgorithm(llvm::StringRef written, SubtleOperation operation,
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

    return err(LibraryCryptoRegistryKnownAlgorithmError{
        .code = LibraryCryptoRegistryKnownAlgorithmErrorCode::EXPERIMENTAL,
        .message = message,
    });
  }
  if (SubtleAlgorithmTable::findUpper(upper) != nullptr) {
    auto message = ErrorTextAlgorithm::otherOp(written, operationName, verb, list);

    return err(LibraryCryptoRegistryKnownAlgorithmError{
        .code = LibraryCryptoRegistryKnownAlgorithmErrorCode::OTHER_OPERATION,
        .message = message,
    });
  }
  auto message = ErrorTextAlgorithm::unknown(written, operationName, verb, list);

  return err(LibraryCryptoRegistryKnownAlgorithmError{
      .code = LibraryCryptoRegistryKnownAlgorithmErrorCode::UNKNOWN_ALGORITHM,
      .message = message,
  });
}

LibraryCryptoRegistry::ResultParamsAsObject
LibraryCryptoRegistry::checkParamsAsObject(const SubtleAlgorithmInfo &algorithm,
                                           SubtleOperation operation,
                                           const Dictionary &dictionary) const {
  if (!dictionary.required.empty()) {
    llvm::StringRef member = dictionary.required[0];
    auto example = SubtleMemberTable::getExample(member);
    auto operationName = SubtleOperationTable::getName(operation);
    auto message =
        ErrorTextAlgorithm::object(algorithm.name, operationName, dictionary.name, member, example);

    return err(LibraryCryptoRegistryParamsAsObjectError{
        .code = LibraryCryptoRegistryParamsAsObjectErrorCode::NEEDS_OBJECT,
        .message = message,
    });
  }

  return ok(&algorithm);
}

LibraryCryptoRegistry::ResultMembersPresent
LibraryCryptoRegistry::checkMembersPresent(const SubtleAlgorithmInfo &algorithm,
                                           SubtleOperation operation, const Dictionary &dictionary,
                                           const ObjectLitExpr *literal) const {
  for (llvm::StringRef member : dictionary.required) {
    if (this->propertyNamed(literal, member) == nullptr) {
      auto example = SubtleMemberTable::getExample(member);
      auto operationName = SubtleOperationTable::getName(operation);
      auto message = ErrorTextAlgorithm::needs(algorithm.name, operationName, member,
                                               dictionary.name, example);

      return err(LibraryCryptoRegistryMembersPresentError{
          .code = LibraryCryptoRegistryMembersPresentErrorCode::MISSING_MEMBER,
          .message = message,
      });
    }
  }

  return ok(literal);
}

LibraryCryptoRegistry::ResultShaHashIsKnown
LibraryCryptoRegistry::checkKnownShaHash(llvm::StringRef hash, llvm::StringRef dictionary) const {
  bool isSha = SubtleAlgorithmTable::hasShaPrefix(hash);
  bool isKnownHash = SubtleAlgorithmTable::findIn(SubtleOperation::DIGEST, hash.upper()) != nullptr;
  if (isSha && isKnownHash) {
    return ok(hash);
  }
  if (isSha || SubtleAlgorithmTable::isExperimental(hash.upper())) {
    auto list =
        ErrorTextAlgorithm::choices(SubtleAlgorithmTable::getNames(SubtleOperation::DIGEST));
    auto message = ErrorTextAlgorithm::hash(hash, dictionary, list);

    return err(LibraryCryptoRegistryKnownShaHashError{
        .code = LibraryCryptoRegistryKnownShaHashErrorCode::UNKNOWN_HASH,
        .message = message,
    });
  }
  auto message = ErrorTextAlgorithm::notSha(hash, dictionary);

  return err(LibraryCryptoRegistryKnownShaHashError{
      .code = LibraryCryptoRegistryKnownShaHashErrorCode::NOT_SHA,
      .message = message,
  });
}

LibraryCryptoRegistry::ResultCurveNameKnown
LibraryCryptoRegistry::checkKnownCurve(llvm::StringRef curve) const {
  if (!CurveNameTable::isWebCryptoCurve(curve)) {
    auto message = ErrorTextAlgorithm::curve(curve);

    return err(LibraryCryptoRegistryKnownCurveError{
        .code = LibraryCryptoRegistryKnownCurveErrorCode::UNKNOWN_CURVE,
        .message = message,
    });
  }

  return ok(curve);
}

LibraryCryptoRegistry::ResultAesLengthValid
LibraryCryptoRegistry::checkAesLength(double bits) const {
  if (bits != 128 && bits != 192 && bits != 256) {
    auto message = ErrorTextAlgorithm::aesLength();

    return err(LibraryCryptoRegistryAesLengthError{
        .code = LibraryCryptoRegistryAesLengthErrorCode::BAD_LENGTH,
        .message = message,
    });
  }

  return ok(bits);
}

LibraryCryptoRegistry::ResultGcmTagLengthOk
LibraryCryptoRegistry::checkGcmTagLength(double bits) const {
  if (bits != 32 && bits != 64 && bits != 96 && bits != 104 && bits != 112 && bits != 120 &&
      bits != 128) {
    auto message = ErrorTextAlgorithm::gcmTag();

    return err(LibraryCryptoRegistryGcmTagLengthError{
        .code = LibraryCryptoRegistryGcmTagLengthErrorCode::BAD_TAG,
        .message = message,
    });
  }

  return ok(bits);
}

const SubtleAlgorithmInfo *
LibraryCryptoRegistry::literalAlgorithm(const Expr *arg, SubtleOperation operation,
                                        std::optional<SubtleOperation> fallback, bool &isBad) {
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
    isBad = true;
    this->state.diagnostics.error(nameExpr->loc(), known.error().message);
    return nullptr;
  }
  const SubtleAlgorithmInfo *algorithm = known.value();
  auto dictionary = SubtleAlgorithms::dictionaryOf(operation, *algorithm);
  if (literal == nullptr) {
    auto object = this->checkParamsAsObject(*algorithm, operation, dictionary);
    if (object.isErr()) {
      isBad = true;
      this->state.diagnostics.error(nameExpr->loc(), object.error().message);
    }
    return isBad ? nullptr : algorithm;
  }
  auto present = this->checkMembersPresent(*algorithm, operation, dictionary, literal);
  if (present.isErr()) {
    isBad = true;
    this->state.diagnostics.error(literal->loc(), present.error().message);
    return nullptr;
  }
  bool isReported = this->reportBadHash(literal, dictionary.name) ||
                    this->reportBadCurve(literal) ||
                    this->reportBadAesLength(literal, *algorithm, operation) ||
                    this->reportBadGcmTag(literal, *algorithm);
  if (isReported) {
    isBad = true;
    return nullptr;
  }

  return algorithm;
}

const Expr *LibraryCryptoRegistry::algorithmNameExpr(const Expr *arg,
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

bool LibraryCryptoRegistry::reportBadHash(const ObjectLitExpr *literal,
                                          llvm::StringRef dictionary) {
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
    this->state.diagnostics.error(value->loc(), sha.error().message);
    return true;
  }

  return false;
}

bool LibraryCryptoRegistry::reportBadCurve(const ObjectLitExpr *literal) {
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
    this->state.diagnostics.error(curve->value->loc(), named.error().message);
    return true;
  }

  return false;
}

bool LibraryCryptoRegistry::reportBadAesLength(const ObjectLitExpr *literal,
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
    this->state.diagnostics.error(length->value->loc(), valid.error().message);
    return true;
  }

  return false;
}

bool LibraryCryptoRegistry::reportBadGcmTag(const ObjectLitExpr *literal,
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
    this->state.diagnostics.error(tag->value->loc(), valid.error().message);
    return true;
  }

  return false;
}

void LibraryCryptoRegistry::walkSubtleArguments(SubtleDraft &subtle) {
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

Expr *LibraryCryptoRegistry::argAt(const CallExpr *call, size_t index) const {
  return index < call->args.size() ? call->args[index] : nullptr;
}

const Expr *LibraryCryptoRegistry::algorithmAt(const CallExpr *call, size_t index) const {
  auto *arg = this->argAt(call, index);
  auto *name = dyn_cast_or_null<NameExpr>(arg);
  if (name == nullptr) {
    return arg;
  }
  VarSymbol *variable = this->state.walk.scopes.findName(name->name).var();
  if (variable == nullptr) {
    return arg;
  }
  const auto *literal = this->state.code.constants.findConstLiteral(variable);

  return literal != nullptr ? literal : arg;
}

void LibraryCryptoRegistry::walkAlgorithmOnly(SubtleDraft &subtle, SubtleOperation operation) {
  this->algorithmNameAt(subtle, 0, operation, std::nullopt);
}

const SubtleAlgorithmInfo *
LibraryCryptoRegistry::algorithmNameAt(SubtleDraft &subtle, size_t index, SubtleOperation operation,
                                       std::optional<SubtleOperation> fallback) {
  const auto *algorithm = this->algorithmAt(subtle.call, index);

  return this->literalAlgorithm(algorithm, operation, fallback, subtle.isBad);
}

std::optional<SubtleKeyFormat> LibraryCryptoRegistry::keyFormatAt(SubtleDraft &subtle,
                                                                  size_t index) {
  auto *given = this->argAt(subtle.call, index);
  auto written = SubtleAlgorithms::stringLiteral(given);
  if (!written.has_value()) {
    return std::nullopt;
  }
  auto known = this->checkKnownFormat(*written);
  if (known.isErr()) {
    subtle.isBad = true;
    this->state.diagnostics.error(given->loc(), known.error().message);
    return std::nullopt;
  }

  return known.value();
}

bool LibraryCryptoRegistry::isSecretAlgorithm(const SubtleAlgorithmInfo *algorithm) const {
  return algorithm != nullptr && SubtleFamilyTable::find(algorithm->family).isSecret;
}

void LibraryCryptoRegistry::walkGenerateKey(SubtleDraft &subtle) {
  const auto *algorithm =
      this->algorithmNameAt(subtle, 0, SubtleOperation::GENERATE_KEY, std::nullopt);
  int isPublic = this->isSecretAlgorithm(algorithm) ? 0 : -1;
  this->walkUsages(subtle, 2, algorithm, isPublic, true);
  if (algorithm != nullptr) {
    subtle.target = SubtleFamilyTable::find(algorithm->family).generate;
  }
}

void LibraryCryptoRegistry::walkImportKey(SubtleDraft &subtle) {
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
  auto &declarations = this->state.library.declarations;
  bool isWrappingJson = isJwkText && this->isJsonValue(this->argAt(subtle.call, 1)) &&
                        isa<NameExpr>(subtle.member->object) &&
                        declarations.findLibraryFunction(LibraryFunction::SUBTLE_JWK_OF) != nullptr;
  if (isWrappingJson) {
    subtle.isJwkWrapped = true;
  }
}

void LibraryCryptoRegistry::walkImportFormat(SubtleDraft &subtle,
                                             std::optional<SubtleKeyFormat> format,
                                             const SubtleAlgorithmInfo *algorithm, bool unwraps) {
  if (!format.has_value() || algorithm == nullptr) {
    return;
  }
  const std::vector<SubtleKeyFormat> &formats = SubtleFamilyTable::find(algorithm->family).formats;
  if (llvm::is_contained(formats, *format)) {
    return;
  }
  subtle.isBad = true;
  auto formatName = SubtleKeyFormatTable::getName(*format);
  auto names = SubtleAlgorithms::formatNames(formats);
  std::string message = unwraps ? ErrorTextSubtleImport::unwrap(algorithm->name, formatName, names)
                                : ErrorTextSubtleImport::format(algorithm->name, formatName, names);
  this->state.diagnostics.error(subtle.call->args[0]->loc(), message);
}

int LibraryCryptoRegistry::importedPublicness(std::optional<SubtleKeyFormat> format,
                                              const SubtleAlgorithmInfo *algorithm) const {
  if (this->isSecretAlgorithm(algorithm)) {
    return 0;
  }
  if (format == SubtleKeyFormat::SPKI || format == SubtleKeyFormat::RAW) {
    return 1;
  }

  return format == SubtleKeyFormat::PKCS8 ? 0 : -1;
}

void LibraryCryptoRegistry::walkExportKey(SubtleDraft &subtle) {
  auto format = this->keyFormatAt(subtle, 0);
  if (format.has_value()) {
    subtle.target = format == SubtleKeyFormat::JWK ? LibraryFunction::SUBTLE_EXPORT_JWK
                                                   : LibraryFunction::SUBTLE_EXPORT_BYTES;
  }
}

void LibraryCryptoRegistry::walkDeriveKey(SubtleDraft &subtle) {
  this->algorithmNameAt(subtle, 0, SubtleOperation::DERIVE_BITS, std::nullopt);
  const auto *algorithm =
      this->algorithmNameAt(subtle, 2, SubtleOperation::IMPORT_KEY, std::nullopt);
  if (algorithm == nullptr) {
    return;
  }
  if (!this->isSecretAlgorithm(algorithm)) {
    subtle.isBad = true;
    auto message = ErrorTextSubtleImport::derived(algorithm->name);
    this->state.diagnostics.error(this->argAt(subtle.call, 2)->loc(), message);
    return;
  }
  if (algorithm->family != SubtleFamily::KDF) {
    this->algorithmNameAt(subtle, 2, SubtleOperation::KEY_LENGTH, std::nullopt);
  }

  return this->walkUsages(subtle, 4, algorithm, 0, true);
}

void LibraryCryptoRegistry::walkWrapKey(SubtleDraft &subtle) {
  this->keyFormatAt(subtle, 0);
  this->algorithmNameAt(subtle, 3, SubtleOperation::WRAP_KEY, SubtleOperation::ENCRYPT);
}

void LibraryCryptoRegistry::walkUnwrapKey(SubtleDraft &subtle) {
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

void LibraryCryptoRegistry::walkUsages(SubtleDraft &subtle, size_t index,
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
      subtle.isBad = true;
      auto message = ErrorTextSubtleUsage::unknown(*written);
      this->state.diagnostics.error(element.expr->loc(), message);
      return;
    }
    if (algorithm != nullptr && !llvm::is_contained(allowed, *usage)) {
      subtle.isBad = true;
      auto names = SubtleAlgorithms::usageNames(allowed);
      bool isSecret = this->isSecretAlgorithm(algorithm);
      auto message =
          ErrorTextSubtleUsage::notOf(*written, algorithm->name, isPublic, isSecret, names);
      this->state.diagnostics.error(element.expr->loc(), message);
      return;
    }
  }
  bool isOwnNeeded = creates && algorithm != nullptr && isPublic != 1;
  if (isOwnNeeded) {
    this->walkOwnUsage(subtle, literal, *algorithm, isPublic);
  }
}

std::vector<SubtleKeyUsage>
LibraryCryptoRegistry::allowedUsages(const SubtleAlgorithmInfo *algorithm, int isPublic) const {
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

void LibraryCryptoRegistry::walkOwnUsage(SubtleDraft &subtle, const ArrayLitExpr *literal,
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
  subtle.isBad = true;
  bool isSingle = literal->elements.empty() || isPublic == 0;
  auto names = SubtleAlgorithms::usageNames(own);
  std::string message =
      isSingle ? ErrorTextSubtleUsage::empty(names) : ErrorTextSubtleUsage::pairEmpty(names);
  this->state.diagnostics.error(literal->loc(), message);
}

bool LibraryCryptoRegistry::matchKeyData(const SubtleDraft &subtle) {
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
    this->state.diagnostics.error(bare->loc(), message);
    return false;
  }
  if (!isJwkFormat && this->isJwkObject(type)) {
    auto formatName = SubtleKeyFormatTable::getName(*subtle.keyFormat);
    auto message = ErrorTextSubtleKeyData::bytes(formatName, type->name());
    this->state.diagnostics.error(bare->loc(), message);
    return false;
  }

  return true;
}

const Expr *LibraryCryptoRegistry::withoutImplicitCasts(const Expr *expr) const {
  const Expr *bare = expr;
  auto *castExpr = dyn_cast<CastExpr>(bare);
  while (castExpr != nullptr && castExpr->isImplicit()) {
    bare = castExpr->operand;
    castExpr = dyn_cast<CastExpr>(bare);
  }

  return bare;
}

bool LibraryCryptoRegistry::isHoldingBytes(const Type *type) const {
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

bool LibraryCryptoRegistry::isJwkObject(const Type *type) const {
  return !type->isBytes() && type->isObject() && type->classDecl() == nullptr && !type->isArray() &&
         !type->isTuple();
}

bool LibraryCryptoRegistry::isJsonValue(const Expr *expr) const {
  const Expr *root = expr;
  const auto *inner = this->accessInner(root);
  while (inner != nullptr) {
    root = inner;
    inner = this->accessInner(root);
  }
  auto *name = dyn_cast_or_null<NameExpr>(root);
  if (name != nullptr) {
    VarSymbol *variable = this->state.walk.scopes.findName(name->name).var();
    return variable != nullptr && variable->type != nullptr &&
           variable->type->nonOptional()->isJson();
  }
  auto *call = dyn_cast_or_null<CallExpr>(root);
  auto *member = call != nullptr ? dyn_cast<MemberExpr>(call->callee) : nullptr;
  if (member == nullptr) {
    return false;
  }

  return JsonStaticTable::find(member->name) == JsonStatic::PARSE && member->typeArgs.empty() &&
         this->state.isGlobalName(member->object, GlobalName::JSON);
}

const Expr *LibraryCryptoRegistry::accessInner(const Expr *expr) const {
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

LibraryCryptoRegistry::ResultSubtleCallShape
LibraryCryptoRegistry::checkSubtleShape(const CallExpr *call, llvm::StringRef intrinsicName) const {
  const SubtleIntrinsic *intrinsic = SubtleIntrinsicTable::find(intrinsicName);
  bool isWrong =
      intrinsic == nullptr || call->args.size() != intrinsic->params.size() || call->hasSpread();
  if (isWrong) {
    auto message = ErrorTextIntrinsicCall::wrong(intrinsicName);

    return err(LibraryCryptoRegistrySubtleShapeError{
        .code = LibraryCryptoRegistrySubtleShapeErrorCode::NOT_AN_INTRINSIC,
        .message = message,
    });
  }

  return ok(intrinsic);
}

LibraryCryptoRegistry::ResultAlgorithmChosen
LibraryCryptoRegistry::checkLiteralAlgorithm(const Expr *checked, char kind,
                                             llvm::StringRef intrinsicName) const {
  if (checked != nullptr && (kind == 'm' || kind == 'c') && !isa<IntLitExpr>(checked)) {
    auto message = ErrorTextSubtle::literal(intrinsicName);

    return err(LibraryCryptoRegistryLiteralAlgorithmError{
        .code = LibraryCryptoRegistryLiteralAlgorithmErrorCode::NOT_A_LITERAL,
        .message = message,
    });
  }

  return ok(checked);
}

LibraryCryptoRegistry::ResultSubtleAvailable LibraryCryptoRegistry::checkSubtleAvailable() const {
  VarSymbol *subtle = this->state.walk.scopes.findName(LibraryModuleNames::kSubtleName).var();
  if (subtle == nullptr) {
    auto message = ErrorTextSubtle::noSubtle();

    return err(LibraryCryptoRegistrySubtleAvailableError{
        .code = LibraryCryptoRegistrySubtleAvailableErrorCode::NO_SUBTLE,
        .message = message,
    });
  }

  return ok(subtle);
}

const Type *LibraryCryptoRegistry::subtleResult(char result) const {
  const auto *buffer = Type::bytesType(Type::Bytes::ArrayBuffer);
  switch (result) {
    case 'B':
      return buffer;
    case 'U':
      return Type::optional(buffer);
    case 'I':
      return Type::intTy();
  }

  const auto *keyObject = Type::cryptoType(Type::Crypto::KeyObject);

  return Type::optional(keyObject);
}

}
