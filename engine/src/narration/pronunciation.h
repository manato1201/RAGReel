#pragma once

#include <QByteArray>
#include <QString>

#include <vector>

// Pronunciation fixes for text that is handed to a TTS engine (2026-10-19).
//
// Why this exists: the SAPI5 voice on this PC ("Microsoft Haruka Desktop") is a Japanese
// voice, so English terms in the script ("Houdini", "Attribute Wrangle", "VEX") are read
// letter-by-letter or in broken katakana. The VOICEVOX path already registers
// engine/assets/tts_dictionary.json as a user dictionary, but the SAPI path (the one that
// actually runs when VOICEVOX is not installed) never used the dictionary at all.
//
// This module replaces dictionary terms with their katakana reading BEFORE the text is
// spoken. Subtitles keep the original spelling (only the spoken text is rewritten).
// Qt6::Core only, so tests/narration_test.cpp can cover it without any engine.
namespace pronunciation {

struct Entry {
    QString surface;  // as written in the script, e.g. "Attribute Wrangle"
    QString reading;  // full-width katakana, e.g. "アトリビュートラングル"
};

// Parses a JSON array of {"surface","pronunciation",...} objects (the format of
// engine/assets/tts_dictionary.json). Malformed entries are skipped; a malformed
// document gives an empty list.
std::vector<Entry> parseDictionary(const QByteArray& json);

// Reads a dictionary file. A missing/unreadable file gives an empty list (never throws).
std::vector<Entry> loadDictionary(const QString& path);

// RAGREEL_TTS_DICTIONARY if set, else <application dir>/assets/tts_dictionary.json.
QString defaultDictionaryPath();

// Replaces every dictionary term in `text` with its reading. Longest surface first, so
// "Attribute Wrangle" wins over "Wrangle". ASCII terms match case-insensitively and only as
// whole words ("VEX" does not match inside "VEXpression"); non-ASCII terms match anywhere.
QString replaceTerms(const QString& text, const std::vector<Entry>& dict);

// Spells out the ALL-CAPS acronyms that are still left after replaceTerms() ("ABC" ->
// "エービーシー"), as TTS voices otherwise mumble them. 2-6 letters, whole words only.
QString spellOutAcronyms(const QString& text);

// What the SAPI engine runs on the text before Speak(): replaceTerms(default dictionary) then
// spellOutAcronyms. The dictionary is loaded once per process.
QString prepareForSapi(const QString& text);

}  // namespace pronunciation
