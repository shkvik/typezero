#pragma once

#include "compiler/program/library/crypto/crypto_api/crypto_api.h"
#include "compiler/program/library/crypto/crypto_choice_table/crypto_choice_table.h"
#include "compiler/program/library/crypto/crypto_option_table/crypto_option_table.h"
#include "compiler/program/library/crypto/subtle_algorithm_table/subtle_algorithm_table.h"
#include "compiler/program/library/crypto/subtle_intrinsic_table/subtle_intrinsic_table.h"
#include "compiler/program/library/crypto/subtle_key_usage_table/subtle_key_usage_table.h"
#include "compiler/program/library/crypto/subtle_member_table/subtle_member_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/flow/proofs/flow_proofs.h"
#include "compiler/semantic/library/crypto/library_crypto_registry_errors.h"
#include "compiler/semantic/library/crypto/subtle_algorithms/subtle_algorithms.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include <optional>
#include <string>
#include <vector>

namespace tz {

class LibraryCryptoRegistry {
public:
  explicit LibraryCryptoRegistry(SemanticState &owner);

  void afterCryptoNumber();
  void afterCryptoEncoding();
  void cryptoValue(const CryptoValue &task);
  void afterJwkWrap(const AfterJwkWrap &task);
  void afterFillSize(const AfterFillSize &task);
  void finishKeyPair(const FinishKeyPair &task);
  void afterSubtleArg(const AfterSubtleArg &task);
  void checkCryptoNew(const CheckCryptoNew &task);
  void checkX509Parse(const CheckX509Parse &task);
  void afterCryptoData(const AfterCryptoData &task);
  void afterFillOffset(const AfterFillOffset &task);
  void afterFillTarget(const AfterFillTarget &task);
  void checkCryptoCall(const CheckCryptoCall &task);
  void checkSubtleCall(const CheckSubtleCall &task);
  void finishBytesText(const FinishBytesText &task);
  void afterCryptoAsync(const AfterCryptoAsync &task);
  void afterJwkArgument(const AfterJwkArgument &task);
  void checkCryptoValue(const CheckCryptoValue &task);
  void cryptoFinishStep(const CryptoFinishStep &task);
  void afterProvenNumber(const AfterProvenNumber &task);
  void afterSubtleFinish(const AfterSubtleFinish &task);
  void checkCryptoMember(const CheckCryptoMember &task);
  void checkCryptoMethod(const CheckCryptoMethod &task);
  void checkWebCryptoCall(const CheckWebCryptoCall &task);
  void finishCryptoResult(const FinishCryptoResult &task);
  void afterRejectCallback(const AfterRejectCallback &task);
  void afterCryptoKeyObject(const AfterCryptoKeyObject &task);
  void checkSubtleIntrinsic(const CheckSubtleIntrinsic &task);
  void checkWebCryptoMember(const CheckWebCryptoMember &task);
  void checkCryptoModuleMember(const CheckCryptoModuleMember &task);

private:
  using ResultFilledTarget = Result<const Type *, LibraryCryptoRegistryFilledTargetError>;
  using ResultKeyObjectType = Result<const Type *, LibraryCryptoRegistryKeyObjectTypeError>;
  using ResultIssuerChecked = Result<const Expr *, LibraryCryptoRegistryIssuerError>;
  using ResultMembersPresent =
      Result<const ObjectLitExpr *, LibraryCryptoRegistryMembersPresentError>;
  using ResultAlgorithmChosen = Result<const Expr *, LibraryCryptoRegistryLiteralAlgorithmError>;
  using ResultCurveIsKnown = Result<llvm::StringRef, LibraryCryptoRegistryCurveIsKnownError>;
  using ResultCurveNameKnown = Result<llvm::StringRef, LibraryCryptoRegistryKnownCurveError>;
  using ResultShaHashIsKnown = Result<llvm::StringRef, LibraryCryptoRegistryKnownShaHashError>;
  using ResultKeyOptionsGiven = Result<int, LibraryCryptoRegistryKeyOptionsError>;
  using ResultAesLengthValid = Result<double, LibraryCryptoRegistryAesLengthError>;
  using ResultGcmTagLengthOk = Result<double, LibraryCryptoRegistryGcmTagLengthError>;
  using ResultKeyFormatKnown = Result<SubtleKeyFormat, LibraryCryptoRegistryKnownFormatError>;
  using ResultParamsAsObject =
      Result<const SubtleAlgorithmInfo *, LibraryCryptoRegistryParamsAsObjectError>;
  using ResultAlgorithmKnown =
      Result<const SubtleAlgorithmInfo *, LibraryCryptoRegistryKnownAlgorithmError>;
  using ResultSubtleAvailable = Result<VarSymbol *, LibraryCryptoRegistrySubtleAvailableError>;
  using ResultSubtleCallShape =
      Result<const SubtleIntrinsic *, LibraryCryptoRegistrySubtleShapeError>;

  void pushCipher(Expr *arg);
  void pushDroppedCheck(Expr *arg);
  void pushToDouble(Expr *checked);
  void pushLooseArgs(CallExpr *call);
  void pushMd(Expr *arg, DigestUse use);
  void walkWrapKey(SubtleDraft &subtle);
  void walkDeriveKey(SubtleDraft &subtle);
  void walkExportKey(SubtleDraft &subtle);
  void walkImportKey(SubtleDraft &subtle);
  void walkUnwrapKey(SubtleDraft &subtle);
  void pushArg(Expr *arg, const Type *type);
  void pushSubtleObject(MemberExpr *member);
  void startSelfOnly(CryptoPlanDraft draft);
  void startSignOnce(CryptoPlanDraft draft);
  void walkGenerateKey(SubtleDraft &subtle);
  void startCreateKey(CryptoPlanDraft draft);
  void startRandomInt(CryptoPlanDraft draft);
  void pushCryptoConstant(MemberExpr *member);
  void startFinalBytes(CryptoPlanDraft draft);
  void startMethodSign(CryptoPlanDraft draft);
  void startAutoPadding(CryptoPlanDraft draft);
  void startGenerateKey(CryptoPlanDraft draft);
  void failCrypto(const CryptoPlanDraft &draft);
  void walkSubtleArguments(SubtleDraft &subtle);
  void pushEncoding(const CryptoValueItem &item);
  void startArgon2(const CryptoPlanDraft &draft);
  void startDigest(const CryptoPlanDraft &draft);
  void startKnownFunction(CryptoPlanDraft draft);
  void startSetAad(const CryptoPlanDraft &draft);
  void pushSubtleArg(const CryptoValueItem &item);
  void checkNameList(const CryptoPlanDraft &draft);
  void startHashCopy(const CryptoPlanDraft &draft);
  void startX509Name(const CryptoPlanDraft &draft);
  void continueCrypto(const CryptoPlanDraft &draft);
  void startKeyEquals(const CryptoPlanDraft &draft);
  void startKeyExport(const CryptoPlanDraft &draft);
  void checkRandomUuid(const CryptoPlanDraft &draft);
  void continueSubtleCall(const SubtleDraft &subtle);
  void startCheckPrime(const CryptoPlanDraft &draft);
  void startCreateEcdh(const CryptoPlanDraft &draft);
  void startRandomFill(const CryptoPlanDraft &draft);
  void startX509Verify(const CryptoPlanDraft &draft);
  void continueSignOnce(const CryptoPlanDraft &draft);
  void continueRandomInt(const CryptoPlanDraft &draft);
  void startCreateSigner(const CryptoPlanDraft &draft);
  void startCryptoMethod(const CryptoPlanDraft &draft);
  void startDataEncoding(const CryptoPlanDraft &draft);
  void startDiffieHellman(const CryptoPlanDraft &draft);
  void startEcdhPublicKey(const CryptoPlanDraft &draft);
  void startGeneratePrime(const CryptoPlanDraft &draft);
  void continueRandomBytes(const CryptoPlanDraft &draft);
  void checkRejectedSubtleCall(const SubtleDraft &subtle);
  void finishKeyExport(CryptoPlanDraft draft, int format);
  void reportNoMethod(CallExpr *call, MemberExpr *member);
  void startGenerateKeyPair(const CryptoPlanDraft &draft);
  void walkAlgorithmOnly(SubtleDraft &subtle, SubtleOperation operation);
  void startEncodedExport(CryptoPlanDraft draft, Expr *options, int format);
  void finishDigest(CryptoPlanDraft draft, const std::vector<Expr *> &values);
  void finishX509Name(CryptoPlanDraft draft, const std::vector<Expr *> &values);
  void finishKeyDetails(CryptoPlanDraft draft, const std::vector<Expr *> &values);
  void finishX509Verify(CryptoPlanDraft draft, const std::vector<Expr *> &values);
  void finishCall(const CryptoPlanDraft &draft, const std::vector<Expr *> &values);
  void finishHkdf(const CryptoPlanDraft &draft, const std::vector<Expr *> &values);
  void finishHmac(const CryptoPlanDraft &draft, const std::vector<Expr *> &values);
  void finishPair(const CryptoPlanDraft &draft, const std::vector<Expr *> &values);
  void finishPairNumbers(CryptoPlanDraft draft, const std::vector<Expr *> &values);
  void finishArgon(const CryptoPlanDraft &draft, const std::vector<Expr *> &values);
  void finishDataEncoding(CryptoPlanDraft draft, const std::vector<Expr *> &values);
  void finishEncodedExport(CryptoPlanDraft draft, const std::vector<Expr *> &values);
  void finishHashMd(const CryptoPlanDraft &draft, const std::vector<Expr *> &values);
  void finishPbkdf2(const CryptoPlanDraft &draft, const std::vector<Expr *> &values);
  void finishResult(const CryptoPlanDraft &draft, const std::vector<Expr *> &values);
  void finishSetAad(const CryptoPlanDraft &draft, const std::vector<Expr *> &values);
  void finishSignKey(const CryptoPlanDraft &draft, const std::vector<Expr *> &values);
  void finishHashOnce(const CryptoPlanDraft &draft, const std::vector<Expr *> &values);
  void finishSignOnce(const CryptoPlanDraft &draft, const std::vector<Expr *> &values);
  void reportUnprovenUpTo(const FlowProofs::ResultUpToWasProven &upTo, llvm::SMLoc at);
  void finishCreateKey(const CryptoPlanDraft &draft, const std::vector<Expr *> &values);
  void finishKeyEquals(const CryptoPlanDraft &draft, const std::vector<Expr *> &values);
  void finishRandomInt(const CryptoPlanDraft &draft, const std::vector<Expr *> &values);
  void finishSecretKey(const CryptoPlanDraft &draft, const std::vector<Expr *> &values);
  void finishCheckPrime(const CryptoPlanDraft &draft, const std::vector<Expr *> &values);
  void finishRandomFill(const CryptoPlanDraft &draft, const std::vector<Expr *> &values);
  void finishScryptCost(const CryptoPlanDraft &draft, const std::vector<Expr *> &values);
  void finishTimingSafe(const CryptoPlanDraft &draft, const std::vector<Expr *> &values);
  void startSignOnceKey(const CryptoPlanDraft &draft, const std::vector<Expr *> &values);
  void finishBytesOrText(const CryptoPlanDraft &draft, const std::vector<Expr *> &values);
  void finishGenerateKey(const CryptoPlanDraft &draft, const std::vector<Expr *> &values);
  void finishPairModulus(const CryptoPlanDraft &draft, const std::vector<Expr *> &values);
  void finishPairOptions(const CryptoPlanDraft &draft, const std::vector<Expr *> &values);
  void finishRandomBytes(const CryptoPlanDraft &draft, const std::vector<Expr *> &values);
  void finishSignOnceKey(const CryptoPlanDraft &draft, const std::vector<Expr *> &values);
  void cryptoCall(CallExpr *call, CryptoFn fn, std::vector<Expr *> args, const Type *type);
  void finishArgonOptions(const CryptoPlanDraft &draft, const std::vector<Expr *> &values);
  void finishMethodVerify(const CryptoPlanDraft &draft, const std::vector<Expr *> &values);
  void finishCipherOptions(const CryptoPlanDraft &draft, const std::vector<Expr *> &values);
  void finishDiffieHellman(const CryptoPlanDraft &draft, const std::vector<Expr *> &values);
  void finishGeneratePrime(const CryptoPlanDraft &draft, const std::vector<Expr *> &values);
  void finishScryptOptions(const CryptoPlanDraft &draft, const std::vector<Expr *> &values);
  void continueMethodVerify(const CryptoPlanDraft &draft, const std::vector<Expr *> &values);
  void reportUnprovenBetween(const FlowProofs::ResultBetweenProven &between, llvm::SMLoc at);
  void startRejectCallback(const CryptoPlanDraft &draft, size_t index, llvm::StringRef hint);
  void finishSubtleIntrinsic(const CryptoPlanDraft &draft, const std::vector<Expr *> &values);
  void addEncodingItems(const KeyEncodingDraft &encoding, std::vector<CryptoValueItem> &items);
  void pushCryptoResult(CallExpr *call, CryptoFn fn, std::vector<Expr *> args, const Type *type);
  void finishCrypto(const CryptoPlanDraft &draft, CryptoFinish finish,
                    const std::vector<Expr *> &values);
  void startSyncCall(CallExpr *call, llvm::StringRef functionName, llvm::StringRef shown,
                     llvm::SMLoc loc);
  void pushPlan(const CryptoPlanDraft &draft, CryptoFinish finish,
                const std::vector<CryptoValueItem> &items);
  void startBytesOrText(CryptoPlanDraft draft, const std::vector<Expr *> &fnArgs, size_t index,
                        bool isFallible);
  void startSignKey(CryptoPlanDraft draft, std::vector<Expr *> prefix, Expr *keyArg, bool isPriv,
                    CryptoFinish after);
  void walkUsages(SubtleDraft &subtle, size_t index, const SubtleAlgorithmInfo *algorithm,
                  int isPublic, bool creates);
  void walkOwnUsage(SubtleDraft &subtle, const ArrayLitExpr *literal,
                    const SubtleAlgorithmInfo &algorithm, int isPublic);
  void reportAfterValues(const std::vector<CryptoValueItem> &items, size_t skipped, llvm::SMLoc loc,
                         const std::string &message);
  void walkImportFormat(SubtleDraft &subtle, std::optional<SubtleKeyFormat> format,
                        const SubtleAlgorithmInfo *algorithm, bool unwraps);


  bool isJsonValue(const Expr *expr) const;
  bool isJwkObject(const Type *type) const;
  bool isHoldingBytes(const Type *type) const;
  bool matchKeyData(const SubtleDraft &subtle);
  bool reportBadCurve(const ObjectLitExpr *literal);
  bool isDecipher(llvm::ArrayRef<Expr *> args) const;
  bool readArgonOptions(const CryptoPlanDraft &draft);
  bool readKeyPairOptions(const CryptoPlanDraft &draft);
  bool flag(llvm::ArrayRef<Expr *> args, size_t i) const;
  bool isProvenFillOffset(const Expr *at, const Expr *target);
  bool isSecretAlgorithm(const SubtleAlgorithmInfo *algorithm) const;
  bool isPresent(const std::vector<Expr *> &args, size_t index) const;
  bool matchWhole(const Expr *arg, llvm::StringRef name, double least);
  bool isCryptoData(CryptoDataKinds kinds, const Type *candidate) const;
  bool isProvenFillSize(const Expr *count, const Expr *at, const Expr *target);
  bool matchCipherLengths(const Expr *cipher, const Expr *key, const Expr *iv);
  bool reportBadHash(const ObjectLitExpr *literal, llvm::StringRef dictionary);
  bool hasCryptoArity(const CryptoPlanDraft &draft, unsigned min, unsigned max);
  bool hasMethodArity(const CryptoPlanDraft &draft, unsigned min, unsigned max);
  bool reportBadGcmTag(const ObjectLitExpr *literal, const SubtleAlgorithmInfo &algorithm);
  bool readOptionsAt(const CryptoPlanDraft &draft, size_t index,
                     llvm::ArrayRef<CryptoOption> allowed);
  bool readCryptoOptions(Expr *arg, llvm::StringRef functionName,
                         llvm::ArrayRef<CryptoOption> allowed);
  bool reportBadAesLength(const ObjectLitExpr *literal, const SubtleAlgorithmInfo &algorithm,
                          SubtleOperation operation);


  CryptoPlanDraft functionDraft(CallExpr *call, llvm::SMLoc loc) const;
  CryptoPlanDraft withResult(const CryptoPlanDraft &draft, CryptoFn fn, const Type *type) const;


  CryptoValueItem cipherItem(Expr *arg) const;
  CryptoValueItem numberItem(Expr *arg) const;
  CryptoValueItem literalItem(Expr *expr) const;
  CryptoValueItem undefinedItem(llvm::SMLoc loc);
  CryptoValueItem toDoubleItem(Expr *checked) const;
  CryptoValueItem bufferEncodingItem(Expr *arg) const;
  CryptoValueItem mdItem(Expr *arg, DigestUse use) const;
  CryptoValueItem argItem(Expr *arg, const Type *type) const;
  CryptoValueItem boolLiteralItem(llvm::SMLoc loc, bool value);
  CryptoValueItem intLiteralItem(llvm::SMLoc loc, int64_t value);
  CryptoValueItem doubleLiteralItem(llvm::SMLoc loc, double value);
  CryptoValueItem valueItem(CryptoValueKind kind, Expr *arg) const;
  CryptoValueItem checkedItem(Expr *arg, const Type *expected) const;
  CryptoValueItem keyObjectItem(Expr *arg, llvm::StringRef what) const;
  CryptoValueItem stringLiteralItem(llvm::SMLoc loc, llvm::StringRef text);
  CryptoValueItem dataItem(Expr *arg, llvm::StringRef what, CryptoDataKinds kinds) const;
  CryptoValueItem encodingItem(CallExpr *call, size_t index, llvm::StringRef name,
                               int fallback) const;
  CryptoValueItem provenItem(Expr *arg, int64_t low, std::optional<int64_t> high,
                             llvm::StringRef what) const;


  Expr *cryptoMember(MemberExpr *member);
  Expr *cryptoUndefined(llvm::SMLoc loc);
  Expr *reportOnlyCalled(MemberExpr *member);
  Expr *cryptoText(Expr *made, Expr *encoding);
  Expr *cryptoBool(llvm::SMLoc loc, bool value);
  Expr *cryptoInt(llvm::SMLoc loc, int64_t value);
  Expr *cryptoDouble(llvm::SMLoc loc, double value);
  Expr *readFingerprint(MemberExpr *member, int bits);
  Expr *argAt(const CallExpr *call, size_t index) const;
  Expr *cryptoString(llvm::SMLoc loc, llvm::StringRef text);
  Expr *cryptoOption(const Expr *arg, CryptoOption option) const;
  Expr *literalMd(Expr *arg, const std::string &text, DigestUse use);
  Expr *reportCryptoMember(MemberExpr *member, const std::string &message);
  Expr *pairOption(const CryptoPlanDraft &draft, CryptoOption option) const;
  Expr *readCryptoProperty(MemberExpr *member, CryptoFn fn, const Type *result);
  Expr *optionAt(const CryptoPlanDraft &draft, size_t index, CryptoOption option) const;


  int cryptoChoice(Expr *arg, llvm::StringRef what, CryptoChoice choice);
  int importedPublicness(std::optional<SubtleKeyFormat> format,
                         const SubtleAlgorithmInfo *algorithm) const;


  std::optional<double> knownNumber(const Expr *arg) const;
  std::optional<double> plainNumber(const Expr *arg) const;
  std::optional<double> numberLiteral(const Expr *expr) const;


  const Type *subtleResult(char result) const;
  const Type *encodedKeyType(int format) const;


  const ObjectLitExpr::Property *memberProperty(const ObjectLitExpr *literal,
                                                SubtleMember member) const;
  const ObjectLitExpr::Property *propertyNamed(const ObjectLitExpr *literal,
                                               llvm::StringRef name) const;


  const SubtleAlgorithmInfo *literalAlgorithm(const Expr *arg, SubtleOperation operation,
                                              std::optional<SubtleOperation> fallback, bool &isBad);
  const SubtleAlgorithmInfo *algorithmNameAt(SubtleDraft &subtle, size_t index,
                                             SubtleOperation operation,
                                             std::optional<SubtleOperation> fallback);


  const Expr *accessInner(const Expr *expr) const;
  const Expr *withoutImplicitCasts(const Expr *expr) const;
  const Expr *algorithmAt(const CallExpr *call, size_t index) const;
  const Expr *algorithmNameExpr(const Expr *arg, const ObjectLitExpr *literal) const;


  std::string numberText(double value) const;
  ResultSubtleAvailable checkSubtleAvailable() const;
  std::vector<Expr *> takeCheckedValues(size_t count);
  ResultAesLengthValid checkAesLength(double bits) const;
  std::optional<int64_t> bytesOf(const Expr *expr) const;
  ResultIssuerChecked checkIssuer(const Expr *other) const;
  ResultGcmTagLengthOk checkGcmTagLength(double bits) const;
  std::optional<std::string> literalOf(const Expr *expr) const;
  std::optional<size_t> directMdIndex(llvm::StringRef name) const;
  ResultCurveIsKnown checkCurveIsKnown(llvm::StringRef curve) const;
  ResultCurveNameKnown checkKnownCurve(llvm::StringRef curve) const;
  ResultKeyFormatKnown checkKnownFormat(llvm::StringRef format) const;
  llvm::StringRef errorNameOf(CryptoFn fn, llvm::ArrayRef<Expr *> args) const;
  std::optional<SubtleKeyFormat> keyFormatAt(SubtleDraft &subtle, size_t index);
  std::vector<CryptoValueItem> literalItems(const std::vector<Expr *> &values) const;
  ResultKeyObjectType checkKeyObjectType(const Type *type, llvm::StringRef what) const;
  std::vector<llvm::StringRef> optionNames(llvm::ArrayRef<CryptoOption> options) const;
  std::optional<KeyEncodingDraft> readKeyEncoding(const CryptoPlanDraft &draft, bool isPrivate);
  ResultShaHashIsKnown checkKnownShaHash(llvm::StringRef hash, llvm::StringRef dictionary) const;
  ResultSubtleCallShape checkSubtleShape(const CallExpr *call, llvm::StringRef intrinsicName) const;
  std::vector<SubtleKeyUsage> allowedUsages(const SubtleAlgorithmInfo *algorithm,
                                            int isPublic) const;
  ResultKeyOptionsGiven checkKeyOptions(int which, llvm::StringRef type,
                                        const CryptoPlanDraft &draft) const;
  ResultAlgorithmChosen checkLiteralAlgorithm(const Expr *checked, char kind,
                                              llvm::StringRef intrinsicName) const;
  ResultFilledTarget checkFilledTarget(const Type *type, CryptoSyncFunction function,
                                       llvm::StringRef shown) const;
  ResultAlgorithmKnown checkKnownAlgorithm(llvm::StringRef written, SubtleOperation operation,
                                           std::optional<SubtleOperation> fallback) const;
  ResultParamsAsObject checkParamsAsObject(const SubtleAlgorithmInfo &algorithm,
                                           SubtleOperation operation,
                                           const Dictionary &dictionary) const;
  ResultMembersPresent checkMembersPresent(const SubtleAlgorithmInfo &algorithm,
                                           SubtleOperation operation, const Dictionary &dictionary,
                                           const ObjectLitExpr *literal) const;

  SemanticState &state;
};

}
