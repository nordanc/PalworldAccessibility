#pragma once

#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <chrono>

namespace PalAccess {

// Speech queueing / deduplication, modeled on the Siralim accessibility mod.
// - Speak():    routine output. Silently dropped if it duplicates the previous
//               utterance within DedupeMillis, or matches any of the last
//               RecentBuffer utterances.
// - Announce(): high-priority output. Interrupts current speech, bypasses
//               dedupe. Use for menu changes, dialog appearing, focus changes.
class Speech {
public:
    static Speech& Get();

    void Speak(std::wstring_view text);
    void Speak(std::string_view utf8);

    void Announce(std::wstring_view text);
    void Announce(std::string_view utf8);

    // Use for menu / focus changes. Interrupts current speech, dedupes only
    // against the immediately previous utterance within FocusDedupeMillis.
    // Crucially does NOT consult the recent-utterance buffer, so navigating
    // up/down through the same options re-speaks each one.
    void FocusUpdate(std::wstring_view text);
    void FocusUpdate(std::string_view utf8);

    // Queue a phrase to play after the current speech finishes. No dedupe,
    // no interrupt — used for sequential lines that all need to be heard
    // (e.g. a list of material costs).
    void Queue(std::wstring_view text);

    void Silence();

    // Tuneables.
    int DedupeMillis      = 200;
    int RecentBuffer      = 30;
    int FocusDedupeMillis = 150;

private:
    Speech() = default;

    static std::wstring WidenUtf8(std::string_view utf8);

    std::mutex             m_mutex;
    std::wstring           m_lastSpoken;
    std::chrono::steady_clock::time_point m_lastSpokenAt{};
    std::deque<std::wstring> m_recent;
};

} // namespace PalAccess
