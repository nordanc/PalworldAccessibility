#include "Speech.hpp"
#include "TolkBridge.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>

namespace PalAccess {

Speech& Speech::Get() {
    static Speech instance;
    return instance;
}

std::wstring Speech::WidenUtf8(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(
        CP_UTF8, 0,
        utf8.data(), static_cast<int>(utf8.size()),
        nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0,
        utf8.data(), static_cast<int>(utf8.size()),
        out.data(), n);
    return out;
}

void Speech::Cue(const std::wstring& text) {
    auto& tolk = TolkBridge::Get();
    if (tolk.IsSpeaking()) {
        m_pending = text;
        m_hasPending = true;
    } else {
        tolk.Output(text.c_str(), /*interrupt=*/false);
        m_pending.clear();
        m_hasPending = false;
    }
}

void Speech::Speak(std::wstring_view text) {
    if (text.empty()) return;
    auto& tolk = TolkBridge::Get();
    if (!tolk.IsLoaded()) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    const auto now = std::chrono::steady_clock::now();
    const std::wstring as_str(text);

    if (as_str == m_lastSpoken) {
        const auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - m_lastSpokenAt).count();
        if (delta < DedupeMillis) return;
    }
    if (std::find(m_recent.begin(), m_recent.end(), as_str) != m_recent.end()) {
        return;
    }

    Cue(as_str);

    m_lastSpoken = as_str;
    m_lastSpokenAt = now;
    m_recent.push_back(as_str);
    while (static_cast<int>(m_recent.size()) > RecentBuffer) {
        m_recent.pop_front();
    }
}

void Speech::Speak(std::string_view utf8) {
    Speak(std::wstring_view(WidenUtf8(utf8)));
}

void Speech::Announce(std::wstring_view text) {
    if (text.empty()) return;
    auto& tolk = TolkBridge::Get();
    if (!tolk.IsLoaded()) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    const std::wstring as_str(text);
    // No coalescing — Announce goes straight to Tolk's native queue so
    // multiple notifications all play in full and in order.
    tolk.Output(as_str.c_str(), /*interrupt=*/false);
    m_lastSpoken = as_str;
    m_lastSpokenAt = std::chrono::steady_clock::now();
}

void Speech::Announce(std::string_view utf8) {
    Announce(std::wstring_view(WidenUtf8(utf8)));
}

void Speech::FocusUpdate(std::wstring_view text) {
    if (text.empty()) return;
    auto& tolk = TolkBridge::Get();
    if (!tolk.IsLoaded()) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    const auto now = std::chrono::steady_clock::now();
    const std::wstring as_str(text);

    if (as_str == m_lastSpoken) {
        const auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - m_lastSpokenAt).count();
        if (delta < FocusDedupeMillis) return;
    }

    // A screen-arrival cascade is in progress if either:
    //   (a) we are inside the initial ScreenArrivalGraceMillis after the
    //       panel arrived, or
    //   (b) another focus event during the cascade fired recently —
    //       panel construction tends to fire BP_OnHovered / AnmEvent_Focus
    //       for every row as it animates in, sometimes well past the
    //       initial grace.
    // During the cascade we don't speak at all: we just keep overwriting
    // a single pending slot so the *last* focus event (the one the user
    // actually ends up on) is what Tick() eventually flushes.
    const auto sinceArrival = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - m_screenArrivalAt).count();
    const bool cascadeActive = m_lastFocusInGraceAt.time_since_epoch().count() != 0;
    const auto sinceCascade = cascadeActive
        ? std::chrono::duration_cast<std::chrono::milliseconds>(
              now - m_lastFocusInGraceAt).count()
        : 0;
    const bool inCascade =
        (sinceArrival >= 0 && sinceArrival < ScreenArrivalGraceMillis) ||
        (cascadeActive && sinceCascade < FocusCascadeDebounceMillis);

    if (inCascade) {
        m_pending = as_str;
        m_hasPending = true;
        m_lastFocusInGraceAt = now;
        m_lastSpoken = as_str;
        m_lastSpokenAt = now;
        return;
    }

    // Normal case: user-driven navigation. Interrupt current speech for
    // snappy scrolling, and drop any cued pending — the player has
    // moved on and a stale cue would speak the wrong context.
    m_pending.clear();
    m_hasPending = false;
    m_lastFocusInGraceAt = {};
    tolk.Output(as_str.c_str(), /*interrupt=*/true);

    m_lastSpoken = as_str;
    m_lastSpokenAt = now;
}

void Speech::FocusUpdate(std::string_view utf8) {
    FocusUpdate(std::wstring_view(WidenUtf8(utf8)));
}

void Speech::SpeakNow(std::wstring_view text) {
    if (text.empty()) return;
    auto& tolk = TolkBridge::Get();
    if (!tolk.IsLoaded()) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    const std::wstring as_str(text);
    // Hard interrupt: clear pending, reset cascade tracking, output now.
    m_pending.clear();
    m_hasPending = false;
    m_lastFocusInGraceAt = {};
    tolk.Output(as_str.c_str(), /*interrupt=*/true);
    m_lastSpoken = as_str;
    m_lastSpokenAt = std::chrono::steady_clock::now();
}

void Speech::SpeakNow(std::string_view utf8) {
    SpeakNow(std::wstring_view(WidenUtf8(utf8)));
}

void Speech::Queue(std::wstring_view text) {
    if (text.empty()) return;
    auto& tolk = TolkBridge::Get();
    if (!tolk.IsLoaded()) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::wstring as_str(text);
    tolk.Output(as_str.c_str(), /*interrupt=*/false);
}

void Speech::NotifyScreenArrival() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_screenArrivalAt = std::chrono::steady_clock::now();
}

void Speech::Tick() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_hasPending) return;
    auto& tolk = TolkBridge::Get();
    if (!tolk.IsLoaded()) return;
    if (tolk.IsSpeaking()) return;

    // Hold the pending utterance while a screen-arrival cascade is
    // still running. Two conditions keep us in the cascade:
    //   (a) we're still inside the initial ScreenArrivalGraceMillis, or
    //   (b) a focus event during the cascade fired within the last
    //       FocusCascadeDebounceMillis.
    // Either way, more focus events are probably coming and we want
    // to flush only the *last* one.
    const auto now = std::chrono::steady_clock::now();
    const auto sinceArrival = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - m_screenArrivalAt).count();
    if (sinceArrival >= 0 && sinceArrival < ScreenArrivalGraceMillis) return;
    if (m_lastFocusInGraceAt.time_since_epoch().count() != 0) {
        const auto sinceCascade = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - m_lastFocusInGraceAt).count();
        if (sinceCascade < FocusCascadeDebounceMillis) return;
    }

    tolk.Output(m_pending.c_str(), /*interrupt=*/false);
    m_pending.clear();
    m_hasPending = false;
    m_lastFocusInGraceAt = {};
}

void Speech::Silence() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pending.clear();
        m_hasPending = false;
    }
    TolkBridge::Get().Silence();
}

} // namespace PalAccess
