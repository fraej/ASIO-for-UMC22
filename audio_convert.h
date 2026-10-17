#ifndef AUDIO_CONVERT_H
#define AUDIO_CONVERT_H
#include <emmintrin.h>
#include <stdint.h>

// Interleaved signed PCM to planar floats, four stereo frames per iteration.
inline void PcmToFloat(const int16_t* pcm, float* left, float* right, long frames) {
    const __m128 scale = _mm_set1_ps(1.0f / 32768.0f);
    long i = 0;
    for (; i + 4 <= frames; i += 4) {
        __m128i packed = _mm_loadu_si128(reinterpret_cast<const __m128i*>(pcm + i * 2));
        __m128i l = _mm_srai_epi32(_mm_slli_epi32(packed, 16), 16);
        __m128i r = _mm_srai_epi32(packed, 16);
        _mm_storeu_ps(left + i, _mm_mul_ps(_mm_cvtepi32_ps(l), scale));
        _mm_storeu_ps(right + i, _mm_mul_ps(_mm_cvtepi32_ps(r), scale));
    }
    for (; i < frames; ++i) {
        left[i] = pcm[i * 2] * (1.0f / 32768.0f);
        right[i] = pcm[i * 2 + 1] * (1.0f / 32768.0f);
    }
}

inline int16_t QuantizeSample(float value) {
    if (value != value) value = 0.0f; // A NaN from a plug-in becomes silence.
    if (value > 1.0f) value = 1.0f;
    if (value < -1.0f) value = -1.0f;
    int sample = _mm_cvtss_si32(_mm_set_ss(value * 32768.0f));
    return static_cast<int16_t>(sample > 32767 ? 32767 : sample);
}

inline void FloatToPcm(const float* left, const float* right, int16_t* pcm, long frames) {
    const __m128 min = _mm_set1_ps(-1.0f), max = _mm_set1_ps(1.0f);
    const __m128 scale = _mm_set1_ps(32768.0f);
    long i = 0;
    // Group two SSE2 batches so every packed lane is used: two packs instead
    // of four for eight stereo frames. The arithmetic and rounding are unchanged.
    if (left && right) {
        for (; i + 8 <= frames; i += 8) {
            __m128 l0 = _mm_loadu_ps(left + i), l1 = _mm_loadu_ps(left + i + 4);
            __m128 r0 = _mm_loadu_ps(right + i), r1 = _mm_loadu_ps(right + i + 4);
            l0 = _mm_and_ps(l0, _mm_cmpord_ps(l0, l0));
            l1 = _mm_and_ps(l1, _mm_cmpord_ps(l1, l1));
            r0 = _mm_and_ps(r0, _mm_cmpord_ps(r0, r0));
            r1 = _mm_and_ps(r1, _mm_cmpord_ps(r1, r1));
            l0 = _mm_mul_ps(_mm_min_ps(_mm_max_ps(l0, min), max), scale);
            l1 = _mm_mul_ps(_mm_min_ps(_mm_max_ps(l1, min), max), scale);
            r0 = _mm_mul_ps(_mm_min_ps(_mm_max_ps(r0, min), max), scale);
            r1 = _mm_mul_ps(_mm_min_ps(_mm_max_ps(r1, min), max), scale);
            __m128i l = _mm_packs_epi32(_mm_cvtps_epi32(l0), _mm_cvtps_epi32(l1));
            __m128i r = _mm_packs_epi32(_mm_cvtps_epi32(r0), _mm_cvtps_epi32(r1));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(pcm + i * 2), _mm_unpacklo_epi16(l, r));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(pcm + i * 2 + 8), _mm_unpackhi_epi16(l, r));
        }
    }
    for (; i + 4 <= frames; i += 4) {
        __m128 l = left ? _mm_loadu_ps(left + i) : _mm_setzero_ps();
        __m128 r = right ? _mm_loadu_ps(right + i) : _mm_setzero_ps();
        l = _mm_and_ps(l, _mm_cmpord_ps(l, l));
        r = _mm_and_ps(r, _mm_cmpord_ps(r, r));
        l = _mm_mul_ps(_mm_min_ps(_mm_max_ps(l, min), max), scale);
        r = _mm_mul_ps(_mm_min_ps(_mm_max_ps(r, min), max), scale);
        __m128i li = _mm_cvtps_epi32(l), ri = _mm_cvtps_epi32(r);
        __m128i ls = _mm_packs_epi32(li, li), rs = _mm_packs_epi32(ri, ri);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(pcm + i * 2), _mm_unpacklo_epi16(ls, rs));
    }
    for (; i < frames; ++i) {
        pcm[i * 2] = QuantizeSample(left ? left[i] : 0.0f);
        pcm[i * 2 + 1] = QuantizeSample(right ? right[i] : 0.0f);
    }
}
#endif
