#pragma once

struct AssistantScrollState {
    int offset;
    bool done;
};

bool assistant_scroll_active(int contentHeight, int viewportHeight, bool done);
int assistant_scroll_max_offset(int contentHeight, int viewportHeight);
void assistant_scroll_advance(AssistantScrollState &state,
                              int contentHeight,
                              int viewportHeight);
