#pragma once

#ifdef UNIT_TEST
#include <string>
using HistoryText = std::string;
#else
#include <Arduino.h>
using HistoryText = String;
#endif

class ConversationHistory {
public:
    ConversationHistory(int maxPairs, int maxBytes);

    void clear();
    void add(const HistoryText &user, const HistoryText &assistant);
    int count() const;
    int bytes() const;
    const HistoryText &at(int index) const;

private:
    static const int kMaxEntries = 12;

    int maxEntries() const;
    int lengthOf(const HistoryText &text) const;
    void dropOldestPair();

    HistoryText entries_[kMaxEntries];
    int count_;
    int maxPairs_;
    int maxBytes_;
};
