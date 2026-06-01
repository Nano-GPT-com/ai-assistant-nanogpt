#include "assistant_notes.h"

size_t assistant_notes_trim_end(const char *notes, size_t length) {
    if (!notes) return 0;
    while (length > 0 && (notes[length - 1] == '\n' || notes[length - 1] == '\r')) {
        length--;
    }
    return length;
}

size_t assistant_notes_recent_slice_start(const char *notes,
                                          size_t length,
                                          int count) {
    if (!notes || length == 0 || count <= 0) return 0;

    int found = 0;
    for (size_t i = length; i > 0; i--) {
        if (notes[i - 1] == '\n') {
            found++;
            if (found >= count) return i;
        }
    }
    return 0;
}
