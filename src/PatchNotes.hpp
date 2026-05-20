#pragma once

#include <string>
#include <string_view>

namespace PalAccess {

// Fetch the patch-notes URL in a background thread, strip HTML, keep the
// English-language paragraphs, and queue them through Speech for sequential
// playback. Safe to call multiple times — a second call while a fetch is
// in flight is a no-op.
class PatchNotes {
public:
    static void Fetch(std::wstring_view url);

    // Discard any in-flight fetch (the result will be ignored) and clear
    // pending speech. Hook this to Destruct of the news widget so closing
    // the screen stops the reader.
    static void Cancel();

private:
    PatchNotes() = default;
};

} // namespace PalAccess
