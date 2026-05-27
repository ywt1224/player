#pragma once

#include <cstdint>

class VadDetector {
public:
    VadDetector();

    // Initialize with sample rate and frame size
    bool Init(int sample_rate, int frame_samples);

    // Detect whether a PCM S16 frame contains speech.
    // Returns true if speech, false if silence.
    bool IsSpeech(const int16_t* pcm, int samples);

    // Accumulate silence duration. Caller checks if >= threshold for sentence break.
    int  silence_ms() const { return silence_ms_; }
    void reset_silence()     { silence_ms_ = 0; }
    void add_silence(int ms) { silence_ms_ += ms; }

    static constexpr int kDefaultSilenceThreshold = 500;

private:
    double ThresholdForRate() const;

    int sample_rate_   = 16000;
    int frame_samples_ = 320;  // 20ms at 16kHz
    int silence_ms_    = 0;
};
