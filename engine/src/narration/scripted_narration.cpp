#include "narration/scripted_narration.h"

#include "narration/voicevox_narration_engine.h"  // wav_util

#include <QFile>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace scripted_narration {

namespace {

std::vector<int16_t> readWavAsMono(const QString& path, int targetRate) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        throw std::runtime_error("cannot read narration part: " + path.toStdString());
    }
    const auto parsed = wav_util::parsePcm16(f.readAll());
    if (!parsed || parsed->samples.empty()) {
        throw std::runtime_error("narration part is not a readable PCM16 WAV: " + path.toStdString());
    }
    if (parsed->sampleRate == targetRate) return parsed->samples;
    return wav_util::resample(parsed->samples, parsed->sampleRate, targetRate);
}

}  // namespace

Result synthesizeBlocks(const SynthFn& synth, const QStringList& texts, const QString& wavPath,
                        const Options& options) {
    Result result;
    std::vector<int16_t> all;
    const int rate = options.sampleRate;
    const auto appendSilence = [&](double seconds) {
        const std::vector<int16_t> s = wav_util::silence(rate, seconds);
        all.insert(all.end(), s.begin(), s.end());
    };

    QStringList partFiles;
    bool allExact = true;  // every spoken block gave exact segment times (VOICEVOX); SAPI's are estimates
    for (int i = 0; i < texts.size(); ++i) {
        Block block;
        block.text = texts[i].trimmed();
        block.startSec = static_cast<double>(all.size()) / rate;

        if (block.text.isEmpty()) {
            appendSilence(options.holdSeconds);
            block.speechEndSec = block.startSec;
        } else {
            const QString part = QStringLiteral("%1.part%2.wav").arg(wavPath).arg(i);
            partFiles << part;
            const NarrationResult r = synth(block.text, part);
            const std::vector<int16_t> samples = readWavAsMono(part, rate);
            const double offset = static_cast<double>(all.size()) / rate;
            all.insert(all.end(), samples.begin(), samples.end());
            block.speechEndSec = static_cast<double>(all.size()) / rate;
            for (NarrationSegment seg : r.segments) {
                seg.startSec += offset;
                seg.endSec += offset;
                result.merged.segments.push_back(seg);
            }
            if (i == 0 || result.merged.engineName.isEmpty()) result.merged.engineName = r.engineName;
            allExact = allExact && r.segmentsExact;
            if (r.engineName != result.merged.engineName && !r.engineName.isEmpty() &&
                !result.merged.engineName.contains(r.engineName)) {
                result.merged.engineName += QStringLiteral(" / ") + r.engineName;  // mixed (a fallback mid-run)
            }
            appendSilence(options.gapSeconds);
        }
        block.endSec = static_cast<double>(all.size()) / rate;
        result.blocks.push_back(block);
    }
    if (all.empty()) throw std::runtime_error("no narration blocks to synthesise");

    const QByteArray wav = wav_util::buildWavPcm16Mono(rate, all);
    QFile out(wavPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        throw std::runtime_error("cannot write " + wavPath.toStdString());
    }
    out.write(wav);
    out.close();
    for (const QString& part : partFiles) QFile::remove(part);

    result.merged.wavPath = wavPath;
    result.merged.segmentsExact = allExact && !partFiles.isEmpty();
    result.merged.durationSeconds = static_cast<double>(all.size()) / rate;
    return result;
}

std::vector<int> startFramesFromBlocks(const std::vector<Block>& blocks, int fps, int totalFrames) {
    std::vector<int> starts;
    starts.reserve(blocks.size() + 1);
    int previous = 0;
    for (size_t i = 0; i < blocks.size(); ++i) {
        int f = i == 0 ? 0 : static_cast<int>(std::lround(blocks[i].startSec * fps));
        f = std::clamp(f, previous, totalFrames);
        starts.push_back(f);
        previous = f;
    }
    starts.push_back(totalFrames);
    return starts;
}

}  // namespace scripted_narration
