#pragma once

#include <condition_variable>
#include <mutex>

// Exclusive GPU-context gatekeeper, per docs/architecture/video-factory-
// design.md §3 ("VRAM/スレッド競合設計(最重要リスク)") and
// IMPROVEMENT_PLAN.md Phase 1.
//
// §3 explicitly scopes this component's job as exclusion, not accounting:
// "ResourceBudgetManagerの役割は細かいメモリ会計ではなく排他制御" -- it
// does not track bytes used. It only guarantees that at most one of
// {NarrationEngine, SceneAssembler} holds the GPU at a time, and that
// releasing a lease actually tears the context down rather than merely
// idling it (that half of the contract is the caller's responsibility --
// see GpuLease's doc comment below).
//
// IMPROVEMENT_PLAN.md's Phase 0 review found that the currently-shipping
// NarrationEngine is Windows SAPI5 (engine/src/narration/narration_engine.h),
// which never touches the GPU -- so this class is not preventing an active
// crash today. It exists so the exclusion boundary is already in place,
// tested, and wired into main_cloudrag.cpp's control flow before
// NarrationEngine is ever switched to a GPU-resident llama.cpp backend (the
// design doc's originally intended implementation), at which point this
// becomes load-bearing without requiring a second pass through the call
// sites that already acquire/release leases.
enum class GpuLeaseOwner {
    NarrationEngine,
    SceneAssembler,
};

class ResourceBudgetManager;

// RAII handle for a held GPU lease. Move-only (a lease has exactly one
// owning scope at a time). The destructor releases the lease and wakes any
// other thread blocked in ResourceBudgetManager::acquireGpuLease().
//
// This class only manages the *gate*, not the GPU resource itself: it is
// the caller's responsibility to ensure that whatever GPU context/session
// it constructed while holding the lease is actually torn down (not merely
// left idle) before the lease is released -- e.g. destroying the
// llama.cpp context, or the QQuickRenderControl/QRhi objects, before
// returning from the scope that holds the GpuLease. §3 is explicit that an
// idled-but-still-resident context does not satisfy this design ("実際に
// 確保領域が解放されること...単なるアイドル化では不可").
class GpuLease {
public:
    GpuLease(GpuLease&& other) noexcept;
    GpuLease& operator=(GpuLease&& other) noexcept;
    ~GpuLease();

    GpuLease(const GpuLease&) = delete;
    GpuLease& operator=(const GpuLease&) = delete;

    GpuLeaseOwner owner() const { return owner_; }

private:
    friend class ResourceBudgetManager;
    GpuLease(ResourceBudgetManager* manager, GpuLeaseOwner owner);

    ResourceBudgetManager* manager_;  // null after being moved-from or released
    GpuLeaseOwner owner_;
};

class ResourceBudgetManager {
public:
    ResourceBudgetManager() = default;
    ResourceBudgetManager(const ResourceBudgetManager&) = delete;
    ResourceBudgetManager& operator=(const ResourceBudgetManager&) = delete;

    // Blocks the calling thread until no lease is currently outstanding
    // (regardless of which owner holds it -- see the class comment: only
    // one of NarrationEngine/SceneAssembler may hold the GPU at a time),
    // then takes the lease for `owner` and returns it. The returned
    // GpuLease releases automatically at the end of its scope.
    [[nodiscard]] GpuLease acquireGpuLease(GpuLeaseOwner owner);

private:
    friend class GpuLease;
    void release();

    std::mutex mutex_;
    std::condition_variable cv_;
    bool leaseHeld_ = false;
};
