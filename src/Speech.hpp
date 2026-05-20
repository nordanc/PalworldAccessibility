#pragma once

#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <chrono>

namespace PalAccess {

// Speech with two policies: cued vs. interrupting.
//
//   Speak / Announce / Queue — CUED.
//     Calls Tolk_Output with interrupt=false. The currently-speaking
//     utterance always plays to completion.
//     - Speak coalesces: a new utterance arriving while Tolk is busy
//       replaces any earlier pending utterance (single-slot). Tick()
//       flushes pending the moment Tolk goes idle. Use for routine
//       text that should play in full but where the *latest* is what
//       matters if several arrive close together.
//     - Announce / Queue do not coalesce — they hand straight to
//       Tolk's native FIFO so multiple lines play in order. Use for
//       notifications, dialogs, and material-cost lists.
//
//   FocusUpdate — INTERRUPTING.
//     Calls Tolk_Output with interrupt=true and clears any pending
//     cued utterance. Use for menu / focus navigation where the user
//     scrolling through items expects the new label to start
//     immediately, not wait for the previous label to finish.
//
// Tick() must be called every frame (on_update). It's a no-op when
// nothing is pending.
//
// Silence() clears the local pending slot and Tolk's queue — for
// explicit user cancels (target-lock cancel, etc.).
class Speech {
public:
    static Speech& Get();

    void Speak(std::wstring_view text);
    void Speak(std::string_view utf8);

    void Announce(std::wstring_view text);
    void Announce(std::string_view utf8);

    void FocusUpdate(std::wstring_view text);
    void FocusUpdate(std::string_view utf8);

    void Queue(std::wstring_view text);

    // Interrupt-and-speak with no dedup, no cascade gate, no coalescing.
    // Use when speech is a direct response to a user action (target-lock
    // cycling, stat hotkeys) and must be heard immediately regardless of
    // what menu-arrival logic thinks is happening.
    void SpeakNow(std::wstring_view text);
    void SpeakNow(std::string_view utf8);

    // Mark "a new screen just appeared." The next FocusUpdate call within
    // ScreenArrivalGraceMillis is treated as the panel's automatic
    // initial-focus event (not user navigation) and falls back to the
    // cued non-interrupting path so the screen title isn't cut off.
    void NotifyScreenArrival();

    void Tick();
    void Silence();

    // Tuneables.
    int DedupeMillis              = 200;
    int RecentBuffer              = 30;
    int FocusDedupeMillis         = 150;
    int ScreenArrivalGraceMillis  = 500;
    // After the initial arrival grace, extend it for this long after each
    // focus event so a row-construction cascade can finish before we flush.
    int FocusCascadeDebounceMillis = 200;

private:
    Speech() = default;

    static std::wstring WidenUtf8(std::string_view utf8);

    // Either sends `text` to Tolk now (if idle) or stores it in the
    // single-slot pending buffer (overwriting any previous pending).
    // Caller holds m_mutex.
    void Cue(const std::wstring& text);

    std::mutex             m_mutex;
    std::wstring           m_lastSpoken;
    std::chrono::steady_clock::time_point m_lastSpokenAt{};
    std::chrono::steady_clock::time_point m_screenArrivalAt{};
    std::chrono::steady_clock::time_point m_lastFocusInGraceAt{};
    std::deque<std::wstring> m_recent;
    std::wstring           m_pending;
    bool                   m_hasPending = false;
};

} // namespace PalAccess
