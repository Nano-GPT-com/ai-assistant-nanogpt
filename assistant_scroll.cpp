#include "assistant_scroll.h"

bool assistant_scroll_active(int contentHeight, int viewportHeight, bool done) {
    return contentHeight > viewportHeight && !done;
}

int assistant_scroll_max_offset(int contentHeight, int viewportHeight) {
    int maxOffset = contentHeight - viewportHeight;
    return maxOffset > 0 ? maxOffset : 0;
}

void assistant_scroll_advance(AssistantScrollState &state,
                              int contentHeight,
                              int viewportHeight) {
    int maxOffset = assistant_scroll_max_offset(contentHeight, viewportHeight);
    if (state.offset >= maxOffset) {
        state.offset = 0;
        state.done = true;
        return;
    }

    int step = viewportHeight / 2;
    if (step < 1) step = 1;
    state.offset += step;
    if (state.offset > maxOffset) state.offset = maxOffset;
}
