#include "compiler/check/sema/sema.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/ErrorHandling.h"
#include <cmath>
#include <functional>
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/infer/call_cycles/call_cycles.h"
#include "compiler/check/functions/function_checker/function_checker.h"
#include "compiler/check/stmt/return_checker/return_checker.h"
#include "compiler/check/stmt/stmt_checker/stmt_checker.h"
#include "compiler/check/stmt/jump_checker/jump_checker.h"
#include "compiler/check/stmt/loop_checker/loop_checker.h"
#include "compiler/check/stmt/switch_checker/switch_checker.h"
#include "compiler/check/stmt/var_checker/var_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/name_checker/name_checker.h"
#include "compiler/check/expr/literal_checker/literal_checker.h"
#include "compiler/check/expr/unary_checker/unary_checker.h"
#include "compiler/check/expr/binary_checker/binary_checker.h"
#include "compiler/check/expr/concat_checker/concat_checker.h"
#include "compiler/check/expr/assign_checker/assign_checker.h"
#include "compiler/check/expr/target_checker/target_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/library/console/console_checker/console_checker.h"
#include "compiler/check/expr/member_checker/member_checker.h"
#include "compiler/check/generics/arg_list_checker/arg_list_checker.h"
#include "compiler/check/async/async_checker/async_checker.h"
#include "compiler/check/async/promise_checker/promise_checker.h"
#include "compiler/check/async/promise_static_checker/promise_static_checker.h"
#include "compiler/check/async/timer_checker/timer_checker.h"
#include "compiler/library/result/result_async_checker/result_async_checker.h"
#include "compiler/library/math/math_checker/math_checker.h"
#include "compiler/check/values/global_function_checker/global_function_checker.h"
#include "compiler/check/values/number_methods/number_methods.h"
#include "compiler/check/values/string_methods/string_methods.h"
#include "compiler/check/values/array_methods/array_methods.h"
#include "compiler/check/expr/new_checker/new_checker.h"
#include "compiler/check/values/array_literal_checker/array_literal_checker.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/check/classes/class_checker/class_checker.h"
#include "compiler/check/classes/class_member_checker/class_member_checker.h"
#include "compiler/check/classes/static_checker/static_checker.h"
#include "compiler/check/classes/method_call_checker/method_call_checker.h"
#include "compiler/check/classes/instance_of_checker/instance_of_checker.h"
#include "compiler/check/classes/overload_resolver/overload_resolver.h"
#include "compiler/check/classes/method_literal_checker/method_literal_checker.h"
#include "compiler/check/classes/class_value_checker/class_value_checker.h"
#include "compiler/instances/late_classes/late_classes.h"
#include "compiler/check/functions/closure_checker/closure_checker.h"
#include "compiler/check/functions/nested_functions/nested_functions.h"
#include "compiler/check/functions/function_value_checker/function_value_checker.h"
#include "compiler/check/functions/callback_checker/callback_checker.h"
#include "compiler/check/types/conditional_types/conditional_types.h"
#include "compiler/check/types/extends_relation/extends_relation.h"
#include "compiler/check/types/template_types/template_types.h"
#include "compiler/declare/type_declarer/type_declarer.h"
#include "compiler/declare/constant_folder/constant_folder.h"
#include "compiler/check/functions/function_method_checker/function_method_checker.h"
#include "compiler/check/classes/decorator_checker/decorator_checker.h"
#include "compiler/library/reflect/metadata_checker/metadata_checker.h"
#include "compiler/check/types/destination/destination.h"
#include "compiler/check/types/destination_scanner/destination_scanner.h"
#include "compiler/check/objects/dict_checker/dict_checker.h"
#include "compiler/check/objects/dict_methods/dict_methods.h"
#include "compiler/check/objects/dict_new_checker/dict_new_checker.h"
#include "compiler/check/objects/record_literal_checker/record_literal_checker.h"
#include "compiler/check/errors/throw_checker/throw_checker.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/flow/index_proofs/index_proofs.h"
#include "compiler/check/flow/condition_facts/condition_facts.h"
#include "compiler/check/flow/loop_scanner/loop_scanner.h"
#include "compiler/check/flow/loop_bounds/loop_bounds.h"
#include "compiler/check/generics/type_params/type_params.h"
#include "compiler/check/generics/generic_checker/generic_checker.h"
#include "compiler/check/generics/type_arg_inferrer/type_arg_inferrer.h"
#include "compiler/check/generics/generic_instantiator/generic_instantiator.h"
#include "compiler/check/generics/alias_instantiator/alias_instantiator.h"
#include "compiler/check/generics/generic_class_checker/generic_class_checker.h"
#include "compiler/check/generics/generic_class_instantiator/generic_class_instantiator.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/check/classes/class_bases/class_bases.h"
#include "compiler/check/classes/class_links/class_links.h"
#include "compiler/check/classes/inherit_checker/inherit_checker.h"
#include "compiler/instances/constructor_calls/constructor_calls.h"
#include "compiler/check/iterators/generator_checker/generator_checker.h"
#include "compiler/check/iterators/iterator_methods/iterator_methods.h"
#include "compiler/check/iterators/iterable_checker/iterable_checker.h"
#include "compiler/declare/module_declarer/module_declarer.h"
#include "compiler/declare/import_binder/import_binder.h"
#include "compiler/check/top_level_checker/top_level_checker.h"
#include "compiler/whole_program/load_order/load_order.h"
#include "compiler/whole_program/unused/unused.h"
#include "compiler/check/objects/object_literal_checker/object_literal_checker.h"
#include "compiler/check/objects/keyed_field_checker/keyed_field_checker.h"
#include "compiler/check/objects/tuple_checker/tuple_checker.h"
#include "compiler/check/objects/object_checker/object_checker.h"
#include "compiler/check/flow/field_facts/field_facts.h"
#include "compiler/check/flow/predicate_checker/predicate_checker.h"
#include "compiler/check/expr/truth_checker/truth_checker.h"
#include "compiler/check/types/type_operators/type_operators.h"
#include "compiler/check/types/utility_types/utility_types.h"
#include "compiler/declare/prelude/prelude.h"
#include "compiler/check/types/literal_types/literal_types.h"
#include "compiler/check/flow/union_narrowing/union_narrowing.h"
#include "compiler/check/types/union_checker/union_checker.h"
#include "compiler/check/types/unknown_checker/unknown_checker.h"
#include "compiler/whole_program/wide_writes/wide_writes.h"
#include "compiler/library/bytes/byte_proofs/byte_proofs.h"
#include "compiler/library/bytes/bytes_checker/bytes_checker.h"
#include "compiler/library/bytes/bytes_new_checker/bytes_new_checker.h"
#include "compiler/library/bytes/typed_array_checker/typed_array_checker.h"
#include "compiler/library/bytes/buffer_checker/buffer_checker.h"
#include "compiler/library/bytes/buffer_static_checker/buffer_static_checker.h"
#include "compiler/library/child_process/child_checker/child_checker.h"
#include "compiler/library/crypto/crypto_checker/crypto_checker.h"
#include "compiler/library/crypto/crypto_args/crypto_args.h"
#include "compiler/library/crypto/crypto_function_checker/crypto_function_checker.h"
#include "compiler/library/crypto/key_pair_checker/key_pair_checker.h"
#include "compiler/library/crypto/crypto_method_checker/crypto_method_checker.h"
#include "compiler/library/crypto/web_crypto_checker/web_crypto_checker.h"
#include "compiler/library/crypto/subtle_call_checker/subtle_call_checker.h"
#include "compiler/library/date/date_checker/date_checker.h"
#include "compiler/library/date/date_format_checker/date_format_checker.h"
#include "compiler/library/decimal/decimal_checker/decimal_checker.h"
#include "compiler/library/tz_modules/library_calls/library_calls.h"
#include "compiler/library/events/event_maps/event_maps.h"
#include "compiler/library/events/event_map_inference/event_map_inference.h"
#include "compiler/library/events/emitter_checker/emitter_checker.h"
#include "compiler/library/events/event_target_checker/event_target_checker.h"
#include "compiler/library/intl/intl_checker/intl_checker.h"
#include "compiler/library/intl/intl_options_checker/intl_options_checker.h"
#include "compiler/library/intl/intl_method_checker/intl_method_checker.h"
#include "compiler/library/json/json_checker/json_checker.h"
#include "compiler/library/json/json_type_checker/json_type_checker.h"
#include "compiler/library/json/json_static_checker/json_static_checker.h"
#include "compiler/library/builtin_modules/builtin_module_checker/builtin_module_checker.h"
#include "compiler/library/fs/fs_checker/fs_checker.h"
#include "compiler/library/fs/fs_declarer/fs_declarer.h"
#include "compiler/library/path/path_checker/path_checker.h"
#include "compiler/library/os/os_checker/os_checker.h"
#include "compiler/library/builtin_modules/runtime_object_checker/runtime_object_checker.h"
#include "compiler/library/fs/fs_promises_checker/fs_promises_checker.h"
#include "compiler/library/fs/readline_checker/readline_checker.h"
#include "compiler/library/process/process_checker/process_checker.h"
#include "compiler/library/reflect/reflect_checker/reflect_checker.h"
#include "compiler/library/reflect/reflect_call_checker/reflect_call_checker.h"
#include "compiler/library/regex/regex_checker/regex_checker.h"
#include "compiler/library/regex/string_regex_checker/string_regex_checker.h"
#include "compiler/library/regex/match_checker/match_checker.h"
#include "compiler/library/result/result_types/result_types.h"
#include "compiler/library/result/result_checker/result_checker.h"
#include "compiler/library/result/safe_try_checker/safe_try_checker.h"
#include "compiler/library/result/result_method_checker/result_method_checker.h"
#include "compiler/library/stream/chunk_types/chunk_types.h"
#include "compiler/library/stream/stream_new_checker/stream_new_checker.h"
#include "compiler/library/stream/stream_checker/stream_checker.h"
#include "compiler/library/stream/set_encoding_checker/set_encoding_checker.h"
#include "compiler/library/stream/stream_sources/stream_sources.h"
#include "compiler/library/stream/stream_pipeline/stream_pipeline.h"
#include "compiler/library/tz_modules/tz_modules_checker/tz_modules_checker.h"
#include "compiler/library/weakref/weak_ref_checker/weak_ref_checker.h"
#include "compiler/declare/function_names/function_names.h"
#include "compiler/check/shared_helpers/shared_helpers.h"

namespace tz {

using llvm::cast;

using llvm::dyn_cast;

using llvm::isa;

std::vector<const Stmt *> followingStatements(llvm::ArrayRef<Stmt *> stmts, size_t i) {
  std::vector<const Stmt *> following;
  for (size_t j = 0; j < i; ++j) {
    if (isa<FunctionDecl>(stmts[j])) {
      following.push_back(stmts[j]);
    }
  }
  following.insert(following.end(), stmts.begin() + i + 1, stmts.end());

  return following;
}


// The type of a parameter without an annotation, from a default value that
// is a literal, as `let x = 5` would give it; null for other values.
const Type *literalType(const Expr *value) {
  if (auto *unary = dyn_cast<UnaryExpr>(value); unary && unary->op == UnaryOp::Neg) {
    value = unary->operand;
  }
  if (auto *lit = dyn_cast<IntLitExpr>(value)) {
    return lit->value < (uint64_t(1) << 31)   ? Type::intTy()
           : lit->value < (uint64_t(1) << 63) ? Type::longTy()
                                              : Type::doubleTy();
  }
  if (isa<FloatLitExpr>(value)) {
    return Type::doubleTy();
  }
  if (isa<StringLitExpr>(value) || isa<TemplateExpr>(value)) {
    return Type::string();
  }
  if (isa<BoolLitExpr>(value)) {
    return Type::boolTy();
  }

  return nullptr;
}

Sema::Sema(AstContext &ctx, Diagnostics &diags) : ctx_(ctx), diags_(diags) {
  scopesPart_ = std::make_unique<Scopes>(*this);
  typeResolverPart_ = std::make_unique<TypeResolver>(*this);
  typeRelationsPart_ = std::make_unique<TypeRelations>(*this);
  callCyclesPart_ = std::make_unique<CallCycles>(*this);
  functionCheckerPart_ = std::make_unique<FunctionChecker>(*this);
  returnCheckerPart_ = std::make_unique<ReturnChecker>(*this);
  stmtCheckerPart_ = std::make_unique<StmtChecker>(*this);
  jumpCheckerPart_ = std::make_unique<JumpChecker>(*this);
  loopCheckerPart_ = std::make_unique<LoopChecker>(*this);
  switchCheckerPart_ = std::make_unique<SwitchChecker>(*this);
  varCheckerPart_ = std::make_unique<VarChecker>(*this);
  exprCheckerPart_ = std::make_unique<ExprChecker>(*this);
  nameCheckerPart_ = std::make_unique<NameChecker>(*this);
  literalCheckerPart_ = std::make_unique<LiteralChecker>(*this);
  unaryCheckerPart_ = std::make_unique<UnaryChecker>(*this);
  binaryCheckerPart_ = std::make_unique<BinaryChecker>(*this);
  concatCheckerPart_ = std::make_unique<ConcatChecker>(*this);
  assignCheckerPart_ = std::make_unique<AssignChecker>(*this);
  targetCheckerPart_ = std::make_unique<TargetChecker>(*this);
  callCheckerPart_ = std::make_unique<CallChecker>(*this);
  argumentCheckerPart_ = std::make_unique<ArgumentChecker>(*this);
  consoleCheckerPart_ = std::make_unique<ConsoleChecker>(*this);
  memberCheckerPart_ = std::make_unique<MemberChecker>(*this);
  argListCheckerPart_ = std::make_unique<ArgListChecker>(*this);
  asyncCheckerPart_ = std::make_unique<AsyncChecker>(*this);
  promiseCheckerPart_ = std::make_unique<PromiseChecker>(*this);
  promiseStaticCheckerPart_ = std::make_unique<PromiseStaticChecker>(*this);
  timerCheckerPart_ = std::make_unique<TimerChecker>(*this);
  resultAsyncCheckerPart_ = std::make_unique<ResultAsyncChecker>(*this);
  mathCheckerPart_ = std::make_unique<MathChecker>(*this);
  globalFunctionCheckerPart_ = std::make_unique<GlobalFunctionChecker>(*this);
  numberMethodsPart_ = std::make_unique<NumberMethods>(*this);
  stringMethodsPart_ = std::make_unique<StringMethods>(*this);
  arrayMethodsPart_ = std::make_unique<ArrayMethods>(*this);
  newCheckerPart_ = std::make_unique<NewChecker>(*this);
  arrayLiteralCheckerPart_ = std::make_unique<ArrayLiteralChecker>(*this);
  classDeclarerPart_ = std::make_unique<ClassDeclarer>(*this);
  classCheckerPart_ = std::make_unique<ClassChecker>(*this);
  classMemberCheckerPart_ = std::make_unique<ClassMemberChecker>(*this);
  staticCheckerPart_ = std::make_unique<StaticChecker>(*this);
  methodCallCheckerPart_ = std::make_unique<MethodCallChecker>(*this);
  instanceOfCheckerPart_ = std::make_unique<InstanceOfChecker>(*this);
  overloadResolverPart_ = std::make_unique<OverloadResolver>(*this);
  methodLiteralCheckerPart_ = std::make_unique<MethodLiteralChecker>(*this);
  classValueCheckerPart_ = std::make_unique<ClassValueChecker>(*this);
  lateClassesPart_ = std::make_unique<LateClasses>(*this);
  closureCheckerPart_ = std::make_unique<ClosureChecker>(*this);
  nestedFunctionsPart_ = std::make_unique<NestedFunctions>(*this);
  functionValueCheckerPart_ = std::make_unique<FunctionValueChecker>(*this);
  callbackCheckerPart_ = std::make_unique<CallbackChecker>(*this);
  conditionalTypesPart_ = std::make_unique<ConditionalTypes>(*this);
  extendsRelationPart_ = std::make_unique<ExtendsRelation>(*this);
  templateTypesPart_ = std::make_unique<TemplateTypes>(*this);
  typeDeclarerPart_ = std::make_unique<TypeDeclarer>(*this);
  constantFolderPart_ = std::make_unique<ConstantFolder>(*this);
  functionMethodCheckerPart_ = std::make_unique<FunctionMethodChecker>(*this);
  decoratorCheckerPart_ = std::make_unique<DecoratorChecker>(*this);
  metadataCheckerPart_ = std::make_unique<MetadataChecker>(*this);
  destinationPart_ = std::make_unique<Destination>(*this);
  destinationScannerPart_ = std::make_unique<DestinationScanner>(*this);
  dictCheckerPart_ = std::make_unique<DictChecker>(*this);
  dictMethodsPart_ = std::make_unique<DictMethods>(*this);
  dictNewCheckerPart_ = std::make_unique<DictNewChecker>(*this);
  recordLiteralCheckerPart_ = std::make_unique<RecordLiteralChecker>(*this);
  throwCheckerPart_ = std::make_unique<ThrowChecker>(*this);
  flowCheckerPart_ = std::make_unique<FlowChecker>(*this);
  indexProofsPart_ = std::make_unique<IndexProofs>(*this);
  conditionFactsPart_ = std::make_unique<ConditionFacts>(*this);
  loopScannerPart_ = std::make_unique<LoopScanner>(*this);
  loopBoundsPart_ = std::make_unique<LoopBounds>(*this);
  typeParamsPart_ = std::make_unique<TypeParams>(*this);
  genericCheckerPart_ = std::make_unique<GenericChecker>(*this);
  typeArgInferrerPart_ = std::make_unique<TypeArgInferrer>(*this);
  genericInstantiatorPart_ = std::make_unique<GenericInstantiator>(*this);
  aliasInstantiatorPart_ = std::make_unique<AliasInstantiator>(*this);
  genericClassCheckerPart_ = std::make_unique<GenericClassChecker>(*this);
  genericClassInstantiatorPart_ = std::make_unique<GenericClassInstantiator>(*this);
  returnInferrerPart_ = std::make_unique<ReturnInferrer>(*this);
  classBasesPart_ = std::make_unique<ClassBases>(*this);
  classLinksPart_ = std::make_unique<ClassLinks>(*this);
  inheritCheckerPart_ = std::make_unique<InheritChecker>(*this);
  constructorCallsPart_ = std::make_unique<ConstructorCalls>(*this);
  generatorCheckerPart_ = std::make_unique<GeneratorChecker>(*this);
  iteratorMethodsPart_ = std::make_unique<IteratorMethods>(*this);
  iterableCheckerPart_ = std::make_unique<IterableChecker>(*this);
  moduleDeclarerPart_ = std::make_unique<ModuleDeclarer>(*this);
  importBinderPart_ = std::make_unique<ImportBinder>(*this);
  topLevelCheckerPart_ = std::make_unique<TopLevelChecker>(*this);
  loadOrderPart_ = std::make_unique<LoadOrder>(*this);
  unusedPart_ = std::make_unique<Unused>(*this);
  objectLiteralCheckerPart_ = std::make_unique<ObjectLiteralChecker>(*this);
  keyedFieldCheckerPart_ = std::make_unique<KeyedFieldChecker>(*this);
  tupleCheckerPart_ = std::make_unique<TupleChecker>(*this);
  objectCheckerPart_ = std::make_unique<ObjectChecker>(*this);
  fieldFactsPart_ = std::make_unique<FieldFacts>(*this);
  predicateCheckerPart_ = std::make_unique<PredicateChecker>(*this);
  truthCheckerPart_ = std::make_unique<TruthChecker>(*this);
  typeOperatorsPart_ = std::make_unique<TypeOperators>(*this);
  utilityTypesPart_ = std::make_unique<UtilityTypes>(*this);
  preludePart_ = std::make_unique<Prelude>(*this);
  literalTypesPart_ = std::make_unique<LiteralTypes>(*this);
  unionNarrowingPart_ = std::make_unique<UnionNarrowing>(*this);
  unionCheckerPart_ = std::make_unique<UnionChecker>(*this);
  unknownCheckerPart_ = std::make_unique<UnknownChecker>(*this);
  wideWritesPart_ = std::make_unique<WideWrites>(*this);
  byteProofsPart_ = std::make_unique<ByteProofs>(*this);
  bytesCheckerPart_ = std::make_unique<BytesChecker>(*this);
  bytesNewCheckerPart_ = std::make_unique<BytesNewChecker>(*this);
  typedArrayCheckerPart_ = std::make_unique<TypedArrayChecker>(*this);
  bufferCheckerPart_ = std::make_unique<BufferChecker>(*this);
  bufferStaticCheckerPart_ = std::make_unique<BufferStaticChecker>(*this);
  childCheckerPart_ = std::make_unique<ChildChecker>(*this);
  cryptoCheckerPart_ = std::make_unique<CryptoChecker>(*this);
  cryptoArgsPart_ = std::make_unique<CryptoArgs>(*this);
  cryptoFunctionCheckerPart_ = std::make_unique<CryptoFunctionChecker>(*this);
  keyPairCheckerPart_ = std::make_unique<KeyPairChecker>(*this);
  cryptoMethodCheckerPart_ = std::make_unique<CryptoMethodChecker>(*this);
  webCryptoCheckerPart_ = std::make_unique<WebCryptoChecker>(*this);
  subtleCallCheckerPart_ = std::make_unique<SubtleCallChecker>(*this);
  dateCheckerPart_ = std::make_unique<DateChecker>(*this);
  dateFormatCheckerPart_ = std::make_unique<DateFormatChecker>(*this);
  decimalCheckerPart_ = std::make_unique<DecimalChecker>(*this);
  libraryCallsPart_ = std::make_unique<LibraryCalls>(*this);
  eventMapsPart_ = std::make_unique<EventMaps>(*this);
  eventMapInferencePart_ = std::make_unique<EventMapInference>(*this);
  emitterCheckerPart_ = std::make_unique<EmitterChecker>(*this);
  eventTargetCheckerPart_ = std::make_unique<EventTargetChecker>(*this);
  intlCheckerPart_ = std::make_unique<IntlChecker>(*this);
  intlOptionsCheckerPart_ = std::make_unique<IntlOptionsChecker>(*this);
  intlMethodCheckerPart_ = std::make_unique<IntlMethodChecker>(*this);
  jsonCheckerPart_ = std::make_unique<JsonChecker>(*this);
  jsonTypeCheckerPart_ = std::make_unique<JsonTypeChecker>(*this);
  jsonStaticCheckerPart_ = std::make_unique<JsonStaticChecker>(*this);
  builtinModuleCheckerPart_ = std::make_unique<BuiltinModuleChecker>(*this);
  fsCheckerPart_ = std::make_unique<FsChecker>(*this);
  fsDeclarerPart_ = std::make_unique<FsDeclarer>(*this);
  pathCheckerPart_ = std::make_unique<PathChecker>(*this);
  osCheckerPart_ = std::make_unique<OsChecker>(*this);
  runtimeObjectCheckerPart_ = std::make_unique<RuntimeObjectChecker>(*this);
  fsPromisesCheckerPart_ = std::make_unique<FsPromisesChecker>(*this);
  readlineCheckerPart_ = std::make_unique<ReadlineChecker>(*this);
  processCheckerPart_ = std::make_unique<ProcessChecker>(*this);
  reflectCheckerPart_ = std::make_unique<ReflectChecker>(*this);
  reflectCallCheckerPart_ = std::make_unique<ReflectCallChecker>(*this);
  regexCheckerPart_ = std::make_unique<RegexChecker>(*this);
  stringRegexCheckerPart_ = std::make_unique<StringRegexChecker>(*this);
  matchCheckerPart_ = std::make_unique<MatchChecker>(*this);
  resultTypesPart_ = std::make_unique<ResultTypes>(*this);
  resultCheckerPart_ = std::make_unique<ResultChecker>(*this);
  safeTryCheckerPart_ = std::make_unique<SafeTryChecker>(*this);
  resultMethodCheckerPart_ = std::make_unique<ResultMethodChecker>(*this);
  chunkTypesPart_ = std::make_unique<ChunkTypes>(*this);
  streamNewCheckerPart_ = std::make_unique<StreamNewChecker>(*this);
  streamCheckerPart_ = std::make_unique<StreamChecker>(*this);
  setEncodingCheckerPart_ = std::make_unique<SetEncodingChecker>(*this);
  streamSourcesPart_ = std::make_unique<StreamSources>(*this);
  streamPipelinePart_ = std::make_unique<StreamPipeline>(*this);
  tzModulesCheckerPart_ = std::make_unique<TzModulesChecker>(*this);
  weakRefCheckerPart_ = std::make_unique<WeakRefChecker>(*this);}

const Type *Sema::awaitedTypeOf(const Type *type) { return AsyncChecker::awaitedType(type); }

void Sema::popScope() {
  if (!untyped_.empty()) {
    varChecker().reportUntyped();
  }
  scopes_.pop_back();
  typeScopes_.pop_back();
  if (!scopeBranches_.empty()) {
    scopeBranches_.pop_back();
  }
}

Sema::~Sema() = default;
} // namespace tz
