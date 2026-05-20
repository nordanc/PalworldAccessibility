#include "PatchNotes.hpp"
#include "Speech.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include <DynamicOutput/Output.hpp>
#include <DynamicOutput/DynamicOutput.hpp>

#include <atomic>
#include <cctype>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "winhttp.lib")

using namespace RC;

namespace PalAccess {

namespace {

std::atomic<bool> g_fetch_in_flight{false};
std::atomic<bool> g_cancel_requested{false};
std::mutex        g_thread_mtx;

// Parse a URL into host + path. Returns false if not HTTPS or malformed.
bool ParseHttpsUrl(std::wstring_view url, std::wstring& host, std::wstring& path) {
    constexpr std::wstring_view kPrefix = L"https://";
    if (url.size() < kPrefix.size() ||
        url.substr(0, kPrefix.size()) != kPrefix) {
        return false;
    }
    auto rest = url.substr(kPrefix.size());
    auto slash = rest.find(L'/');
    if (slash == std::wstring_view::npos) {
        host = std::wstring(rest);
        path = L"/";
    } else {
        host = std::wstring(rest.substr(0, slash));
        path = std::wstring(rest.substr(slash));
    }
    // Strip any trailing NUL (the URL came from FString::GetCharArray()
    // which is NUL-terminated).
    while (!host.empty() && host.back() == L'\0') host.pop_back();
    while (!path.empty() && path.back() == L'\0') path.pop_back();
    return !host.empty();
}

std::string HttpsGet(const std::wstring& host, const std::wstring& path) {
    std::string body;
    HINTERNET hSession = WinHttpOpen(
        L"PalAccess/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return body;
    WinHttpSetTimeouts(hSession, 5000, 5000, 5000, 10000);

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return body; }

    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"GET", path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return body;
    }

    BOOL ok = WinHttpSendRequest(
        hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (ok) ok = WinHttpReceiveResponse(hRequest, nullptr);

    if (ok) {
        DWORD avail = 0;
        do {
            avail = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &avail)) break;
            if (avail == 0) break;
            std::vector<char> buf(avail);
            DWORD read = 0;
            if (!WinHttpReadData(hRequest, buf.data(), avail, &read)) break;
            body.append(buf.data(), read);
            if (body.size() > 4 * 1024 * 1024) break;  // 4 MB safety cap
        } while (avail > 0);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return body;
}

std::string DecodeHtmlEntities(std::string s) {
    struct Ent { const char* name; const char* repl; };
    static constexpr Ent kEnts[] = {
        {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"},
        {"&quot;", "\""}, {"&apos;", "'"}, {"&#39;", "'"},
        {"&nbsp;", " "}, {"&ndash;", "-"}, {"&mdash;", "-"},
        {"&hellip;", "..."}, {"&rsquo;", "'"}, {"&lsquo;", "'"},
        {"&ldquo;", "\""}, {"&rdquo;", "\""},
    };
    for (const auto& e : kEnts) {
        std::string::size_type p = 0;
        while ((p = s.find(e.name, p)) != std::string::npos) {
            s.replace(p, std::strlen(e.name), e.repl);
            p += std::strlen(e.repl);
        }
    }
    return s;
}

// Remove every <script>...</script> and <style>...</style> block (and
// HTML comments) before tag stripping — their contents otherwise leak
// through as text.
std::string StripScriptsAndStyles(std::string s) {
    auto erase_between = [&](std::string_view open, std::string_view close) {
        std::string::size_type p = 0;
        while ((p = s.find(open, p)) != std::string::npos) {
            auto q = s.find(close, p + open.size());
            if (q == std::string::npos) {
                s.erase(p);
                break;
            }
            s.erase(p, q - p + close.size());
        }
    };
    erase_between("<script", "</script>");
    erase_between("<style",  "</style>");
    erase_between("<!--",    "-->");
    return s;
}

// Block-level closers and <br> become newlines so paragraph splitting
// works; everything else just disappears.
std::string StripTags(std::string s) {
    // Replace block boundaries with explicit newlines first.
    static constexpr const char* kBreaks[] = {
        "<br>", "<br/>", "<br />", "</p>", "</div>", "</li>",
        "</h1>", "</h2>", "</h3>", "</h4>", "</h5>", "</h6>",
        "</article>", "</section>",
    };
    for (auto* tag : kBreaks) {
        std::string::size_type p = 0;
        while ((p = s.find(tag, p)) != std::string::npos) {
            s.replace(p, std::strlen(tag), "\n");
            p += 1;
        }
    }
    // Now strip every remaining <...>.
    std::string out;
    out.reserve(s.size());
    bool in_tag = false;
    for (char c : s) {
        if (c == '<')      { in_tag = true;  continue; }
        if (c == '>')      { in_tag = false; continue; }
        if (!in_tag) out += c;
    }
    return out;
}

// True if at least `threshold` fraction of non-space chars are 7-bit ASCII.
// Used to drop Japanese / Chinese paragraphs the page also serves.
bool MostlyAscii(std::string_view s, double threshold = 0.80) {
    size_t total = 0, ascii = 0;
    for (unsigned char c : s) {
        if (c <= 32) continue;            // skip whitespace + control
        ++total;
        if (c < 0x80) ++ascii;
    }
    if (total < 6) return false;          // too short to judge
    return static_cast<double>(ascii) / total >= threshold;
}

// Collapse runs of whitespace inside a single line.
std::string CollapseWhitespace(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    bool prev_space = false;
    for (char c : in) {
        if (c == '\t' || c == '\r') c = ' ';
        if (c == ' ') {
            if (!prev_space) out += ' ';
            prev_space = true;
        } else {
            out += c;
            prev_space = false;
        }
    }
    // Trim leading/trailing space.
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    while (!out.empty() && out.back()  == ' ') out.pop_back();
    return out;
}

std::vector<std::string> SplitParagraphs(const std::string& s) {
    std::vector<std::string> out;
    std::string current;
    for (char c : s) {
        if (c == '\n') {
            auto line = CollapseWhitespace(current);
            current.clear();
            if (!line.empty()) out.push_back(std::move(line));
        } else {
            current += c;
        }
    }
    auto line = CollapseWhitespace(current);
    if (!line.empty()) out.push_back(std::move(line));
    return out;
}

std::wstring WidenUtf8(std::string_view utf8) {
    if (utf8.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), out.data(), n);
    return out;
}

void FetchAndSpeakWorker(std::wstring url) {
    g_fetch_in_flight.store(true);
    g_cancel_requested.store(false);

    std::wstring host, path;
    if (!ParseHttpsUrl(url, host, path)) {
        Output::send<LogLevel::Default>(
            STR("[PalAccess] PatchNotes: bad URL\n"));
        g_fetch_in_flight.store(false);
        return;
    }

    Output::send<LogLevel::Default>(
        STR("[PalAccess] PatchNotes: fetching {}{}\n"), host, path);

    std::string body = HttpsGet(host, path);
    if (g_cancel_requested.load()) { g_fetch_in_flight.store(false); return; }

    if (body.empty()) {
        Output::send<LogLevel::Default>(
            STR("[PalAccess] PatchNotes: empty response\n"));
        Speech::Get().Queue(L"Could not load patch notes.");
        g_fetch_in_flight.store(false);
        return;
    }
    Output::send<LogLevel::Default>(
        STR("[PalAccess] PatchNotes: got {} bytes\n"), body.size());

    body = StripScriptsAndStyles(std::move(body));
    body = StripTags(std::move(body));
    body = DecodeHtmlEntities(std::move(body));

    auto paragraphs = SplitParagraphs(body);

    // Filter: keep only mostly-ASCII paragraphs (English content) and skip
    // very short fragments that are almost always nav links or share
    // buttons.
    std::vector<std::string> keep;
    keep.reserve(paragraphs.size());
    for (auto& p : paragraphs) {
        if (p.size() < 24) continue;
        if (!MostlyAscii(p)) continue;
        keep.push_back(std::move(p));
    }

    if (g_cancel_requested.load()) { g_fetch_in_flight.store(false); return; }

    if (keep.empty()) {
        Speech::Get().Queue(L"No English patch notes content found.");
        Output::send<LogLevel::Default>(
            STR("[PalAccess] PatchNotes: zero kept paragraphs (out of {})\n"),
            paragraphs.size());
        g_fetch_in_flight.store(false);
        return;
    }

    Output::send<LogLevel::Default>(
        STR("[PalAccess] PatchNotes: queuing {} paragraphs\n"), keep.size());

    {
        std::wstring intro = L"Reading patch notes. ";
        intro += std::to_wstring(keep.size());
        intro += L" paragraphs. Press F8 to silence.";
        Speech::Get().Queue(std::wstring_view(intro));
    }

    for (auto& p : keep) {
        if (g_cancel_requested.load()) break;
        Speech::Get().Queue(std::wstring_view(WidenUtf8(p)));
    }
    g_fetch_in_flight.store(false);
}

} // namespace

void PatchNotes::Fetch(std::wstring_view url) {
    if (g_fetch_in_flight.load()) return;
    std::lock_guard<std::mutex> lock(g_thread_mtx);
    std::wstring url_copy(url);
    std::thread([u = std::move(url_copy)]() mutable {
        FetchAndSpeakWorker(std::move(u));
    }).detach();
}

void PatchNotes::Cancel() {
    g_cancel_requested.store(true);
    Speech::Get().Silence();
}

} // namespace PalAccess
