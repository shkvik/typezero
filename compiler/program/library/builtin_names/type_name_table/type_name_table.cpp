#include "compiler/program/library/builtin_names/type_name_table/type_name_table.h"

namespace tz {

namespace {

const TypeNameInfo kTypeNames[] = {
    {
        .name = "Array",
        .type = TypeName::ARRAY,
        .form = ArrayTypeName{},
    },
    {
        .name = "ArrayBuffer",
        .type = TypeName::ARRAY_BUFFER,
        .form = PlainTypeName{},
    },
    {
        .name = "ArrayBufferLike",
        .type = TypeName::ARRAY_BUFFER_LIKE,
        .form = PlainTypeName{},
    },
    {
        .name = "AsyncGenerator",
        .type = TypeName::ASYNC_GENERATOR,
        .form = IteratorTypeName{.async = true},
    },
    {
        .name = "AsyncIterable",
        .type = TypeName::ASYNC_ITERABLE,
        .form = IteratorTypeName{.async = true},
    },
    {
        .name = "AsyncIterableIterator",
        .type = TypeName::ASYNC_ITERABLE_ITERATOR,
        .form = IteratorTypeName{.async = true},
    },
    {
        .name = "AsyncIterator",
        .type = TypeName::ASYNC_ITERATOR,
        .form = IteratorTypeName{.async = true},
    },
    {
        .name = "bigint",
        .type = TypeName::BIGINT,
        .form = PlainTypeName{},
    },
    {
        .name = "BigInt64Array",
        .type = TypeName::BIG_INT64_ARRAY,
        .form = PlainTypeName{},
    },
    {
        .name = "BigUint64Array",
        .type = TypeName::BIG_UINT64_ARRAY,
        .form = PlainTypeName{},
    },
    {
        .name = "boolean",
        .type = TypeName::BOOLEAN,
        .form = PlainTypeName{},
    },
    {
        .name = "Capitalize",
        .type = TypeName::CAPITALIZE,
        .form =
            UtilityTypeName{
                .op = TypeOp::Capitalize,
                .argumentCount = 1,
                .operand = UtilityOperand::TEXT_TYPE,
            },
    },
    {
        .name = "Constructor",
        .type = TypeName::CONSTRUCTOR,
        .form = PlainTypeName{},
    },
    {
        .name = "ConstructorArgs",
        .type = TypeName::CONSTRUCTOR_ARGS,
        .form = PlainTypeName{},
    },
    {
        .name = "Date",
        .type = TypeName::DATE,
        .form = PlainTypeName{},
    },
    {
        .name = "Err",
        .type = TypeName::ERR,
        .form = ResultTypeName{},
    },
    {
        .name = "Exclude",
        .type = TypeName::EXCLUDE,
        .form =
            UtilityTypeName{
                .op = TypeOp::Exclude,
                .argumentCount = 2,
                .operand = UtilityOperand::ANY_TYPE,
            },
    },
    {
        .name = "Extract",
        .type = TypeName::EXTRACT,
        .form =
            UtilityTypeName{
                .op = TypeOp::Extract,
                .argumentCount = 2,
                .operand = UtilityOperand::ANY_TYPE,
            },
    },
    {
        .name = "Generator",
        .type = TypeName::GENERATOR,
        .form = IteratorTypeName{.async = false},
    },
    {
        .name = "Immediate",
        .type = TypeName::IMMEDIATE,
        .form = PlainTypeName{},
    },
    {
        .name = "NodeJS.Immediate",
        .type = TypeName::IMMEDIATE,
        .form = PlainTypeName{},
    },
    {
        .name = "InstanceType",
        .type = TypeName::INSTANCE_TYPE,
        .form = PlainTypeName{},
    },
    {
        .name = "Iterable",
        .type = TypeName::ITERABLE,
        .form = PlainTypeName{},
    },
    {
        .name = "IterableIterator",
        .type = TypeName::ITERABLE_ITERATOR,
        .form = IteratorTypeName{.async = false},
    },
    {
        .name = "Iterator",
        .type = TypeName::ITERATOR,
        .form = IteratorTypeName{.async = false},
    },
    {
        .name = "IteratorResult",
        .type = TypeName::ITERATOR_RESULT,
        .form = PlainTypeName{},
    },
    {
        .name = "Json",
        .type = TypeName::JSON,
        .form = PlainTypeName{},
    },
    {
        .name = "long",
        .type = TypeName::LONG,
        .form = PlainTypeName{},
    },
    {
        .name = "Lowercase",
        .type = TypeName::LOWERCASE,
        .form =
            UtilityTypeName{
                .op = TypeOp::Lowercase,
                .argumentCount = 1,
                .operand = UtilityOperand::TEXT_TYPE,
            },
    },
    {
        .name = "Map",
        .type = TypeName::MAP,
        .form = CollectionTypeName{.collection = Collection::MAP},
    },
    {
        .name = "MethodDescriptor",
        .type = TypeName::METHOD_DESCRIPTOR,
        .form = PlainTypeName{},
    },
    {
        .name = "never",
        .type = TypeName::NEVER,
        .form = PlainTypeName{},
    },
    {
        .name = "NonNullable",
        .type = TypeName::NON_NULLABLE,
        .form =
            UtilityTypeName{
                .op = TypeOp::NonNullable,
                .argumentCount = 1,
                .operand = UtilityOperand::ANY_TYPE,
            },
    },
    {
        .name = "null",
        .type = TypeName::NULL_TYPE,
        .form = PlainTypeName{},
    },
    {
        .name = "number",
        .type = TypeName::NUMBER,
        .form = PlainTypeName{},
    },
    {
        .name = "Ok",
        .type = TypeName::OK,
        .form = ResultTypeName{},
    },
    {
        .name = "Parameters",
        .type = TypeName::PARAMETERS,
        .form =
            UtilityTypeName{
                .op = TypeOp::Parameters,
                .argumentCount = 1,
                .operand = UtilityOperand::FUNCTION_TYPE,
            },
    },
    {
        .name = "Promise",
        .type = TypeName::PROMISE,
        .form = PromiseTypeName{},
    },
    {
        .name = "PromiseLike",
        .type = TypeName::PROMISE_LIKE,
        .form = PromiseTypeName{},
    },
    {
        .name = "ReadonlyArray",
        .type = TypeName::READONLY_ARRAY,
        .form = ArrayTypeName{},
    },
    {
        .name = "ReadonlyMap",
        .type = TypeName::READONLY_MAP,
        .form = CollectionTypeName{.collection = Collection::MAP},
    },
    {
        .name = "ReadonlySet",
        .type = TypeName::READONLY_SET,
        .form = CollectionTypeName{.collection = Collection::SET},
    },
    {
        .name = "Record",
        .type = TypeName::RECORD,
        .form = CollectionTypeName{.collection = Collection::RECORD},
    },
    {
        .name = "RegExp",
        .type = TypeName::REG_EXP,
        .form = PlainTypeName{},
    },
    {
        .name = "RegExpMatchArray",
        .type = TypeName::REG_EXP_MATCH_ARRAY,
        .form = PlainTypeName{},
    },
    {
        .name = "RegExpExecArray",
        .type = TypeName::REG_EXP_MATCH_ARRAY,
        .form = PlainTypeName{},
    },
    {
        .name = "Result",
        .type = TypeName::RESULT,
        .form = ResultTypeName{},
    },
    {
        .name = "ReturnType",
        .type = TypeName::RETURN_TYPE,
        .form =
            UtilityTypeName{
                .op = TypeOp::ReturnType,
                .argumentCount = 1,
                .operand = UtilityOperand::FUNCTION_TYPE,
            },
    },
    {
        .name = "Set",
        .type = TypeName::SET,
        .form = CollectionTypeName{.collection = Collection::SET},
    },
    {
        .name = "SharedArrayBuffer",
        .type = TypeName::SHARED_ARRAY_BUFFER,
        .form = PlainTypeName{},
    },
    {
        .name = "string",
        .type = TypeName::STRING,
        .form = PlainTypeName{},
    },
    {
        .name = "TemplateStringsArray",
        .type = TypeName::TEMPLATE_STRINGS_ARRAY,
        .form = PlainTypeName{},
    },
    {
        .name = "Timeout",
        .type = TypeName::TIMEOUT,
        .form = PlainTypeName{},
    },
    {
        .name = "NodeJS.Timeout",
        .type = TypeName::TIMEOUT,
        .form = PlainTypeName{},
    },
    {
        .name = "Uncapitalize",
        .type = TypeName::UNCAPITALIZE,
        .form =
            UtilityTypeName{
                .op = TypeOp::Uncapitalize,
                .argumentCount = 1,
                .operand = UtilityOperand::TEXT_TYPE,
            },
    },
    {
        .name = "undefined",
        .type = TypeName::UNDEFINED,
        .form = PlainTypeName{},
    },
    {
        .name = "unknown",
        .type = TypeName::UNKNOWN,
        .form = PlainTypeName{},
    },
    {
        .name = "Uppercase",
        .type = TypeName::UPPERCASE,
        .form =
            UtilityTypeName{
                .op = TypeOp::Uppercase,
                .argumentCount = 1,
                .operand = UtilityOperand::TEXT_TYPE,
            },
    },
    {
        .name = "void",
        .type = TypeName::VOID_TYPE,
        .form = PlainTypeName{},
    },
    {
        .name = "WeakMap",
        .type = TypeName::WEAK_MAP,
        .form = WeakCollectionTypeName{},
    },
    {
        .name = "WeakRef",
        .type = TypeName::WEAK_REF,
        .form = PlainTypeName{},
    },
    {
        .name = "WeakSet",
        .type = TypeName::WEAK_SET,
        .form = WeakCollectionTypeName{},
    },
};

}

const TypeNameInfo *TypeNameTable::find(llvm::StringRef name) {
  for (const TypeNameInfo &info : kTypeNames) {
    if (info.name == name) {
      return &info;
    }
  }

  return nullptr;
}

const UtilityTypeName *TypeNameTable::findUtility(llvm::StringRef name) {
  const TypeNameInfo *info = TypeNameTable::find(name);
  if (info == nullptr) {
    return nullptr;
  }

  return info->form.as<UtilityTypeName>();
}

bool TypeNameTable::isName(llvm::StringRef name, TypeName type) {
  const TypeNameInfo *info = TypeNameTable::find(name);

  return info != nullptr && info->type == type;
}

}
