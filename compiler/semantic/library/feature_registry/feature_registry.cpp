#include "compiler/semantic/library/feature_registry/feature_registry.h"
#include "compiler/semantic/library/builtin_modules/builtin_modules_feature/builtin_modules_feature.h"
#include "compiler/semantic/library/bytes/bytes_feature/bytes_feature.h"
#include "compiler/semantic/library/child_process/child_process_feature/child_process_feature.h"
#include "compiler/semantic/library/console/console_feature/console_feature.h"
#include "compiler/semantic/library/crypto/crypto_feature/crypto_feature.h"
#include "compiler/semantic/library/date/date_feature/date_feature.h"
#include "compiler/semantic/library/decimal/decimal_feature/decimal_feature.h"
#include "compiler/semantic/library/events/events_feature/events_feature.h"
#include "compiler/semantic/library/fs/fs_feature/fs_feature.h"
#include "compiler/semantic/library/intl/intl_feature/intl_feature.h"
#include "compiler/semantic/library/json/json_feature/json_feature.h"
#include "compiler/semantic/library/math/math_feature/math_feature.h"
#include "compiler/semantic/library/os/os_feature/os_feature.h"
#include "compiler/semantic/library/path/path_feature/path_feature.h"
#include "compiler/semantic/library/process/process_feature/process_feature.h"
#include "compiler/semantic/library/reflect/reflect_feature/reflect_feature.h"
#include "compiler/semantic/library/regex/regex_feature/regex_feature.h"
#include "compiler/semantic/library/result/result_feature/result_feature.h"
#include "compiler/semantic/library/stream/stream_feature/stream_feature.h"
#include "compiler/semantic/library/tz_modules/tz_modules_feature/tz_modules_feature.h"
#include "compiler/semantic/library/weakref/weakref_feature/weakref_feature.h"

namespace tz {

FeatureRegistry::FeatureRegistry() {
  this->features.push_back(std::make_unique<TzModulesFeature>());
  this->features.push_back(std::make_unique<BuiltinModulesFeature>());
  this->features.push_back(std::make_unique<ResultFeature>());
  this->features.push_back(std::make_unique<JsonFeature>());
  this->features.push_back(std::make_unique<ReflectFeature>());
  this->features.push_back(std::make_unique<ConsoleFeature>());
  this->features.push_back(std::make_unique<ProcessFeature>());
  this->features.push_back(std::make_unique<MathFeature>());
  this->features.push_back(std::make_unique<RegexFeature>());
  this->features.push_back(std::make_unique<DateFeature>());
  this->features.push_back(std::make_unique<IntlFeature>());
  this->features.push_back(std::make_unique<DecimalFeature>());
  this->features.push_back(std::make_unique<CryptoFeature>());
  this->features.push_back(std::make_unique<WeakRefFeature>());
  this->features.push_back(std::make_unique<BytesFeature>());
  this->features.push_back(std::make_unique<PathFeature>());
  this->features.push_back(std::make_unique<OsFeature>());
  this->features.push_back(std::make_unique<FsFeature>());
  this->features.push_back(std::make_unique<ChildProcessFeature>());
  this->features.push_back(std::make_unique<EventsFeature>());
  this->features.push_back(std::make_unique<StreamFeature>());
  for (const std::unique_ptr<Feature> &feature : this->features) {
    this->order.push_back(feature.get());
  }
}

}
