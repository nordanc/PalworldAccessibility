#include "TolkBridge.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace PalAccess {

TolkBridge& TolkBridge::Get() {
    static TolkBridge instance;
    return instance;
}

bool TolkBridge::Load() {
    if (m_loaded) return true;

    HMODULE mod = LoadLibraryW(L"Tolk.dll");
    if (!mod) return false;
    m_module = mod;

    m_pLoad       = reinterpret_cast<FnLoad>        (GetProcAddress(mod, "Tolk_Load"));
    m_pUnload     = reinterpret_cast<FnUnload>      (GetProcAddress(mod, "Tolk_Unload"));
    m_pTrySAPI    = reinterpret_cast<FnTrySAPI>     (GetProcAddress(mod, "Tolk_TrySAPI"));
    m_pPreferSAPI = reinterpret_cast<FnPreferSAPI>  (GetProcAddress(mod, "Tolk_PreferSAPI"));
    m_pIsLoaded   = reinterpret_cast<FnIsLoaded>    (GetProcAddress(mod, "Tolk_IsLoaded"));
    m_pHasSpeech  = reinterpret_cast<FnHasSpeech>   (GetProcAddress(mod, "Tolk_HasSpeech"));
    m_pDetect     = reinterpret_cast<FnDetectReader>(GetProcAddress(mod, "Tolk_DetectScreenReader"));
    m_pOutput     = reinterpret_cast<FnOutput>      (GetProcAddress(mod, "Tolk_Output"));
    m_pSpeak      = reinterpret_cast<FnSpeak>       (GetProcAddress(mod, "Tolk_Speak"));
    m_pSilence    = reinterpret_cast<FnSilence>     (GetProcAddress(mod, "Tolk_Silence"));

    if (!m_pLoad || !m_pOutput || !m_pSilence) {
        FreeLibrary(mod);
        m_module = nullptr;
        return false;
    }

    // Enable SAPI as fallback so users without a screen reader still hear something useful.
    if (m_pTrySAPI)   m_pTrySAPI(true);
    if (m_pPreferSAPI) m_pPreferSAPI(false);

    m_pLoad();
    m_loaded = true;
    return true;
}

void TolkBridge::Unload() {
    if (!m_loaded) return;
    if (m_pUnload) m_pUnload();
    if (m_module) FreeLibrary(static_cast<HMODULE>(m_module));
    m_module = nullptr;
    m_loaded = false;
}

bool TolkBridge::HasSpeech() const {
    return m_loaded && m_pHasSpeech && m_pHasSpeech();
}

const wchar_t* TolkBridge::DetectScreenReader() const {
    return (m_loaded && m_pDetect) ? m_pDetect() : nullptr;
}

bool TolkBridge::Output(const wchar_t* text, bool interrupt) {
    return (m_loaded && m_pOutput && text) ? m_pOutput(text, interrupt) : false;
}

bool TolkBridge::Speak(const wchar_t* text, bool interrupt) {
    return (m_loaded && m_pSpeak && text) ? m_pSpeak(text, interrupt) : false;
}

bool TolkBridge::Silence() {
    return (m_loaded && m_pSilence) ? m_pSilence() : false;
}

} // namespace PalAccess
