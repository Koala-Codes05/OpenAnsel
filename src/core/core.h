#pragma once

#include <cstdint>
#include <string>
#include <atomic>
#include <mutex>

namespace OpenAnsel {

enum class CaptureState {
    Idle,
    Requested,
    Capturing,
    Processing,
    Error
};

struct CaptureConfig {
    uint32_t tileCountX = 2;
    uint32_t tileCountY = 2;
    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;
    std::string outputPath = "./OpenAnsel_Capture";
    bool pauseGame = false;
    bool disableHud = false;
    bool disableTaa = false;
    bool disableMotionBlur = false;
};

class Core {
public:
    static Core& Instance();

    CaptureState GetState() const;
    void RequestCapture(const CaptureConfig& config);
    CaptureConfig GetNextConfig();
    void SetState(CaptureState state);
    bool IsCaptureRequested() const;
    void ClearRequest();

private:
    Core() = default;

    std::atomic<CaptureState> state_{CaptureState::Idle};
    CaptureConfig pendingConfig_;
    std::mutex configMutex_;
};

} // namespace OpenAnsel
