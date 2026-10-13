#include "prx/libSceNgs2.native/include/Ngs2Types.hpp"
#include "prx/libc/include/General.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace {
using Vector = std::array<double, 3>;
constexpr double Pi = std::numbers::pi;
void Check(bool valid) { if (!valid) throw std::invalid_argument("NGS2: invalid source geometry parameters"); }
Vector Read(const Ngs2GeomVector& value) {
    Check(std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z));
    return {value.x, value.y, value.z};
}
double Dot(const Vector& a, const Vector& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
double Length(const Vector& v) { return std::hypot(v[0], v[1], v[2]); }
float Float(double value) {
    Check(std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max());
    return static_cast<float>(value);
}
void Validate(const Ngs2GeomListenerWork& listener, const Ngs2GeomSourceParam& source, unsigned flags) {
    Check(flags == 0 && listener.coordinate <= 1);
    for (const auto& row : listener.matrix) for (float value : row) Check(std::isfinite(value));
    Check(listener.matrix[3][0] == 0 && listener.matrix[3][1] == 0 && listener.matrix[3][2] == 0 && listener.matrix[3][3] == 1);
    for (unsigned a = 0; a < 3; ++a) for (unsigned b = 0; b < 3; ++b) {
        double dot = 0;
        for (unsigned i = 0; i < 3; ++i) dot += double(listener.matrix[a][i]) * listener.matrix[b][i];
        Check(std::abs(dot - (a == b ? 1.0 : 0.0)) < 0.001);
    }
    Check(std::isfinite(listener.sound_speed) && listener.sound_speed > 0);
    for (float value : {source.cone.inner_level, source.cone.outer_level, source.cone.inner_angle, source.cone.outer_angle,
                       source.rolloff.max_distance, source.rolloff.reference_distance, source.rolloff.rolloff_factor,
                       source.doppler_factor, source.fbw_level, source.lfe_level, source.max_level, source.min_level, source.radius})
        Check(std::isfinite(value) && value >= 0);
    Check(source.cone.inner_angle <= source.cone.outer_angle && source.cone.outer_angle <= 360);
    Check(source.rolloff.model <= 2 && source.rolloff.reference_distance > 0 && source.rolloff.max_distance >= source.rolloff.reference_distance);
    Check(source.min_level <= source.max_level);
    Check(source.num_speakers == 1 || source.num_speakers == 2 || source.num_speakers == 6 || source.num_speakers == 8);
    Check(source.matrix_format >= 1 && source.matrix_format <= 8);
}
double Attenuation(const Ngs2GeomSourceParam& source, double distance, const Vector& towardListener) {
    const auto& r = source.rolloff;
    const double d = std::clamp(distance, double(r.reference_distance), double(r.max_distance));
    double gain;
    if (r.model == 0) gain = r.reference_distance / (r.reference_distance + r.rolloff_factor * (d - r.reference_distance));
    else if (r.model == 1) gain = r.max_distance == r.reference_distance ? 1 : 1 - r.rolloff_factor * (d - r.reference_distance) / (r.max_distance - r.reference_distance);
    else gain = std::pow(d / r.reference_distance, -double(r.rolloff_factor));
    const Vector direction = Read(source.direction);
    const double length = Length(direction);
    double cone = source.cone.inner_level;
    if (distance > 0 && source.cone.inner_angle < 360) {
        Check(length > 0);
        const double angle = 2 * std::acos(std::clamp(Dot(direction, towardListener) / length, -1.0, 1.0)) * 180 / Pi;
        if (angle >= source.cone.outer_angle) cone = source.cone.outer_level;
        else if (angle > source.cone.inner_angle) {
            const double weight = (angle - source.cone.inner_angle) / (source.cone.outer_angle - source.cone.inner_angle);
            cone += (source.cone.outer_level - source.cone.inner_level) * weight;
        }
    }
    return std::clamp(std::max(gain, 0.0) * cone, double(source.min_level), double(source.max_level));
}
std::array<double, 8> Pan(const Vector& local, double radius, unsigned speakers) {
    std::array<double, 8> levels{};
    if (speakers == 1) { levels[0] = 1; return levels; }
    const double distance = Length(local), horizontal = std::hypot(local[0], local[2]);
    if (distance <= radius || horizontal <= distance * 1e-12) {
        const double gain = 1 / std::sqrt(double(speakers >= 6 ? speakers - 1 : speakers));
        for (unsigned channel = 0; channel < speakers; ++channel) if (!(speakers >= 6 && channel == 3)) levels[channel] = gain;
        return levels;
    }
    std::vector<std::pair<double, unsigned>> ring;
    if (speakers == 2) ring = {{-90, 0}, {90, 1}};
    else if (speakers == 6) ring = {{-110, 4}, {-30, 0}, {0, 2}, {30, 1}, {110, 5}};
    else ring = {{-150, 4}, {-90, 6}, {-30, 0}, {0, 2}, {30, 1}, {90, 7}, {150, 5}};
    double angle = std::atan2(local[0], local[2]) * 180 / Pi;
    if (angle < ring.front().first) angle += 360;
    for (std::size_t i = 0; i < ring.size(); ++i) {
        const auto next = (i + 1) % ring.size();
        const double end = ring[next].first + (next == 0 ? 360 : 0);
        if (angle < ring[i].first || angle > end) continue;
        const double blend = (angle - ring[i].first) / (end - ring[i].first) * Pi / 2;
        levels[ring[i].second] = std::cos(blend);
        levels[ring[next].second] = std::sin(blend);
        break;
    }
    return levels;
}
}

int Ngs2GeomApply(const Ngs2GeomListenerWork* listener, const Ngs2GeomSourceParam* source,
                                         Ngs2GeomAttribute* output, std::uint32_t flags) {
    if (!listener || !source) throw std::invalid_argument("NGS2: missing geometry input");
    if (!output) return SCE_NGS2_ERROR_INVALID_OUT_ADDRESS;
    Validate(*listener, *source, flags);
    const Vector position = Read(source->position), sourceVelocity = Read(source->velocity), listenerVelocity = Read(listener->velocity);
    Vector local{}, relative{}, towardListener{};
    for (unsigned axis = 0; axis < 3; ++axis) {
        local[axis] = listener->matrix[axis][3];
        for (unsigned column = 0; column < 3; ++column) local[axis] += listener->matrix[axis][column] * position[column];
    }
    for (unsigned axis = 0; axis < 3; ++axis)
        for (unsigned row = 0; row < 3; ++row) relative[axis] += listener->matrix[row][axis] * local[row];
    const double distance = Length(relative);
    if (distance > 0) for (unsigned axis = 0; axis < 3; ++axis) towardListener[axis] = -relative[axis] / distance;
    const double gain = Attenuation(*source, distance, towardListener);
    const double speed = listener->sound_speed, limit = speed * 0.99;
    const double sourceSpeed = std::clamp(source->doppler_factor * Dot(sourceVelocity, towardListener), -limit, limit);
    const double listenerSpeed = std::clamp(source->doppler_factor * Dot(listenerVelocity, towardListener), -limit, limit);
    Ngs2GeomAttribute result{};
    result.pitch_ratio = Float((speed - listenerSpeed) / (speed - sourceSpeed));
    result.a3d_attrib.position = {Float(local[0]), Float(local[1]), Float(local[2])};
    result.a3d_attrib.volume = Float(gain * source->fbw_level);
    const auto levels = Pan(local, source->radius, source->num_speakers);
    for (unsigned row = 0; row < source->matrix_format; ++row) {
        for (unsigned channel = 0; channel < source->num_speakers; ++channel)
            result.level[row * 8 + channel] = Float(gain * (channel == 3 && source->num_speakers >= 6 ? source->lfe_level : source->fbw_level * levels[channel]));
    }
    *output = result;
    return SCE_NGS2_OK;
}
