// Standalone smoke test for ResourceBudgetManager (see IMPROVEMENT_PLAN.md
// Phase 1 checklist: "モックのリース取得順序で検証すれば足りる。GPU実機は
// 不要"). Written without a test framework (plain assert + process exit
// code) because engine/tests/ has no GoogleTest/Catch2 dependency yet --
// that's IMPROVEMENT_PLAN.md Phase 5, which is expected to migrate this
// file's assertions into a proper GTest target rather than leaving this
// hand-rolled runner as the permanent test story.
//
// Verifies the one behavior ResourceBudgetManager exists for (design doc
// §3): NarrationEngine and SceneAssembler never hold the GPU lease at the
// same time, even when both threads race to acquire it.

#include "orchestrator/resource_budget_manager.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {

void bumpMax(std::atomic<int>& current, std::atomic<int>& maxSeen) {
    const int now = current.load();
    int prevMax = maxSeen.load();
    while (now > prevMax && !maxSeen.compare_exchange_weak(prevMax, now)) {
        // retry with the updated prevMax
    }
}

}  // namespace

int main() {
    ResourceBudgetManager manager;
    std::atomic<int> concurrentLeases{0};
    std::atomic<int> maxConcurrentLeases{0};

    // narrationThread grabs the lease first and holds it briefly, so
    // sceneThread (started shortly after) is forced to block in
    // acquireGpuLease() -- this is the actual exclusion behavior under test.
    std::thread narrationThread([&] {
        GpuLease lease = manager.acquireGpuLease(GpuLeaseOwner::NarrationEngine);
        ++concurrentLeases;
        bumpMax(concurrentLeases, maxConcurrentLeases);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        --concurrentLeases;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(10));  // let narrationThread acquire first

    std::thread sceneThread([&] {
        GpuLease lease = manager.acquireGpuLease(GpuLeaseOwner::SceneAssembler);
        ++concurrentLeases;
        bumpMax(concurrentLeases, maxConcurrentLeases);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        --concurrentLeases;
    });

    narrationThread.join();
    sceneThread.join();

    if (maxConcurrentLeases.load() != 1) {
        std::fprintf(stderr,
                     "FAIL: expected at most 1 concurrent GPU lease across "
                     "NarrationEngine/SceneAssembler, observed %d\n",
                     maxConcurrentLeases.load());
        return EXIT_FAILURE;
    }

    // A second acquire/release cycle on the same manager must still work
    // (the mutex/condition_variable state must not get stuck "held" after
    // the first release).
    {
        GpuLease lease = manager.acquireGpuLease(GpuLeaseOwner::NarrationEngine);
        (void)lease;
    }
    {
        GpuLease lease = manager.acquireGpuLease(GpuLeaseOwner::SceneAssembler);
        (void)lease;
    }

    std::printf(
        "PASS: ResourceBudgetManager enforces exclusive GPU lease across "
        "NarrationEngine/SceneAssembler\n");
    return EXIT_SUCCESS;
}
