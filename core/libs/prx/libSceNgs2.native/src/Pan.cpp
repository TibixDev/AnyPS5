#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <stdexcept>
#include <string>

#include "prx/libc/include/General.hpp"
#include "Ngs2Internal.hpp"

static constexpr std::uint32_t PAN_FORMAT_MONO = 1;
static constexpr std::uint32_t PAN_FORMAT_STEREO = 2;
static constexpr std::uint32_t PAN_FORMAT_5_1 = 6;
static constexpr std::uint32_t PAN_FORMAT_7_1 = 8;
static constexpr std::uint32_t LFE_CHANNEL = 3;
static constexpr std::uint32_t MAX_PAN_SPEAKERS = 7;

static bool HasLfe(std::uint32_t matrixFormat) {
    return matrixFormat == PAN_FORMAT_5_1 || matrixFormat == PAN_FORMAT_7_1;
}

static float WrapDegrees(float angle, float unitAngle) {
    float degrees = static_cast<float>(std::fmod(static_cast<double>(angle), unitAngle) * 360.0 / unitAngle);
    degrees = std::fmod(degrees + 180.0f, 360.0f);
    if (degrees < 0.0f) degrees += 360.0f;
    return degrees - 180.0f;
}

static void EqualPower(float* gains, std::uint32_t from, std::uint32_t to, float t) {
    gains[from] = std::cos(t * std::numbers::pi_v<float> / 2.0f);
    gains[to] = std::sin(t * std::numbers::pi_v<float> / 2.0f);
}

static void PanStereo(const float* degrees, float angle, float* gains) {
    if (angle > 90.0f) angle = 180.0f - angle;
    if (angle < -90.0f) angle = -180.0f - angle;
    const bool leftFirst = degrees[0] <= degrees[1];
    const auto left = leftFirst ? 0u : 1u;
    const auto right = leftFirst ? 1u : 0u;
    const float span = degrees[right] - degrees[left];
    const float t = span <= 0.0f ? 0.5f : std::clamp((angle - degrees[left]) / span, 0.0f, 1.0f);
    EqualPower(gains, left, right, t);
}

static void PanAround(const float* degrees, std::uint32_t numSpeakers, float angle, float* gains) {
    std::uint32_t order[MAX_PAN_SPEAKERS];
    for (std::uint32_t i = 0; i < numSpeakers; i++) order[i] = i;
    std::sort(order, order + numSpeakers, [&](std::uint32_t a, std::uint32_t b) { return degrees[a] < degrees[b]; });
    for (std::uint32_t i = 0; i < numSpeakers; i++) {
        const auto from = order[i];
        const auto to = order[(i + 1) % numSpeakers];
        float span = degrees[to] - degrees[from];
        if (span < 0.0f) span += 360.0f;
        if (span == 0.0f) continue;
        float offset = angle - degrees[from];
        if (offset < 0.0f) offset += 360.0f;
        if (offset <= span) {
            EqualPower(gains, from, to, offset / span);
            return;
        }
    }
    gains[order[0]] = 1.0f;
}

#pragma GCC visibility push(default)

extern "C" {

int APS5_VABI sceNgs2PanInit(Ngs2PanWork* work, const float* speaker_angles, float unit_angle, uint32_t num_speakers) {
    if (work == nullptr || num_speakers == 0 || num_speakers > MAX_PAN_SPEAKERS || !std::isfinite(unit_angle) || unit_angle <= 0.0f) APS5_INVALID_ARG_EX;
    if (speaker_angles == nullptr && num_speakers > 2) {
        throw std::runtime_error("NGS2: default speaker angles for " + std::to_string(num_speakers) + " speakers are not implemented");
    }
    if (speaker_angles != nullptr && !std::all_of(speaker_angles, speaker_angles + num_speakers, [](float angle) { return std::isfinite(angle); })) APS5_INVALID_ARG_EX;
    Ngs2PanWork initialized{};
    for (std::uint32_t i = 0; i < num_speakers; i++) {
        if (speaker_angles != nullptr) initialized.speaker_angles[i] = speaker_angles[i];
        else if (num_speakers == 2) initialized.speaker_angles[i] = (i == 0 ? -0.25f : 0.25f) * unit_angle;
    }
    initialized.unit_angle = unit_angle;
    initialized.num_speakers = num_speakers;
    *work = initialized;
    return SCE_NGS2_OK;
}

int APS5_VABI sceNgs2PanGetVolumeMatrix(Ngs2PanWork* work, const Ngs2PanParam* params, uint32_t num_params, uint32_t matrix_format, float* out_volume_matrix) {
    if (out_volume_matrix == nullptr) return SCE_NGS2_ERROR_INVALID_OUT_ADDRESS;
    if (num_params == 0) return SCE_NGS2_OK;
    if (work == nullptr || params == nullptr) APS5_INVALID_ARG_EX;
    const auto numSpeakers = work->num_speakers;
    if (numSpeakers == 0 || numSpeakers > MAX_PAN_SPEAKERS || !std::isfinite(work->unit_angle) || work->unit_angle <= 0.0f) APS5_INVALID_ARG_EX;
    if (!std::all_of(work->speaker_angles, work->speaker_angles + numSpeakers, [](float angle) { return std::isfinite(angle); })) APS5_INVALID_ARG_EX;
    if (matrix_format != PAN_FORMAT_MONO && matrix_format != PAN_FORMAT_STEREO && !HasLfe(matrix_format)) {
        throw std::runtime_error("NGS2: pan matrix format " + std::to_string(matrix_format) + " is not implemented");
    }
    const bool hasLfe = HasLfe(matrix_format);
    if (numSpeakers != (hasLfe ? matrix_format - 1 : matrix_format)) {
        throw std::runtime_error("NGS2: panning " + std::to_string(numSpeakers) + " speakers into matrix format " + std::to_string(matrix_format) + " is not implemented");
    }
    float degrees[MAX_PAN_SPEAKERS];
    for (std::uint32_t i = 0; i < numSpeakers; i++) degrees[i] = WrapDegrees(work->speaker_angles[i], work->unit_angle);
    for (std::uint32_t p = 0; p < num_params; p++) {
        const auto& param = params[p];
        if (!std::isfinite(param.angle) || !std::isfinite(param.distance) || !std::isfinite(param.fbw_level) || !std::isfinite(param.lfe_level)) APS5_INVALID_ARG_EX;
        if (param.distance != 1.0f) throw std::runtime_error("NGS2: pan distance other than 1 is not implemented");
        float gains[MAX_PAN_SPEAKERS] = {};
        const float angle = WrapDegrees(param.angle, work->unit_angle);
        if (numSpeakers == 1) gains[0] = 1.0f;
        else if (numSpeakers == 2) PanStereo(degrees, angle, gains);
        else PanAround(degrees, numSpeakers, angle, gains);
        float* row = out_volume_matrix + static_cast<std::size_t>(p) * matrix_format;
        for (std::uint32_t i = 0; i < numSpeakers; i++) {
            const auto channel = hasLfe && i >= LFE_CHANNEL ? i + 1 : i;
            row[channel] = gains[i] * param.fbw_level;
        }
        if (hasLfe) row[LFE_CHANNEL] = param.lfe_level;
    }
    return SCE_NGS2_OK;
}

}

#pragma GCC visibility pop
