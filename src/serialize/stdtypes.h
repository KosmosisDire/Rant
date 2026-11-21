/* The standard type library: named types always in scope in the DSL. The C mirrors are in
 * common/stdtypes.h, and docs/stdtypes.md has the roster and conventions. */
#ifndef RANT_STD_TABLE_H
#define RANT_STD_TABLE_H

#include "../common/api.h"
#include "../common/stdtypes.h"
#include "schema.h"
#include "stdroster.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The type's name and its definition, whose comments describe it and give each field's
 * [unit]. NULL for RANT_STD_NONE or an out of range value. */
RANT_API const char *rant_std_name(RantStdType t);
RANT_API const char *rant_std_text(RantStdType t);
/* A name lookup only. rant_std_recognize (node/runtime.h) verifies a peer's shape too. */
RANT_API RantStdType rant_std_by_name(RantString name);

#ifdef __cplusplus
}
#endif
#endif /* RANT_STD_TABLE_H */
