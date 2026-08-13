#pragma once

#include <vector>

#include "orchestrator/job_pipeline.h"
#include "orchestrator/resource_budget_manager.h"

// Configuration-root / phase-bookkeeping layer for one video-generation
// job, per docs/architecture/video-factory-design.md §2 and
// IMPROVEMENT_PLAN.md Phase 1.
//
// Scope note (IMPROVEMENT_PLAN.md Phase 0 "設計書との乖離" #6): the design
// doc's Orchestrator sketch wires NarrationEngine/SceneAssembler/
// VideoEncoder/ManifestWriter/CloudRagClient together and drives the whole
// job end-to-end via a single runJob(). SceneAssembler doesn't exist yet
// (that's Phase 3 -- main_cloudrag.cpp still builds QQuickRenderControl/
// QQuickWindow/the render loop inline), and much of Ingest/Compose is
// still markdown-string-munging inline in main_cloudrag.cpp rather than
// behind a ScriptComposer (Phase 2). Building a full runJob() now would
// mean either faking a SceneAssembler that doesn't do anything real, or
// pulling Phase 2/3's extraction forward under Phase 1's changelist --
// neither is a faithful "wire the existing pieces, don't build new
// architecture" step (this plan's own stated Phase 1 rule).
//
// So this class currently does exactly the two things Phase 1 can
// deliver on its own: (1) hand out the exclusive GPU lease
// (ResourceBudgetManager, see resource_budget_manager.h) that gates
// NarrationEngine vs SceneAssembler access, and (2) accumulate
// StageResult entries as main_cloudrag.cpp's main() runs through each
// JobStage, so ManifestVideoDetail::pipeline (manifest_writer.h) can be
// built from one typed list instead of a hand-rolled parallel
// {stage, label, duration} initializer. main_cloudrag.cpp still owns the
// actual phase bodies (narration synthesis, QML scene setup, the render
// loop, encoding, publishing) -- Orchestrator does not call into
// NarrationEngine/VideoEncoder/etc. itself. Phase 2/3 are expected to grow
// this into the fuller runJob()-driven class the design doc describes,
// once ScriptComposer and SceneAssembler exist to be driven.
class Orchestrator {
public:
    Orchestrator() = default;
    Orchestrator(const Orchestrator&) = delete;
    Orchestrator& operator=(const Orchestrator&) = delete;

    // Pass-through to the owned ResourceBudgetManager -- see its class
    // comment for the exclusion contract this enforces.
    [[nodiscard]] GpuLease acquireGpuLease(GpuLeaseOwner owner) {
        return resourceBudgetManager_.acquireGpuLease(owner);
    }

    // Records one phase's outcome. Call once per JobStage as main()
    // completes each phase; recorded in call order.
    void recordStage(JobStage stage, bool success, double durationSec,
                      std::string errorMessage = {}) {
        stageResults_.push_back(
            StageResult{stage, success, durationSec, std::move(errorMessage)});
    }

    const std::vector<StageResult>& stageResults() const { return stageResults_; }

private:
    ResourceBudgetManager resourceBudgetManager_;
    std::vector<StageResult> stageResults_;
};
