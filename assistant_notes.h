#pragma once

#include <stddef.h>

size_t assistant_notes_recent_slice_start(const char *notes,
                                          size_t length,
                                          int count);

size_t assistant_notes_trim_end(const char *notes, size_t length);
