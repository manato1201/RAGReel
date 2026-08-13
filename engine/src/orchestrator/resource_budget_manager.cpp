#include "orchestrator/resource_budget_manager.h"

#include <utility>

GpuLease::GpuLease(ResourceBudgetManager* manager, GpuLeaseOwner owner)
    : manager_(manager), owner_(owner) {}

GpuLease::GpuLease(GpuLease&& other) noexcept
    : manager_(std::exchange(other.manager_, nullptr)), owner_(other.owner_) {}

GpuLease& GpuLease::operator=(GpuLease&& other) noexcept {
    if (this != &other) {
        if (manager_) {
            manager_->release();
        }
        manager_ = std::exchange(other.manager_, nullptr);
        owner_ = other.owner_;
    }
    return *this;
}

GpuLease::~GpuLease() {
    if (manager_) {
        manager_->release();
    }
}

GpuLease ResourceBudgetManager::acquireGpuLease(GpuLeaseOwner owner) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return !leaseHeld_; });
    leaseHeld_ = true;
    return GpuLease(this, owner);
}

void ResourceBudgetManager::release() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        leaseHeld_ = false;
    }
    cv_.notify_one();
}
