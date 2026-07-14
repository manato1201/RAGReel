#include "narration_engine.h"

#include <windows.h>

#include <sapi.h>
#include <wrl/client.h>

#include <QFileInfo>

#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {

void throwIfFailed(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
        throw std::runtime_error(std::string(what) + " failed, HRESULT=" + buf);
    }
}

// Best-effort: selects an installed ja-JP voice (LCID 0x411) if one exists.
// Leaves SAPI's system default voice in place if none is found -- narration
// still gets produced, just possibly mispronounced, which is preferable to
// hard-failing the whole pipeline over a missing language pack.
void trySelectJapaneseVoice(ISpVoice* voice) {
    ComPtr<ISpObjectTokenCategory> category;
    if (FAILED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL,
                                 IID_ISpObjectTokenCategory, &category))) {
        return;
    }
    if (FAILED(category->SetId(SPCAT_VOICES, FALSE))) {
        return;
    }
    ComPtr<IEnumSpObjectTokens> tokens;
    if (FAILED(category->EnumTokens(L"language=411", nullptr, &tokens)) || !tokens) {
        return;
    }
    ComPtr<ISpObjectToken> token;
    if (tokens->Next(1, &token, nullptr) == S_OK && token) {
        voice->SetVoice(token.Get());
    }
}

} // namespace

NarrationResult NarrationEngine::synthesize(const QString& text, const QString& wavPath) {
    const HRESULT coInitHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(coInitHr) && coInitHr != RPC_E_CHANGED_MODE) {
        throwIfFailed(coInitHr, "CoInitializeEx");
    }
    const bool needsUninit = (coInitHr != RPC_E_CHANGED_MODE);
    struct ComGuard {
        bool active;
        ~ComGuard() {
            if (active) CoUninitialize();
        }
    } comGuard{needsUninit};

    WAVEFORMATEX wfx{};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 1;
    wfx.nSamplesPerSec = 44100;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = static_cast<WORD>(wfx.nChannels * wfx.wBitsPerSample / 8);
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
    wfx.cbSize = 0;

    ComPtr<ISpStream> stream;
    throwIfFailed(
        CoCreateInstance(CLSID_SpStream, nullptr, CLSCTX_ALL, IID_ISpStream, &stream),
        "CoCreateInstance(SpStream)");

    const std::wstring wpath = wavPath.toStdWString();
    throwIfFailed(stream->BindToFile(wpath.c_str(), SPFM_CREATE_ALWAYS, &SPDFID_WaveFormatEx, &wfx,
                                      SPFEI_ALL_EVENTS),
                  "ISpStream::BindToFile");

    ComPtr<ISpVoice> voice;
    throwIfFailed(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice, &voice),
                  "CoCreateInstance(SpVoice)");
    throwIfFailed(voice->SetOutput(stream.Get(), TRUE), "ISpVoice::SetOutput");
    trySelectJapaneseVoice(voice.Get());

    const std::wstring wtext = text.toStdWString();
    // SPF_DEFAULT (0) is synchronous: Speak() blocks until the whole
    // utterance has been written to the bound file stream.
    throwIfFailed(voice->Speak(wtext.c_str(), SPF_DEFAULT, nullptr), "ISpVoice::Speak");
    stream->Close();

    const qint64 fileSize = QFileInfo(wavPath).size();
    constexpr qint64 kCanonicalWavHeaderSize = 44;
    if (fileSize <= kCanonicalWavHeaderSize) {
        throw std::runtime_error(
            "SAPI produced an empty WAV file -- is any TTS voice installed? "
            "(Settings > Time & Language > Speech > Manage voices)");
    }

    NarrationResult result;
    result.wavPath = wavPath;
    result.durationSeconds =
        static_cast<double>(fileSize - kCanonicalWavHeaderSize) / wfx.nAvgBytesPerSec;
    return result;
}
