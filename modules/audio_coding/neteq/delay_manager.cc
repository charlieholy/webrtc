/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/audio_coding/neteq/delay_manager.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <memory>
#include <numeric>
#include <string>

#include "modules/audio_coding/neteq/histogram.h"
#include "modules/include/module_common_types_public.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "rtc_base/numerics/safe_conversions.h"
#include "rtc_base/numerics/safe_minmax.h"
#include "system_wrappers/include/field_trial.h"

namespace webrtc {
namespace {

constexpr int kMinBaseMinimumDelayMs = 0;
constexpr int kMaxBaseMinimumDelayMs = 10000;
constexpr int kMaxHistoryMs = 2000;  // Oldest packet to include in history to
                                     // calculate relative packet arrival delay.
constexpr int kDelayBuckets = 100;
constexpr int kBucketSizeMs = 20;
constexpr int kStartDelayMs = 80;
constexpr int kMaxNumReorderedPackets = 5;

int PercentileToQuantile(double percentile) {
  return static_cast<int>((1 << 30) * percentile / 100.0 + 0.5);
}

struct DelayHistogramConfig {
  int quantile = 1041529569;  // 0.97 in Q30.
  int forget_factor = 32745;  // 0.9993 in Q15.
  absl::optional<double> start_forget_weight = 2;
};

// TODO(jakobi): Remove legacy field trial.
DelayHistogramConfig GetDelayHistogramConfig() {
  constexpr char kDelayHistogramFieldTrial[] =
      "WebRTC-Audio-NetEqDelayHistogram";
  DelayHistogramConfig config;
  if (webrtc::field_trial::IsEnabled(kDelayHistogramFieldTrial)) {
    const auto field_trial_string =
        webrtc::field_trial::FindFullName(kDelayHistogramFieldTrial);
    double percentile = -1.0;
    double forget_factor = -1.0;
    double start_forget_weight = -1.0;
    if (sscanf(field_trial_string.c_str(), "Enabled-%lf-%lf-%lf", &percentile,
               &forget_factor, &start_forget_weight) >= 2 &&
        percentile >= 0.0 && percentile <= 100.0 && forget_factor >= 0.0 &&
        forget_factor <= 1.0) {
      config.quantile = PercentileToQuantile(percentile);
      config.forget_factor = (1 << 15) * forget_factor;
      config.start_forget_weight =
          start_forget_weight >= 1 ? absl::make_optional(start_forget_weight)
                                   : absl::nullopt;
    }
  }
  RTC_LOG(LS_INFO) << "Delay histogram config:"
                      " quantile="
                   << config.quantile
                   << " forget_factor=" << config.forget_factor
                   << " start_forget_weight="
                   << config.start_forget_weight.value_or(0);
  return config;
}

}  // namespace

DelayManager::DelayManager(int max_packets_in_buffer,
                           int base_minimum_delay_ms,
                           int histogram_quantile,
                           const TickTimer* tick_timer,
                           std::unique_ptr<Histogram> histogram)
    : first_packet_received_(false),
      max_packets_in_buffer_(max_packets_in_buffer),
      histogram_(std::move(histogram)),
      histogram_quantile_(histogram_quantile),
      tick_timer_(tick_timer),
      base_minimum_delay_ms_(base_minimum_delay_ms),
      effective_minimum_delay_ms_(base_minimum_delay_ms),
      minimum_delay_ms_(0),
      maximum_delay_ms_(0),
      target_level_ms_(kStartDelayMs),
      last_timestamp_(0) {
  RTC_CHECK(histogram_);
  RTC_DCHECK_GE(base_minimum_delay_ms_, 0);

  Reset();
}

std::unique_ptr<DelayManager> DelayManager::Create(
    int max_packets_in_buffer,
    int base_minimum_delay_ms,
    const TickTimer* tick_timer) {
  auto config = GetDelayHistogramConfig();
  std::unique_ptr<Histogram> histogram = std::make_unique<Histogram>(
      kDelayBuckets, config.forget_factor, config.start_forget_weight);
  return std::make_unique<DelayManager>(max_packets_in_buffer,
                                        base_minimum_delay_ms, config.quantile,
                                        tick_timer, std::move(histogram));
}

DelayManager::~DelayManager() {}

absl::optional<int> DelayManager::Update(uint32_t timestamp,
                                         int sample_rate_hz,
                                         bool reset) {
  if (sample_rate_hz <= 0) {
    return absl::nullopt;
  }

  if (!first_packet_received_ || reset) {
    // Restart relative delay esimation from this packet.
    delay_history_.clear();
    packet_iat_stopwatch_ = tick_timer_->GetNewStopwatch();
    last_timestamp_ = timestamp;
    first_packet_received_ = true;
    num_reordered_packets_ = 0;
    return absl::nullopt;
  }

  const int expected_iat_ms =
      1000 * static_cast<int32_t>(timestamp - last_timestamp_) / sample_rate_hz;
  const int iat_ms = packet_iat_stopwatch_->ElapsedMs();
  const int iat_delay_ms = iat_ms - expected_iat_ms;
  absl::optional<int> relative_delay;
  bool reordered = !IsNewerTimestamp(timestamp, last_timestamp_);
  if (reordered) {
    relative_delay = std::max(iat_delay_ms, 0);
  } else {
    UpdateDelayHistory(iat_delay_ms, timestamp, sample_rate_hz);
    relative_delay = CalculateRelativePacketArrivalDelay();
  }
  const int index = relative_delay.value() / kBucketSizeMs;
  if (index < histogram_->NumBuckets()) {
    // Maximum delay to register is 2000 ms.
    histogram_->Add(index);
  }
  // Calculate new |target_level_ms_| based on updated statistics.
  int bucket_index = histogram_->Quantile(histogram_quantile_);
  target_level_ms_ = (1 + bucket_index) * kBucketSizeMs;
  target_level_ms_ = std::max(target_level_ms_, effective_minimum_delay_ms_);
  if (maximum_delay_ms_ > 0) {
    target_level_ms_ = std::min(target_level_ms_, maximum_delay_ms_);
  }
  if (packet_len_ms_ > 0) {
    // Target level should be at least one packet.
    target_level_ms_ = std::max(target_level_ms_, packet_len_ms_);
    // Limit to 75% of maximum buffer size.
    target_level_ms_ = std::min(
        target_level_ms_, 3 * max_packets_in_buffer_ * packet_len_ms_ / 4);
  }

  // Prepare for next packet arrival.
  if (reordered) {
    // Allow a small number of reordered packets before resetting the delay
    // estimation.
    if (num_reordered_packets_ < kMaxNumReorderedPackets) {
      ++num_reordered_packets_;
      return relative_delay;
    }
    delay_history_.clear();
  }
  num_reordered_packets_ = 0;
  packet_iat_stopwatch_ = tick_timer_->GetNewStopwatch();
  last_timestamp_ = timestamp;
  return relative_delay;
}

void DelayManager::UpdateDelayHistory(int iat_delay_ms,
                                      uint32_t timestamp,
                                      int sample_rate_hz) {
  PacketDelay delay;
  delay.iat_delay_ms = iat_delay_ms;
  delay.timestamp = timestamp;
  delay_history_.push_back(delay);
  while (timestamp - delay_history_.front().timestamp >
         static_cast<uint32_t>(kMaxHistoryMs * sample_rate_hz / 1000)) {
    delay_history_.pop_front();
  }
}

int DelayManager::CalculateRelativePacketArrivalDelay() const {
  // This effectively calculates arrival delay of a packet relative to the
  // packet preceding the history window. If the arrival delay ever becomes
  // smaller than zero, it means the reference packet is invalid, and we
  // move the reference.
  int relative_delay = 0;
  for (const PacketDelay& delay : delay_history_) {
    relative_delay += delay.iat_delay_ms;
    relative_delay = std::max(relative_delay, 0);
  }
  return relative_delay;
}

int DelayManager::SetPacketAudioLength(int length_ms) {
  if (length_ms <= 0) {
    RTC_LOG_F(LS_ERROR) << "length_ms = " << length_ms;
    return -1;
  }
  packet_len_ms_ = length_ms;
  return 0;
}

void DelayManager::Reset() {
  packet_len_ms_ = 0;
  histogram_->Reset();
  delay_history_.clear();
  target_level_ms_ = kStartDelayMs;
  packet_iat_stopwatch_ = tick_timer_->GetNewStopwatch();
  first_packet_received_ = false;
  num_reordered_packets_ = 0;
}

int DelayManager::TargetDelayMs() const {
  return target_level_ms_;
}

bool DelayManager::IsValidMinimumDelay(int delay_ms) const {
  return 0 <= delay_ms && delay_ms <= MinimumDelayUpperBound();
}

bool DelayManager::IsValidBaseMinimumDelay(int delay_ms) const {
  return kMinBaseMinimumDelayMs <= delay_ms &&
         delay_ms <= kMaxBaseMinimumDelayMs;
}

bool DelayManager::SetMinimumDelay(int delay_ms) {
  if (!IsValidMinimumDelay(delay_ms)) {
    return false;
  }

  minimum_delay_ms_ = delay_ms;
  UpdateEffectiveMinimumDelay();
  return true;
}

bool DelayManager::SetMaximumDelay(int delay_ms) {
  // If |delay_ms| is zero then it unsets the maximum delay and target level is
  // unconstrained by maximum delay.
  if (delay_ms != 0 &&
      (delay_ms < minimum_delay_ms_ || delay_ms < packet_len_ms_)) {
    // Maximum delay shouldn't be less than minimum delay or less than a packet.
    return false;
  }

  maximum_delay_ms_ = delay_ms;
  UpdateEffectiveMinimumDelay();
  return true;
}

bool DelayManager::SetBaseMinimumDelay(int delay_ms) {
  if (!IsValidBaseMinimumDelay(delay_ms)) {
    return false;
  }

  base_minimum_delay_ms_ = delay_ms;
  UpdateEffectiveMinimumDelay();
  return true;
}

int DelayManager::GetBaseMinimumDelay() const {
  return base_minimum_delay_ms_;
}

void DelayManager::UpdateEffectiveMinimumDelay() {
  // Clamp |base_minimum_delay_ms_| into the range which can be effectively
  // used.
  const int base_minimum_delay_ms =
      rtc::SafeClamp(base_minimum_delay_ms_, 0, MinimumDelayUpperBound());
  effective_minimum_delay_ms_ =
      std::max(minimum_delay_ms_, base_minimum_delay_ms);
}

int DelayManager::MinimumDelayUpperBound() const {
  // Choose the lowest possible bound discarding 0 cases which mean the value
  // is not set and unconstrained.
  int q75 = max_packets_in_buffer_ * packet_len_ms_ * 3 / 4;
  q75 = q75 > 0 ? q75 : kMaxBaseMinimumDelayMs;
  const int maximum_delay_ms =
      maximum_delay_ms_ > 0 ? maximum_delay_ms_ : kMaxBaseMinimumDelayMs;
  return std::min(maximum_delay_ms, q75);
}

}  // namespace webrtc
