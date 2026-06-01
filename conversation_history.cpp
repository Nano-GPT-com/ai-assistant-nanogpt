#include "conversation_history.h"

ConversationHistory::ConversationHistory(int maxPairs, int maxBytes)
    : count_(0), maxPairs_(maxPairs), maxBytes_(maxBytes) {}

void ConversationHistory::clear() {
    for (int i = 0; i < count_; i++) entries_[i] = HistoryText();
    count_ = 0;
}

void ConversationHistory::add(const HistoryText &user, const HistoryText &assistant) {
    while (count_ + 2 > maxEntries()) dropOldestPair();

    entries_[count_++] = user;
    entries_[count_++] = assistant;

    while (count_ > 2 && bytes() > maxBytes_) dropOldestPair();
}

int ConversationHistory::count() const {
    return count_;
}

int ConversationHistory::bytes() const {
    int total = 0;
    for (int i = 0; i < count_; i++) total += lengthOf(entries_[i]);
    return total;
}

const HistoryText &ConversationHistory::at(int index) const {
    return entries_[index];
}

int ConversationHistory::maxEntries() const {
    int requested = maxPairs_ * 2;
    return requested < kMaxEntries ? requested : kMaxEntries;
}

int ConversationHistory::lengthOf(const HistoryText &text) const {
#ifdef UNIT_TEST
    return (int)text.size();
#else
    return text.length();
#endif
}

void ConversationHistory::dropOldestPair() {
    if (count_ <= 2) {
        clear();
        return;
    }

    for (int i = 0; i < count_ - 2; i++) entries_[i] = entries_[i + 2];
    entries_[count_ - 1] = HistoryText();
    entries_[count_ - 2] = HistoryText();
    count_ -= 2;
}
