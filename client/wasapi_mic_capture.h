#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

namespace audiosub {

    class WasapiMicCapture {
    public:
        using PcmCallback = std::function<void(const int16_t* samples,
            size_t sample_count,
            int sample_rate,
            int channels)>;

        WasapiMicCapture();
        ~WasapiMicCapture();

        bool Start(PcmCallback callback);
        void Stop();

    private:
        void CaptureThreadMain();

        std::atomic<bool> running_{ false };
        std::thread thread_;
        PcmCallback callback_;
    };

}  // namespace audiosub