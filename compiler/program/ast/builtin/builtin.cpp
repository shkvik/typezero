#include "compiler/program/ast/builtin/builtin.h"

namespace tz {

bool BuiltinGroups::isLibBuiltin(Builtin builtin) {
  return builtin >= Builtin::LibListener && builtin <= Builtin::LibWithContext;
}

bool BuiltinGroups::isCryptoBuiltin(Builtin builtin) {
  return builtin == Builtin::Crypto || builtin == Builtin::CryptoKeyPair ||
         builtin == Builtin::CryptoKeyPairAsync;
}

bool BuiltinGroups::isMetaBuiltin(Builtin builtin) {
  return builtin >= Builtin::MetaDefine && builtin <= Builtin::MetaDelete;
}

bool BuiltinGroups::isDictBuiltin(Builtin builtin) {
  return builtin >= Builtin::MapGet && builtin <= Builtin::EnvMap;
}

bool BuiltinGroups::isDictIterator(Builtin builtin) {
  return builtin >= Builtin::MapKeys && builtin <= Builtin::SetEntries;
}

bool BuiltinGroups::isPathBuiltin(Builtin builtin) {
  return builtin >= Builtin::PathJoin && builtin <= Builtin::PathToNamespacedPath;
}

bool BuiltinGroups::isOsBuiltin(Builtin builtin) {
  return builtin >= Builtin::OsString && builtin <= Builtin::OsUserInfo;
}

bool BuiltinGroups::isFsModuleBuiltin(Builtin builtin) {
  return builtin >= Builtin::ReadLine && builtin <= Builtin::RlClose;
}

bool BuiltinGroups::isFspBuiltin(Builtin builtin) {
  return builtin >= Builtin::FspReadFile && builtin <= Builtin::FhSync;
}

bool BuiltinGroups::isChildBuiltin(Builtin builtin) {
  return builtin >= Builtin::ChildSpawnSync && builtin <= Builtin::ChildExecSync;
}

bool BuiltinGroups::isFsBuiltin(Builtin builtin) {
  return builtin >= Builtin::FsReadFile && builtin <= Builtin::FsRealpath &&
         builtin != Builtin::FsExists;
}

bool BuiltinGroups::isPromiseBuiltin(Builtin builtin) {
  return builtin >= Builtin::PromiseNew && builtin <= Builtin::PromiseWithResolvers;
}

bool BuiltinGroups::isAsyncBuiltin(Builtin builtin) {
  return builtin >= Builtin::PromiseNew && builtin <= Builtin::TimersSetInterval;
}

bool BuiltinGroups::isJsonBuiltin(Builtin builtin) {
  return builtin >= Builtin::JsonParse && builtin <= Builtin::JsonIsJson;
}

bool BuiltinGroups::isDateBuiltin(Builtin builtin) {
  return builtin >= Builtin::DateNew && builtin <= Builtin::DtfResolvedOptions;
}

bool BuiltinGroups::isBytesBuiltin(Builtin builtin) {
  return builtin >= Builtin::BytesNew && builtin <= Builtin::EmbedBytes;
}

bool BuiltinGroups::isIntlBuiltin(Builtin builtin) {
  return builtin >= Builtin::IntlNew && builtin <= Builtin::SegmentsContaining;
}

bool BuiltinGroups::isMathBuiltin(Builtin builtin) {
  return builtin >= Builtin::MathFloor && builtin <= Builtin::MathRandom;
}

bool BuiltinGroups::isDecimalBuiltin(Builtin builtin) {
  return builtin >= Builtin::DecimalOf && builtin <= Builtin::DecToFraction;
}

bool BuiltinGroups::isResultBuiltin(Builtin builtin) {
  return builtin >= Builtin::ResultOk && builtin <= Builtin::ResultAndThrough;
}

bool BuiltinGroups::isResultCallback(Builtin builtin) {
  return builtin >= Builtin::ResultMap && builtin <= Builtin::ResultAndThrough &&
         builtin != Builtin::ResultUnwrapOr;
}

bool BuiltinGroups::isRegexBuiltin(Builtin builtin) {
  return builtin >= Builtin::RegExpNew && builtin <= Builtin::StrSplitRegex;
}

bool BuiltinGroups::isProcessBuiltin(Builtin builtin) {
  return builtin >= Builtin::ProcessArgv && builtin <= Builtin::PerformanceNow;
}

bool BuiltinGroups::isRoundingBuiltin(Builtin builtin) {
  return builtin >= Builtin::MathFloor && builtin <= Builtin::MathTrunc;
}

bool BuiltinGroups::isIteratorStage(Builtin builtin) {
  return builtin >= Builtin::IterMap && builtin <= Builtin::IterFlatMap;
}

bool BuiltinGroups::isIteratorSink(Builtin builtin) {
  return builtin >= Builtin::IterToArray && builtin <= Builtin::IterFind;
}

}
