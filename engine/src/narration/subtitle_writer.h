#pragma once

#include <QString>
#include <QStringList>

#include <vector>

#include "narration/narration_engine.h"  // NarrationSegment only

// Subtitle file generation from narration timing (IMPROVEMENT_PLAN_2026-10.md
// V7). Pure functions (QtCore only) so they are unit-testable without an
// engine, GPU or display, in the same style as narration_text / wav_util.
namespace subtitle_writer {

// "HH:MM:SS,mmm" (SRT) or "HH:MM:SS.mmm" (WebVTT). Negative input clamps to 0.
QString formatTimestamp(double seconds, bool vtt);

// Breaks Japanese text into display lines of at most `maxChars` characters,
// preferring a break just after 、/。 when one falls in the second half of the
// line. At most `maxLines` lines are returned; any remainder is appended to
// the last line (never dropped).
QStringList wrapLines(const QString& text, int maxChars = 22, int maxLines = 2);

// Segments whose end <= start or whose text is blank are skipped. Cue indexes
// are renumbered from 1 over the cues actually written.
QString toSrt(const std::vector<NarrationSegment>& segments);
QString toVtt(const std::vector<NarrationSegment>& segments);

}  // namespace subtitle_writer
