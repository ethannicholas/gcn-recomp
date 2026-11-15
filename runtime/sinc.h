// Windowed-sinc interpolation, for resampling a stream to another rate: the AI DMA's
// 32 kHz to the device's 48 kHz (audio.cpp), and the AX voices' enhanced interpolation
// (hw/ax.cpp). Portable and allocation-free once made.
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace sinc {

constexpr double kPi = 3.14159265358979323846;

// The zeroth-order modified Bessel function of the first kind, for the Kaiser window.
inline double bessel_i0(double x) {
    double sum = 1, term = 1;
    for (int k = 1; k < 50; k++) {
        term *= (x / (2 * k)) * (x / (2 * k));
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

// A kernel for interpolating a stream between its samples: `taps` of them, oldest first,
// at a fraction f of the way from sample taps/2 - 1 to sample taps/2. It is a sinc cutting
// at `cutoff` times the input's Nyquist frequency (-6 dB there) under a Kaiser window of
// shape `beta`, tabulated at `phases` fractions and interpolated linearly between them,
// each phase scaled to unity gain.
class Kernel {
public:
    Kernel(int taps, int phases, double cutoff, double beta) : taps_(taps), phases_(phases), k_((size_t)(phases + 1) * taps) {
        const double i0b = bessel_i0(beta), half = taps / 2.0;
        for (int p = 0; p <= phases; p++) {
            float* k = &k_[(size_t)p * taps];
            double sum = 0;
            for (int j = 0; j < taps; j++) {
                const double d = j - (taps / 2 - 1) - (double)p / phases;  // from the point, in samples
                const double x = kPi * cutoff * d;
                const double s = d == 0 ? 1 : std::sin(x) / x;
                const double r = d / half;
                const double w = r * r < 1 ? bessel_i0(beta * std::sqrt(1 - r * r)) / i0b : 0;
                k[j] = (float)(s * w);
                sum += s * w;
            }
            for (int j = 0; j < taps; j++) k[j] = (float)(k[j] / sum);
        }
    }
    int taps() const { return taps_; }

    // The stream x[0..taps-1] at fraction f (0 <= f < 1), one channel or two.
    template <typename T>
    float apply(const T* x, double f) const {
        const float* k0;
        float a;
        locate(f, k0, a);
        const float* k1 = k0 + taps_;
        float s0 = 0, s1 = 0;
        for (int j = 0; j < taps_; j++) {
            s0 += k0[j] * (float)x[j];
            s1 += k1[j] * (float)x[j];
        }
        return s0 + (s1 - s0) * a;
    }
    void apply2(const float* x, const float* y, double f, float& ox, float& oy) const {
        const float* k0;
        float a;
        locate(f, k0, a);
        const float* k1 = k0 + taps_;
        float x0 = 0, x1 = 0, y0 = 0, y1 = 0;
        for (int j = 0; j < taps_; j++) {
            x0 += k0[j] * x[j];
            x1 += k1[j] * x[j];
            y0 += k0[j] * y[j];
            y1 += k1[j] * y[j];
        }
        ox = x0 + (x1 - x0) * a;
        oy = y0 + (y1 - y0) * a;
    }

private:
    void locate(double f, const float*& k0, float& a) const {
        const double p = std::clamp(f, 0.0, 1.0) * phases_;
        const int i = std::min((int)p, phases_ - 1);
        a = (float)(p - i);
        k0 = &k_[(size_t)i * taps_];
    }
    int taps_, phases_;
    std::vector<float> k_;  // (phases + 1) * taps: the last phase is the first moved on a sample
};

// The newest `taps` samples of a stereo stream, in order, for Kernel::apply2: a doubled
// ring, so the window is always contiguous.
class StereoHistory {
public:
    explicit StereoHistory(int taps) : taps_(taps), l_(2 * taps), r_(2 * taps) {}
    void clear() {
        std::fill(l_.begin(), l_.end(), 0.0f);
        std::fill(r_.begin(), r_.end(), 0.0f);
    }
    void push(float l, float r) {
        l_[pos_] = l_[pos_ + taps_] = l;
        r_[pos_] = r_[pos_ + taps_] = r;
        pos_ = pos_ + 1 == taps_ ? 0 : pos_ + 1;
    }
    const float* left() const { return &l_[pos_]; }
    const float* right() const { return &r_[pos_]; }

private:
    int taps_, pos_ = 0;
    std::vector<float> l_, r_;
};

}  // namespace sinc
