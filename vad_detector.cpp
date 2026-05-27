#include "vad_detector.h"
#include <cmath>
#include <iostream>

VadDetector::VadDetector() = default;

bool VadDetector::Init(int sample_rate, int frame_samples) {
    sample_rate_   = sample_rate;
    frame_samples_ = frame_samples;
    std::cout << "[VadDetector] Init: rate=" << sample_rate_
              << " frame=" << frame_samples << " samples"
              << " threshold=" << ThresholdForRate() << "\n";
    return true;
}

bool VadDetector::IsSpeech(const int16_t* pcm, int samples) {
    // Energy-based detection: RMS of the frame vs adaptive threshold
    double sum = 0.0;
    for (int i = 0; i < samples; i++) {
        sum += static_cast<double>(pcm[i]) * static_cast<double>(pcm[i]);
    }
    double rms = std::sqrt(sum / samples);
    return rms > ThresholdForRate();
}

double VadDetector::ThresholdForRate() const {
    // Silence RMS sits around 20-100 for 16-bit PCM.
    // Speech typically exceeds 500-800. Use 400 as default, scale for rate.
    (void)sample_rate_;
    return static_cast<double>(kDefaultSilenceThreshold);
}
