#pragma once

#include <string>
#include <string_view>

namespace PalAccess {

class TolkBridge {
public:
    static TolkBridge& Get();

    bool Load();
    void Unload();

    bool IsLoaded() const { return m_loaded; }
    bool HasSpeech() const;
    const wchar_t* DetectScreenReader() const;

    bool Output(const wchar_t* text, bool interrupt);
    bool Speak(const wchar_t* text, bool interrupt);
    bool Silence();

private:
    TolkBridge() = default;
    ~TolkBridge() = default;
    TolkBridge(const TolkBridge&) = delete;
    TolkBridge& operator=(const TolkBridge&) = delete;

    using FnLoad           = void (__cdecl*)();
    using FnUnload         = void (__cdecl*)();
    using FnTrySAPI        = void (__cdecl*)(bool);
    using FnPreferSAPI     = void (__cdecl*)(bool);
    using FnIsLoaded       = bool (__cdecl*)();
    using FnHasSpeech      = bool (__cdecl*)();
    using FnDetectReader   = const wchar_t* (__cdecl*)();
    using FnOutput         = bool (__cdecl*)(const wchar_t*, bool);
    using FnSpeak          = bool (__cdecl*)(const wchar_t*, bool);
    using FnSilence        = bool (__cdecl*)();

    void*           m_module        = nullptr;
    bool            m_loaded        = false;
    FnLoad          m_pLoad         = nullptr;
    FnUnload        m_pUnload       = nullptr;
    FnTrySAPI       m_pTrySAPI      = nullptr;
    FnPreferSAPI    m_pPreferSAPI   = nullptr;
    FnIsLoaded      m_pIsLoaded     = nullptr;
    FnHasSpeech     m_pHasSpeech    = nullptr;
    FnDetectReader  m_pDetect       = nullptr;
    FnOutput        m_pOutput       = nullptr;
    FnSpeak         m_pSpeak        = nullptr;
    FnSilence       m_pSilence      = nullptr;
};

} // namespace PalAccess
