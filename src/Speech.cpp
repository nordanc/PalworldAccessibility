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

    tolk.Output(as_str.c_str(), /*interrupt=*/false);

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
    tolk.Output(as_str.c_str(), /*interrupt=*/true);
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

    tolk.Output(as_str.c_str(), /*interrupt=*/true);
    m_lastSpoken = as_str;
    m_lastSpokenAt = now;
}

void Speech::FocusUpdate(std::string_view utf8) {
    FocusUpdate(std::wstring_view(WidenUtf8(utf8)));
}

void Speech::Queue(std::wstring_view text) {
    if (text.empty()) return;
    auto& tolk = TolkBridge::Get();
    if (!tolk.IsLoaded()) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::wstring as_str(text);
    tolk.Output(as_str.c_str(), /*interrupt=*/false);
}

void Speech::Silence() {
    TolkBridge::Get().Silence();
}

} // namespace PalAccess
