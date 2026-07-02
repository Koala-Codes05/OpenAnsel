#include "core.h"

namespace OpenAnsel {

Core& Core::Instance() {
    static Core instance;
    return instance;
}

CaptureState Core::GetState() const {
    return state_.load();
}

void Core::RequestCapture(const CaptureConfig& config) {
    std::lock_guard<std::mutex> lock(configMutex_);
    pendingConfig_ = config;
    state_.store(CaptureState::Requested);
}

CaptureConfig Core::GetNextConfig() {
    std::lock_guard<std::mutex> lock(configMutex_);
    return pendingConfig_;
}

void Core::SetState(CaptureState state) {
    state_.store(state);
}

bool Core::IsCaptureRequested() const {
    return state_.load() == CaptureState::Requested;
}

void Core::ClearRequest() {
    state_.store(CaptureState::Idle);
}

} // namespace OpenAnsel
