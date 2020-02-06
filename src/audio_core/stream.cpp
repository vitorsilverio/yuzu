// Copyright 2018 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cmath>

#include "audio_core/sink.h"
#include "audio_core/sink_details.h"
#include "audio_core/sink_stream.h"
#include "audio_core/stream.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/core_timing_util.h"
#include "core/perf_stats.h"
#include "core/settings.h"

namespace AudioCore {

constexpr std::size_t MaxAudioBufferCount{32};

u32 Stream::GetNumChannels() const {
    switch (format) {
    case Format::Mono16:
        return 1;
    case Format::Stereo16:
        return 2;
    case Format::Multi51Channel16:
        return 6;
    }
    UNIMPLEMENTED_MSG("Unimplemented format={}", static_cast<u32>(format));
    return {};
}

Stream::Stream(Core::Timing::CoreTiming& core_timing, u32 sample_rate, Format format,
               ReleaseCallback&& release_callback, SinkStream& sink_stream, std::string&& name_)
    : sample_rate{sample_rate}, format{format}, release_callback{std::move(release_callback)},
      sink_stream{sink_stream}, core_timing{core_timing}, name{std::move(name_)} {

    release_event = Core::Timing::CreateEvent(
        name, [this](u64 userdata, s64 cycles_late) { ReleaseActiveBuffer(); });
}

void Stream::Play() {
    state = State::Playing;
    PlayNextBuffer();
}

void Stream::Stop() {
    state = State::Stopped;
    UNIMPLEMENTED();
}

void Stream::SetVolume(float volume) {
    game_volume = volume;
}

Stream::State Stream::GetState() const {
    return state;
}

s64 Stream::GetBufferReleaseCycles(const Buffer& buffer) const {
    const std::size_t num_samples{buffer.GetSamples().size() / GetNumChannels()};
    const double time_scale{Settings::values.enable_realtime_audio
                                ? Core::System::GetInstance().GetPerfStats().GetLastFrameTimeScale()
                                : 1.0f};
    const auto us{std::chrono::microseconds(
        (static_cast<u64>(num_samples) * static_cast<u64>(1000000 / time_scale)) / sample_rate)};
    return Core::Timing::usToCycles(us);
}

static void VolumeAdjustSamples(std::vector<s16>& samples, float game_volume) {
    const float volume{std::clamp(Settings::values.volume - (1.0f - game_volume), 0.0f, 1.0f)};

    if (volume == 1.0f) {
        return;
    }

    // Implementation of a volume slider with a dynamic range of 60 dB
    const float volume_scale_factor = volume == 0 ? 0 : std::exp(6.90775f * volume) * 0.001f;
    for (auto& sample : samples) {
        sample = static_cast<s16>(sample * volume_scale_factor);
    }
}

void Stream::PlayNextBuffer() {
    if (!IsPlaying()) {
        // Ensure we are in playing state before playing the next buffer
        sink_stream.Flush();
        return;
    }

    if (active_buffer) {
        // Do not queue a new buffer if we are already playing a buffer
        return;
    }

    if (queued_buffers.empty()) {
        // No queued buffers - we are effectively paused
        sink_stream.Flush();
        return;
    }

    active_buffer = queued_buffers.front();
    queued_buffers.pop();

    VolumeAdjustSamples(active_buffer->GetSamples(), game_volume);

    sink_stream.EnqueueSamples(GetNumChannels(), active_buffer->GetSamples());

    core_timing.ScheduleEvent(GetBufferReleaseCycles(*active_buffer), release_event, {});
}

void Stream::ReleaseActiveBuffer() {
    ASSERT(active_buffer);
    released_buffers.push(std::move(active_buffer));
    release_callback();
    PlayNextBuffer();
}

bool Stream::QueueBuffer(BufferPtr&& buffer) {
    if (queued_buffers.size() < MaxAudioBufferCount) {
        queued_buffers.push(std::move(buffer));
        PlayNextBuffer();
        return true;
    }
    return false;
}

bool Stream::ContainsBuffer(Buffer::Tag tag) const {
    UNIMPLEMENTED();
    return {};
}

std::vector<Buffer::Tag> Stream::GetTagsAndReleaseBuffers(std::size_t max_count) {
    std::vector<Buffer::Tag> tags;
    for (std::size_t count = 0; count < max_count && !released_buffers.empty(); ++count) {
        tags.push_back(released_buffers.front()->GetTag());
        released_buffers.pop();
    }
    return tags;
}

} // namespace AudioCore
