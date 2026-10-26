#include "compiler/library/fs/fs_feature/fs_feature.h"
#include "compiler/check/checker/checker.h"
#include "compiler/library/fs/fs_declarer/fs_declarer.h"
#include "compiler/lower/lowering/lowering.h"

namespace tz {

void FsFeature::declare(Checker &checker) const { checker.fsDeclarer().declareFsClasses(); }

}
