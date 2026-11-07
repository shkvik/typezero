#pragma once

#include "compiler/program/types/field/field.h"
#include "compiler/program/types/intl_kind/intl_kind.h"
#include "compiler/program/types/overload/overload.h"
#include "compiler/program/types/type_op/type_op.h"
#include "compiler/program/types/union_slot/union_slot.h"
#include "compiler/union/union/union.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

namespace tz {

struct ClassDecl;
struct Interned;

class Type {
public:
  enum class Kind {
    Error,
    Void,
    Undefined,
    Bool,
    Int,
    Double,
    Decimal,
    String,
    Optional,
    Array,
    Function,
    Object,
    Param,
    Result,
    Never,
    Iterator,
    Json,
    Dict,
    Literal,
    Union,
    Promise,
    Unknown,
    ClassRef,
  };
  enum class DictKind { Record, Map, Set };

  Kind kind() const {
    return data_.kind();
  }
  bool isError() const {
    return kind() == Kind::Error;
  }
  bool isVoid() const {
    return kind() == Kind::Void;
  }
  bool isUndefined() const {
    return kind() == Kind::Undefined;
  }
  bool isBool() const {
    return kind() == Kind::Bool;
  }
  bool isInt() const;
  bool isString() const {
    return kind() == Kind::String;
  }
  bool isInteger() const {
    return kind() == Kind::Int;
  }
  bool isUnsigned() const;
  bool isWrapping() const;
  int64_t intMin() const;
  uint64_t intMax() const;
  bool holds(const Type *other) const {
    return other->isInteger() && isInteger() && other->intMin() >= intMin() &&
           other->intMax() <= intMax();
  }
  bool isFloat() const {
    return kind() == Kind::Double;
  }
  bool isNumeric() const { return isInteger() || isFloat(); }
  bool isDecimal() const {
    return kind() == Kind::Decimal;
  }
  bool isOptional() const {
    return kind() == Kind::Optional;
  }
  bool isArray() const {
    return kind() == Kind::Array;
  }
  bool isFunction() const {
    return kind() == Kind::Function;
  }
  bool isObject() const {
    return kind() == Kind::Object;
  }
  bool isTuple() const;
  bool isParam() const {
    return kind() == Kind::Param;
  }
  bool isResult() const {
    return kind() == Kind::Result;
  }
  bool holdsFunctions() const;
  bool holdsObjects() const;
  bool holdsDates() const;
  bool holdsResults() const;
  bool holdsPromises() const;
  bool isNever() const {
    return kind() == Kind::Never;
  }
  bool isClass() const;
  const ClassDecl *classDecl() const;
  bool isIterator() const {
    return kind() == Kind::Iterator;
  }
  bool isJson() const {
    return kind() == Kind::Json;
  }
  bool isDict() const {
    return kind() == Kind::Dict;
  }
  bool isUnknown() const {
    return kind() == Kind::Unknown;
  }
  bool isClassRef() const {
    return kind() == Kind::ClassRef;
  }
  const ClassDecl *classRef() const;
  bool isConstructor() const;
  const Type *constructed() const { return isConstructor() ? inner_ : nullptr; }
  bool isRecord() const;
  bool isMap() const;
  bool isSet() const;
  DictKind dictKind() const;
  const Type *keyType() const { return isDict() ? params_[0] : nullptr; }
  const Type *valueType() const { return isDict() ? inner_ : nullptr; }
  bool isLiteral() const {
    return kind() == Kind::Literal;
  }
  bool isUnion() const {
    return kind() == Kind::Union;
  }

  const Type *literalBase() const { return isLiteral() ? inner_ : nullptr; }
  const std::u16string &literalText() const;
  double literalNumber() const;
  bool isStringLiteral() const { return isLiteral() && inner_->isString(); }
  unsigned atom() const;
  llvm::ArrayRef<const Type *> members() const {
    return isUnion() ? llvm::ArrayRef<const Type *>(params_) : llvm::ArrayRef<const Type *>();
  }
  llvm::ArrayRef<UnionSlot> slots() const;
  bool isTagged() const;
  const Type *slotPart(UnionSlot slot) const;
  unsigned id() const { return id_; }
  bool isPromise() const {
    return kind() == Kind::Promise;
  }
  const Type *promised() const { return isPromise() ? inner_ : nullptr; }
  bool isTimer() const;
  bool isImmediate() const;
  enum class Runtime : uint8_t {
    None,
    FileHandle,
    Readline,
    ReadlinePromises,
    AsyncIterator,
  };
  Runtime runtimeKind() const;
  bool isRuntimeObject() const;
  bool isAsyncIterator() const;
  enum class Crypto { None, Hash, Hmac, Cipheriv, Decipheriv, KeyObject, Sign, Verify, Ecdh, X509 };
  Crypto crypto() const;
  bool isCrypto() const;

  const Type *inner() const { return inner_; }
  llvm::ArrayRef<const Type *> params() const { return params_; }
  size_t minArgs() const;
  bool hasRest() const;
  bool hasThis() const;
  const Type *ret() const { return inner_; }
  const Type *okType() const { return inner_; }
  const Type *errType() const;
  const Type *nonOptional() const { return isOptional() ? inner_ : this; }
  llvm::ArrayRef<Field> fields() const;
  const Field *field(llvm::StringRef name) const;
  llvm::ArrayRef<Overload> overloads() const;
  const Overload *overload(llvm::StringRef name) const;
  bool isIncomplete() const { return incomplete_; }
  bool isAnyObject() const;
  bool isRegExp() const;
  bool isMatch() const;
  bool isDate() const;
  bool isDateFormat() const;
  bool isIntl() const;
  IntlKind intlKind() const;
  bool isWeakRef() const;
  const Type *weakTarget() const;
  enum class Bytes {
    None,
    Uint8,
    Int8,
    Uint8Clamped,
    Int16,
    Uint16,
    Int32,
    Uint32,
    Float16,
    Float32,
    Float64,
    Buffer,
    ArrayBuffer,
    DataView,
    TextEncoder,
    TextDecoder,
    FatalTextDecoder,
  };
  Bytes bytes() const;
  bool isBytes() const;
  bool isTypedArray() const;
  bool isBuffer() const;
  bool isByteView() const;
  const Type *elementType() const;
  unsigned elementSize() const;
  const Type *accessType() const;
  bool elementRange(int64_t &lo, int64_t &hi) const;
  int matchGroups() const;
  bool captureMandatory(unsigned k) const;
  const Type *constraint() const { return isParam() ? inner_ : nullptr; }
  llvm::ArrayRef<const Type *> alternatives() const {
    return isParam() ? llvm::ArrayRef<const Type *>(params_) : llvm::ArrayRef<const Type *>();
  }
  const Type *iterated() const;
  void setIterated(const Type *element);
  bool isListParam() const;
  void setList();
  static const Type *typeOp(TypeOp op, llvm::ArrayRef<const Type *> operands);
  static const Type *indexedAccess(const Type *object, const Type *key) {
    return typeOp(TypeOp::Index, {object, key});
  }
  static const Type *mapped(const Type *key, const Type *keys, const Type *value, int optional,
                            int readonly, const Type *source, const Type *rename = nullptr);
  TypeOp typeOp() const;
  llvm::ArrayRef<const Type *> operands() const;
  int mappedOptional() const;
  int mappedReadonly() const;
  const Type *yielded() const { return inner_; }
  const Type *returned() const { return isIterator() || isAsyncIterator() ? params_[0] : nullptr; }
  const Type *sent() const { return isIterator() || isAsyncIterator() ? params_[1] : nullptr; }
  llvm::ArrayRef<const Type *> bases() const { return bases_; }
  void addBase(const Type *base);
  bool isBase() const { return isBase_; }
  bool isSubtypeOf(const Type *base) const;
  bool isManaged() const;
  bool hasZeroValue() const;

  unsigned bitWidth() const;
  llvm::StringRef name() const { return name_; }

  static const Type *error();
  static const Type *voidTy();
  static const Type *undefinedTy();
  static const Type *boolTy();
  static const Type *intTy();
  static const Type *integer(unsigned bits, bool isUnsigned);
  static const Type *wrapping(unsigned bits);
  static llvm::ArrayRef<const Type *> integers();
  static const Type *doubleTy();
  static const Type *decimalTy();
  static const Type *string();
  static const Type *json();
  static const Type *optional(const Type *type);
  static const Type *array(const Type *element);
  static const Type *function(llvm::ArrayRef<const Type *> params, size_t minArgs, bool hasRest,
                              const Type *ret, bool hasThis = false);
  static const Type *object(std::vector<Field> fields, llvm::StringRef name = {},
                            std::vector<Overload> overloads = {});
  static Type *declareObject(llvm::StringRef name);
  void defineFields(std::vector<Field> fields, std::vector<Overload> overloads = {});
  static Type *declareUnion(llvm::StringRef name);
  void defineMembers(const Type *of);
  static const Type *tuple(llvm::ArrayRef<const Type *> elements, bool readonly = false);
  static const Type *argumentList(llvm::ArrayRef<const Type *> elements, size_t min,
                                  const Type *rest);
  size_t tupleMin() const;
  const Type *tupleRest() const;
  static Type *param(llvm::StringRef name);
  void setConstraint(const Type *constraint, std::vector<const Type *> alternatives);
  static const Type *anyObject();
  static const Type *never();
  static const Type *unknown();
  static const Type *classRef(const ClassDecl *cls, llvm::StringRef name);
  static const Type *constructorOf(const Type *instance);
  static const Type *result(const Type *ok, const Type *err);
  static Type *classType(llvm::StringRef name, const ClassDecl *decl);
  void rename(std::string name) { name_ = std::move(name); }
  static void nameUnion(const Type *type, llvm::StringRef name);
  static void swapClasses(Type *a, Type *b);
  void forgetFields();
  void replaceBase(const Type *from, const Type *to) {
    for (const Type *&base : bases_) {
      if (base == from) {
        base = to;
      }
    }
  }
  llvm::ArrayRef<const Type *> typeArgs() const;
  void setTypeArgs(std::vector<const Type *> args);
  static const Type *iterator(const Type *yielded, const Type *returned = voidTy(),
                              const Type *sent = voidTy());
  static const Type *regExp();
  static const Type *date();
  static const Type *dateFormat();
  static const Type *intl(IntlKind kind);
  static const Type *weakRef(const Type *target);
  static const Type *dict(DictKind kind, const Type *key, const Type *value);
  static const Type *promise(const Type *value);
  static const Type *timeout();
  static const Type *immediate();
  static const Type *cryptoType(Crypto kind);
  static const Type *bytesType(Bytes kind);
  static const Type *runtimeObject(Runtime kind);
  static const Type *asyncIterator(const Type *yielded, const Type *returned = voidTy(),
                                   const Type *sent = voidTy());
  static const Type *match(int groups, std::vector<bool> mandatory, const Type *named,
                           bool indices);
  static const Type *stringLiteral(const std::u16string &text);
  static const Type *stringPattern(std::vector<std::u16string> texts,
                                   std::vector<const Type *> holes);
  bool isPattern() const;
  llvm::ArrayRef<std::u16string> patternTexts() const;
  llvm::ArrayRef<const Type *> patternHoles() const;
  static const Type *numberLiteral(double value, const Type *base);
  static const Type *boolLiteral(bool value);
  static llvm::ArrayRef<const Type *> stringLiterals();
  static const Type *unionOf(llvm::ArrayRef<const Type *> types);

  static const Type *fromName(llvm::StringRef name);

private:
  using ObjectKey = std::vector<std::tuple<std::string, const Type *, int>>;

  static Interned &interned();
  static ObjectKey objectKey(llvm::ArrayRef<Field> fields, llvm::ArrayRef<Overload> overloads);
  static std::string quotedText(const std::u16string &text);
  static std::string numberText(double value);
  static std::string utf8Text(const std::u16string &text);
  enum class ObjectKind {
    ORDINARY_OBJECT,
    ANY_OBJECT,
    REG_EXP_OBJECT,
    MATCH_OBJECT,
    DATE_OBJECT,
    INTL_OBJECT,
    WEAK_REF_OBJECT,
    TIMEOUT_OBJECT,
    IMMEDIATE_OBJECT,
    CRYPTO_OBJECT,
    BYTES_OBJECT,
    RUNTIME_OBJECT,
  };

  struct OrdinaryObject {
    static constexpr ObjectKind kind = ObjectKind::ORDINARY_OBJECT;
  };

  struct AnyObject {
    static constexpr ObjectKind kind = ObjectKind::ANY_OBJECT;
  };

  struct RegExpObject {
    static constexpr ObjectKind kind = ObjectKind::REG_EXP_OBJECT;
  };

  struct MatchObject {
    static constexpr ObjectKind kind = ObjectKind::MATCH_OBJECT;
    int groups = -1;
    std::vector<bool> mandatory;
  };

  struct DateObject {
    static constexpr ObjectKind kind = ObjectKind::DATE_OBJECT;
  };

  struct IntlObject {
    static constexpr ObjectKind kind = ObjectKind::INTL_OBJECT;
    IntlKind intl = IntlKind::None;
  };

  struct WeakRefObject {
    static constexpr ObjectKind kind = ObjectKind::WEAK_REF_OBJECT;
  };

  struct TimeoutObject {
    static constexpr ObjectKind kind = ObjectKind::TIMEOUT_OBJECT;
  };

  struct ImmediateObject {
    static constexpr ObjectKind kind = ObjectKind::IMMEDIATE_OBJECT;
  };

  struct CryptoObject {
    static constexpr ObjectKind kind = ObjectKind::CRYPTO_OBJECT;
    Crypto crypto = Crypto::None;
  };

  struct BytesObject {
    static constexpr ObjectKind kind = ObjectKind::BYTES_OBJECT;
    Bytes bytes = Bytes::None;
  };

  struct RuntimeObject {
    static constexpr ObjectKind kind = ObjectKind::RUNTIME_OBJECT;
    Runtime runtime = Runtime::None;
  };

  using ObjectForm = Union<OrdinaryObject, AnyObject, RegExpObject, MatchObject, DateObject,
                           IntlObject, WeakRefObject, TimeoutObject, ImmediateObject, CryptoObject,
                           BytesObject, RuntimeObject>;

  struct ErrorData {
    static constexpr Kind kind = Kind::Error;
  };

  struct VoidData {
    static constexpr Kind kind = Kind::Void;
  };

  struct UndefinedData {
    static constexpr Kind kind = Kind::Undefined;
  };

  struct BoolData {
    static constexpr Kind kind = Kind::Bool;
  };

  struct IntData {
    static constexpr Kind kind = Kind::Int;
    unsigned bits = 0;
    bool isUnsigned = false;
    bool wrapping = false;
  };

  struct DoubleData {
    static constexpr Kind kind = Kind::Double;
  };

  struct DecimalData {
    static constexpr Kind kind = Kind::Decimal;
  };

  struct StringData {
    static constexpr Kind kind = Kind::String;
    bool pattern = false;
    std::vector<std::u16string> texts;
  };

  struct OptionalData {
    static constexpr Kind kind = Kind::Optional;
  };

  struct ArrayData {
    static constexpr Kind kind = Kind::Array;
  };

  struct FunctionData {
    static constexpr Kind kind = Kind::Function;
    size_t minArgs = 0;
    bool hasRest = false;
    bool hasThis = false;
  };

  struct ObjectData {
    static constexpr Kind kind = Kind::Object;
    std::vector<Field> fields;
    std::vector<Overload> overloads;
    const ClassDecl *cls = nullptr;
    bool tuple = false;
    size_t tupleMin = 0;
    const Type *tupleRest = nullptr;
    std::vector<const Type *> typeArgs;
    ObjectForm form = OrdinaryObject{};
  };

  struct ParamData {
    static constexpr Kind kind = Kind::Param;
    TypeOp op = TypeOp::None;
    std::vector<const Type *> operands;
    int mappedOptional = 0;
    int mappedReadonly = 0;
    const Type *iterated = nullptr;
    bool list = false;
  };

  struct ResultData {
    static constexpr Kind kind = Kind::Result;
    const Type *error = nullptr;
  };

  struct NeverData {
    static constexpr Kind kind = Kind::Never;
  };

  struct IteratorData {
    static constexpr Kind kind = Kind::Iterator;
  };

  struct JsonData {
    static constexpr Kind kind = Kind::Json;
  };

  struct DictData {
    static constexpr Kind kind = Kind::Dict;
    DictKind dictKind = DictKind::Record;
  };

  struct LiteralData {
    static constexpr Kind kind = Kind::Literal;
    std::u16string text;
    double number = 0;
    unsigned atom = 0;
  };

  struct UnionData {
    static constexpr Kind kind = Kind::Union;
    std::vector<UnionSlot> slots;
  };

  struct PromiseData {
    static constexpr Kind kind = Kind::Promise;
  };

  struct UnknownData {
    static constexpr Kind kind = Kind::Unknown;
  };

  struct ClassRefData {
    static constexpr Kind kind = Kind::ClassRef;
    const ClassDecl *refClass = nullptr;
  };

  using Data = Union<ErrorData, VoidData, UndefinedData, BoolData, IntData, DoubleData, DecimalData,
                     StringData, OptionalData, ArrayData, FunctionData, ObjectData, ParamData,
                     ResultData, NeverData, IteratorData, JsonData, DictData, LiteralData,
                     UnionData, PromiseData, UnknownData, ClassRefData>;

  static Data dataFor(Kind kind);
  const ObjectForm *objectForm() const;

  Type(Kind kind, std::string name, const Type *inner = nullptr)
      : data_(dataFor(kind)),
        inner_(inner),
        name_(std::move(name)),
        id_(++count_) {}
  static unsigned count_;

  Data data_;
  const Type *inner_;
  std::string name_;
  std::vector<const Type *> params_;
  bool incomplete_ = false;
  std::vector<const Type *> bases_;
  bool isBase_ = false;
  unsigned id_;
};

}
