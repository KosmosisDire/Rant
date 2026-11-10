/* Reads a message or a map the way an app would, every field, element and entry, touching
 * each byte a view hands back so ASan sees one that points outside the input. */
#ifndef RANT_FUZZ_READ_H
#define RANT_FUZZ_READ_H

#include "rant.h"

void fuzz_read_message(const RantSchema *s, RantBytes msg);
void fuzz_read_map(RantBytes map);
/* reads every byte of a view */
void fuzz_touch(const void *data, size_t len);

#endif /* RANT_FUZZ_READ_H */
