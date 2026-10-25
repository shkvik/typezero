#include "compiler/library/fs/fs_feature/fs_feature.h"
#include "compiler/check/sema/sema.h"
#include "compiler/lower/codegen/codegen.h"
#include "compiler/library/fs/fs_declarer/fs_declarer.h"

namespace tz {

void FsFeature::declare(Sema &sema) const {
  sema.fsDeclarer().declareFsClasses();
}

} // namespace tz
