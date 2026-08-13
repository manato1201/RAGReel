#pragma once

#include <string>

// Explicit phase typing for the video-generation job, per
// docs/architecture/video-factory-design.md §2's module table and
// IMPROVEMENT_PLAN.md Phase 1. main_cloudrag.cpp already executes these
// phases in this exact order (Ingest -> Compose -> Narrate -> Assemble ->
// Render -> Encode -> Publish); this header makes that implicit sequence
// an explicit type instead of a purely textual convention, so
// ResourceBudgetManager (resource_budget_manager.h) and ManifestWriter's
// pipeline array can both key off the same enum rather than off
// independently-typed strings.
enum class JobStage {
    Ingest,
    Compose,
    Narrate,
    Assemble,
    Render,
    Encode,
    Publish,
};

// Human-readable label for JobStage, matching the "label" strings
// main_cloudrag.cpp already writes into ManifestVideoDetail::pipeline
// (see manifest_writer.h) so callers can build that array directly from
// StageResult instances without a second lookup table.
inline const char* jobStageLabel(JobStage stage) {
    switch (stage) {
        case JobStage::Ingest: return "取り込み";
        case JobStage::Compose: return "構成 (スライド分割)";
        case JobStage::Narrate: return "ナレーション";
        case JobStage::Assemble: return "シーン組立";
        case JobStage::Render: return "レンダリング";
        case JobStage::Encode: return "エンコード";
        case JobStage::Publish: return "公開";
    }
    return "";
}

// Machine key for JobStage, matching manifest_writer.cpp's PipelineStageTiming::stage
// field (e.g. "ingest", "narrate") and docs/architecture/video-factory-design.md §5's
// metadata.json pipeline[].stage values.
inline const char* jobStageKey(JobStage stage) {
    switch (stage) {
        case JobStage::Ingest: return "ingest";
        case JobStage::Compose: return "compose";
        case JobStage::Narrate: return "narrate";
        case JobStage::Assemble: return "assemble";
        case JobStage::Render: return "render";
        case JobStage::Encode: return "encode";
        case JobStage::Publish: return "publish";
    }
    return "";
}

// Outcome of running one JobStage. An empty errorMessage means success;
// this mirrors (and is meant to directly populate) ManifestWriter's
// PipelineStageTiming, without main_cloudrag.cpp having to hand-roll a
// second parallel {stage, label, duration} structure.
struct StageResult {
    JobStage stage;
    bool success = true;
    double durationSec = 0.0;
    std::string errorMessage;  // empty if success
};
