#include <game/ai/route_import.hpp>

#include <content/formats/ai_route.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace mh::game {
namespace {

float original_squared_xz_distance(
    const mh::content::AiRouteSample &sample,
    const std::array<float, 2U> &vehicle_xz) {
  // The original keeps both products and their sum on the x87 stack, then
  // rounds once when storing the comparison value as float32.
  const auto dx = static_cast<long double>(vehicle_xz[0U]) -
                  static_cast<long double>(sample.position[0U]);
  const auto dz = static_cast<long double>(vehicle_xz[1U]) -
                  static_cast<long double>(sample.position[1U]);
  return static_cast<float>(dx * dx + dz * dz);
}

} // namespace

std::size_t original_ai_initial_lookahead_sample(
    const std::size_t current_sample, const std::size_t sample_count,
    const float speed_like, const float route_spacing_control) {
  if (sample_count == 0U || current_sample >= sample_count) {
    throw std::invalid_argument(
        "original AI lookahead requires an in-range current sample");
  }
  if (!std::isfinite(speed_like) || !std::isfinite(route_spacing_control) ||
      route_spacing_control == 0.0F) {
    throw std::invalid_argument(
        "original AI lookahead requires finite nonzero control inputs");
  }

  // The x87 path loads binary32 operands, keeps the intermediate extended,
  // then executes FRNDINT with RC=truncate before FISTP.
  const auto speed = static_cast<long double>(speed_like);
  const auto records = std::trunc(
      speed * speed * 0.5L /
      static_cast<long double>(route_spacing_control));
  if (records <
          static_cast<long double>(std::numeric_limits<std::int32_t>::min()) ||
      records >
          static_cast<long double>(std::numeric_limits<std::int32_t>::max())) {
    throw std::overflow_error("original AI lookahead displacement overflow");
  }

  const auto displacement = -static_cast<std::int64_t>(records);
  const auto count = static_cast<std::int64_t>(sample_count);
  auto wrapped =
      (static_cast<std::int64_t>(current_sample) + displacement) % count;
  if (wrapped < 0) {
    wrapped += count;
  }
  return static_cast<std::size_t>(wrapped);
}

std::size_t original_ai_nearest_route_sample(
    const mh::content::AiRouteData &route,
    const std::array<float, 2U> &vehicle_xz) {
  if (route.samples.empty()) {
    throw std::invalid_argument(
        "original AI nearest-route lookup requires samples");
  }
  auto best_distance = std::numeric_limits<float>::infinity();
  auto best_index = std::size_t{0U};
  for (std::size_t index = 0U; index < route.samples.size(); ++index) {
    const auto distance =
        original_squared_xz_distance(route.samples[index], vehicle_xz);
    if (distance < best_distance) {
      best_distance = distance;
      best_index = index;
    }
  }
  return best_index;
}

OriginalAiRouteCursor::OriginalAiRouteCursor(
    const mh::content::AiRouteData &route,
    const std::array<float, 2U> &vehicle_xz)
    : route_(&route) {
  if (route.samples.empty()) {
    throw std::invalid_argument("original AI route cursor requires samples");
  }
  reset(vehicle_xz);
}

void OriginalAiRouteCursor::reset(
    const std::array<float, 2U> &vehicle_xz) {
  const auto best_index =
      original_ai_nearest_route_sample(*route_, vehicle_xz);
  current_sample_ = best_index;
  lookahead_sample_ = best_index;
}

void OriginalAiRouteCursor::update_current(
    const std::array<float, 2U> &vehicle_xz) {
  auto current_distance = original_squared_xz_distance(
      route_->samples[current_sample_], vehicle_xz);

  // RVA 0x00017c59..0x00017cab scans toward increasing record addresses
  // first and wraps the trailing synthetic record to the first real sample.
  for (std::size_t scan = 0U; scan < route_->samples.size(); ++scan) {
    const auto following =
        current_sample_ + 1U == route_->samples.size()
            ? 0U
            : current_sample_ + 1U;
    const auto following_distance =
        original_squared_xz_distance(route_->samples[following], vehicle_xz);
    if (following_distance >= current_distance) {
      break;
    }
    const auto previous_current = current_sample_;
    current_sample_ = following;
    if (lookahead_sample_ == previous_current) {
      lookahead_sample_ = current_sample_;
    }
    current_distance = following_distance;
  }

  // RVA 0x0001696d..0x000169ca then performs the symmetric decreasing
  // scan and wraps the leading synthetic record to the last real sample.
  for (std::size_t scan = 0U; scan < route_->samples.size(); ++scan) {
    const auto preceding =
        current_sample_ == 0U ? route_->samples.size() - 1U
                              : current_sample_ - 1U;
    const auto preceding_distance =
        original_squared_xz_distance(route_->samples[preceding], vehicle_xz);
    if (preceding_distance >= current_distance) {
      return;
    }
    const auto previous_current = current_sample_;
    current_sample_ = preceding;
    if (lookahead_sample_ == previous_current) {
      lookahead_sample_ = current_sample_;
    }
    current_distance = preceding_distance;
  }
}

void OriginalAiRouteCursor::update_lookahead_seed(
    const float speed_like, const float route_spacing_control) {
  lookahead_sample_ = original_ai_initial_lookahead_sample(
      current_sample_, route_->samples.size(), speed_like,
      route_spacing_control);
}

void OriginalAiRouteCursor::select_lookahead_sample(const std::size_t sample) {
  if (sample >= route_->samples.size()) {
    throw std::out_of_range(
        "original AI lookahead selection is outside the route");
  }
  lookahead_sample_ = sample;
}

std::size_t OriginalAiRouteCursor::current_sample() const noexcept {
  return current_sample_;
}

std::size_t OriginalAiRouteCursor::lookahead_sample() const noexcept {
  return lookahead_sample_;
}

} // namespace mh::game
