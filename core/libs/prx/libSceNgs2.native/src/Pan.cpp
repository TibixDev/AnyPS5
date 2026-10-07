#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <stdexcept>
#include <string>

#include "prx/libc/include/General.hpp"
#include "prx/libSceNgs2.native/include/Ngs2Types.hpp"

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

extern "C" {

int APS5_VABI sceNgs2PanInit(Ngs2PanWork* work, const float* speakerAngles, float unitAngle, uint32_t numSpeakers) {
    if (work == nullptr || numSpeakers == 0 || numSpeakers > MAX_PAN_SPEAKERS || !std::isfinite(unitAngle) || unitAngle <= 0.0f) APS5_INVALID_ARG_EX;
    if (speakerAngles == nullptr && numSpeakers > 2) {
        throw std::runtime_error("NGS2: default speaker angles for " + std::to_string(numSpeakers) + " speakers are not implemented");
    }
    if (speakerAngles != nullptr && !std::all_of(speakerAngles, speakerAngles + numSpeakers, [](float angle) { return std::isfinite(angle); })) APS5_INVALID_ARG_EX;
    Ngs2PanWork initialized{};
    for (std::uint32_t i = 0; i < numSpeakers; i++) {
        if (speakerAngles != nullptr) initialized.speaker_angles[i] = speakerAngles[i];
        else if (numSpeakers == 2) initialized.speaker_angles[i] = (i == 0 ? -0.25f : 0.25f) * unitAngle;
    }
    initialized.unit_angle = unitAngle;
    initialized.num_speakers = numSpeakers;
    *work = initialized;
    return SCE_NGS2_OK;
}

int APS5_VABI sceNgs2PanGetVolumeMatrix(Ngs2PanWork* work, const Ngs2PanParam* params, uint32_t numParams, uint32_t matrixFormat, float* outVolumeMatrix) {
    if (outVolumeMatrix == nullptr) return SCE_NGS2_ERROR_INVALID_OUT_ADDRESS;
    if (numParams == 0) return SCE_NGS2_OK;
    if (work == nullptr || params == nullptr) APS5_INVALID_ARG_EX;
    const auto numSpeakers = work->num_speakers;
    if (numSpeakers == 0 || numSpeakers > MAX_PAN_SPEAKERS || !std::isfinite(work->unit_angle) || work->unit_angle <= 0.0f) APS5_INVALID_ARG_EX;
    if (!std::all_of(work->speaker_angles, work->speaker_angles + numSpeakers, [](float angle) { return std::isfinite(angle); })) APS5_INVALID_ARG_EX;
    if (matrixFormat != PAN_FORMAT_MONO && matrixFormat != PAN_FORMAT_STEREO && !HasLfe(matrixFormat)) {
        throw std::runtime_error("NGS2: pan matrix format " + std::to_string(matrixFormat) + " is not implemented");
    }
    const bool hasLfe = HasLfe(matrixFormat);
    if (numSpeakers != (hasLfe ? matrixFormat - 1 : matrixFormat)) {
        throw std::runtime_error("NGS2: panning " + std::to_string(numSpeakers) + " speakers into matrix format " + std::to_string(matrixFormat) + " is not implemented");
    }
    float degrees[MAX_PAN_SPEAKERS];
    for (std::uint32_t i = 0; i < numSpeakers; i++) degrees[i] = WrapDegrees(work->speaker_angles[i], work->unit_angle);
    for (std::uint32_t p = 0; p < numParams; p++) {
        const auto& param = params[p];
        if (!std::isfinite(param.angle) || !std::isfinite(param.distance) || !std::isfinite(param.fbw_level) || !std::isfinite(param.lfe_level)) APS5_INVALID_ARG_EX;
        if (param.distance < 0.0f || param.distance > 1.0f) throw std::runtime_error("NGS2: pan distances outside the unit circle are not implemented");
        float gains[MAX_PAN_SPEAKERS] = {};
        const float angle = WrapDegrees(param.angle, work->unit_angle);
        if (numSpeakers == 1) gains[0] = 1.0f;
        else if (numSpeakers == 2) PanStereo(degrees, angle, gains);
        else PanAround(degrees, numSpeakers, angle, gains);
        if (param.distance != 1.0f) {
            const float diffusePower = (1.0f - param.distance) / numSpeakers;
            for (std::uint32_t i = 0; i < numSpeakers; ++i)
                gains[i] = std::sqrt(diffusePower + param.distance * gains[i] * gains[i]);
        }
        float* row = outVolumeMatrix + static_cast<std::size_t>(p) * matrixFormat;
        for (std::uint32_t i = 0; i < numSpeakers; i++) {
            const auto channel = hasLfe && i >= LFE_CHANNEL ? i + 1 : i;
            row[channel] = gains[i] * param.fbw_level;
        }
        if (hasLfe) row[LFE_CHANNEL] = param.lfe_level;
    }
    return SCE_NGS2_OK;
}

}
