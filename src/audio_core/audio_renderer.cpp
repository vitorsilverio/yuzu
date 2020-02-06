// Copyright 2018 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "audio_core/algorithm/interpolate.h"
#include "audio_core/audio_out.h"
#include "audio_core/audio_renderer.h"
#include "audio_core/codec.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "core/core.h"
#include "core/hle/kernel/writable_event.h"
#include "core/memory.h"

namespace AudioCore {

constexpr u32 STREAM_SAMPLE_RATE{48000};
constexpr u32 STREAM_NUM_CHANNELS{2};

class AudioRenderer::VoiceState {
public:
    bool IsPlaying() const {
        return is_in_use && info.play_state == PlayState::Started;
    }

    const VoiceOutStatus& GetOutStatus() const {
        return out_status;
    }

    const VoiceInfo& GetInfo() const {
        return info;
    }

    VoiceInfo& GetInfo() {
        return info;
    }

    void SetWaveIndex(std::size_t index);
    std::vector<s16> DequeueSamples(std::size_t sample_count, Memory::Memory& memory);
    void UpdateState();
    void RefreshBuffer(Memory::Memory& memory);

private:
    bool is_in_use{};
    bool is_refresh_pending{};
    std::size_t wave_index{};
    std::size_t offset{};
    Codec::ADPCMState adpcm_state{};
    InterpolationState interp_state{};
    std::vector<s16> samples;
    VoiceOutStatus out_status{};
    VoiceInfo info{};
};

class AudioRenderer::EffectState {
public:
    const EffectOutStatus& GetOutStatus() const {
        return out_status;
    }

    const EffectInStatus& GetInfo() const {
        return info;
    }

    EffectInStatus& GetInfo() {
        return info;
    }

    void UpdateState(Memory::Memory& memory);

private:
    EffectOutStatus out_status{};
    EffectInStatus info{};
};

class AudioRenderer::ChannelState {
public:
    const ChannelInfoIn& GetInfo() const {
        return info;
    }

    ChannelInfoIn& GetInfo() {
        return info;
    }

private:
    ChannelInfoIn info{};
};

AudioRenderer::AudioRenderer(Core::Timing::CoreTiming& core_timing, Memory::Memory& memory_,
                             AudioRendererParameter params,
                             std::shared_ptr<Kernel::WritableEvent> buffer_event,
                             std::size_t instance_number)
    : worker_params{params}, buffer_event{buffer_event}, voices(params.voice_count),
      effects(params.effect_count), memory{memory_} {

    audio_out = std::make_unique<AudioCore::AudioOut>();
    stream = audio_out->OpenStream(core_timing, STREAM_SAMPLE_RATE, STREAM_NUM_CHANNELS,
                                   fmt::format("AudioRenderer-Instance{}", instance_number),
                                   [=]() { buffer_event->Signal(); });
    audio_out->StartStream(stream);

    QueueMixedBuffer(0);
    QueueMixedBuffer(1);
    QueueMixedBuffer(2);
}

AudioRenderer::~AudioRenderer() = default;

u32 AudioRenderer::GetSampleRate() const {
    return worker_params.sample_rate;
}

u32 AudioRenderer::GetSampleCount() const {
    return worker_params.sample_count;
}

u32 AudioRenderer::GetMixBufferCount() const {
    return worker_params.mix_buffer_count;
}

Stream::State AudioRenderer::GetStreamState() const {
    return stream->GetState();
}

static constexpr u32 VersionFromRevision(u32_le rev) {
    // "REV7" -> 7
    return ((rev >> 24) & 0xff) - 0x30;
}

std::vector<u8> AudioRenderer::UpdateAudioRenderer(const std::vector<u8>& input_params) {
    // Copy UpdateDataHeader struct
    UpdateDataHeader config{};
    std::memcpy(&config, input_params.data(), sizeof(UpdateDataHeader));
    u32 memory_pool_count = worker_params.effect_count + (worker_params.voice_count * 4);

    // Copy MemoryPoolInfo structs
    std::vector<MemoryPoolInfo> mem_pool_info(memory_pool_count);
    std::memcpy(mem_pool_info.data(),
                input_params.data() + sizeof(UpdateDataHeader) + config.behavior_size,
                memory_pool_count * sizeof(MemoryPoolInfo));

    // Copy VoiceInfo structs
    std::size_t voice_offset{sizeof(UpdateDataHeader) + config.behavior_size +
                             config.memory_pools_size + config.voice_resource_size};
    for (auto& voice : voices) {
        std::memcpy(&voice.GetInfo(), input_params.data() + voice_offset, sizeof(VoiceInfo));
        voice_offset += sizeof(VoiceInfo);
    }

    std::size_t channel_offset{sizeof(UpdateDataHeader) + config.behavior_size +
                               config.memory_pools_size};
    channels.resize((voice_offset - channel_offset) / sizeof(ChannelInfoIn));
    for (auto& channel : channels) {
        std::memcpy(&channel.GetInfo(), input_params.data() + channel_offset,
                    sizeof(ChannelInfoIn));
        channel_offset += sizeof(ChannelInfoIn);
    }

    std::size_t effect_offset{sizeof(UpdateDataHeader) + config.behavior_size +
                              config.memory_pools_size + config.voice_resource_size +
                              config.voices_size};
    for (auto& effect : effects) {
        std::memcpy(&effect.GetInfo(), input_params.data() + effect_offset, sizeof(EffectInStatus));
        effect_offset += sizeof(EffectInStatus);
    }

    // Update memory pool state
    std::vector<MemoryPoolEntry> memory_pool(memory_pool_count);
    for (std::size_t index = 0; index < memory_pool.size(); ++index) {
        if (mem_pool_info[index].pool_state == MemoryPoolStates::RequestAttach) {
            memory_pool[index].state = MemoryPoolStates::Attached;
        } else if (mem_pool_info[index].pool_state == MemoryPoolStates::RequestDetach) {
            memory_pool[index].state = MemoryPoolStates::Detached;
        }
    }

    // Update voices
    for (auto& voice : voices) {
        voice.UpdateState();
        if (!voice.GetInfo().is_in_use) {
            continue;
        }
        if (voice.GetInfo().is_new) {
            voice.SetWaveIndex(voice.GetInfo().wave_buffer_head);
        }
    }

    for (auto& effect : effects) {
        effect.UpdateState(memory);
    }

    // Release previous buffers and queue next ones for playback
    ReleaseAndQueueBuffers();

    // Copy output header
    UpdateDataHeader response_data{worker_params};
    std::vector<u8> output_params(response_data.total_size);
    const auto audren_revision = VersionFromRevision(config.revision);
    if (audren_revision >= 5) {
        response_data.frame_count = 0x10;
        response_data.total_size += 0x10;
    }
    std::memcpy(output_params.data(), &response_data, sizeof(UpdateDataHeader));

    // Copy output memory pool entries
    std::memcpy(output_params.data() + sizeof(UpdateDataHeader), memory_pool.data(),
                response_data.memory_pools_size);

    // Copy output voice status
    std::size_t voice_out_status_offset{sizeof(UpdateDataHeader) + response_data.memory_pools_size};
    for (const auto& voice : voices) {
        std::memcpy(output_params.data() + voice_out_status_offset, &voice.GetOutStatus(),
                    sizeof(VoiceOutStatus));
        voice_out_status_offset += sizeof(VoiceOutStatus);
    }

    std::size_t effect_out_status_offset{
        sizeof(UpdateDataHeader) + response_data.memory_pools_size + response_data.voices_size +
        response_data.voice_resource_size};
    for (const auto& effect : effects) {
        std::memcpy(output_params.data() + effect_out_status_offset, &effect.GetOutStatus(),
                    sizeof(EffectOutStatus));
        effect_out_status_offset += sizeof(EffectOutStatus);
    }
    return output_params;
}

void AudioRenderer::VoiceState::SetWaveIndex(std::size_t index) {
    wave_index = index & 3;
    is_refresh_pending = true;
}

std::vector<s16> AudioRenderer::VoiceState::DequeueSamples(std::size_t sample_count,
                                                           Memory::Memory& memory) {
    if (!IsPlaying()) {
        return {};
    }

    if (is_refresh_pending) {
        RefreshBuffer(memory);
    }

    const std::size_t max_size{samples.size() - offset};
    const std::size_t dequeue_offset{offset};
    std::size_t size{sample_count * STREAM_NUM_CHANNELS};
    if (size > max_size) {
        size = max_size;
    }

    out_status.played_sample_count += size / STREAM_NUM_CHANNELS;
    offset += size;

    const auto& wave_buffer{info.wave_buffer[wave_index]};
    if (offset == samples.size()) {
        offset = 0;

        if (!wave_buffer.is_looping && wave_buffer.buffer_sz) {
            SetWaveIndex(wave_index + 1);
        }

        if (wave_buffer.buffer_sz) {
            out_status.wave_buffer_consumed++;
        }

        if (wave_buffer.end_of_stream || wave_buffer.buffer_sz == 0) {
            info.play_state = PlayState::Paused;
        }
    }

    return {samples.begin() + dequeue_offset, samples.begin() + dequeue_offset + size};
}

void AudioRenderer::VoiceState::UpdateState() {
    if (is_in_use && !info.is_in_use) {
        // No longer in use, reset state
        is_refresh_pending = true;
        wave_index = 0;
        offset = 0;
        out_status = {};
    }
    is_in_use = info.is_in_use;
}

void AudioRenderer::VoiceState::RefreshBuffer(Memory::Memory& memory) {
    const auto wave_buffer_address = info.wave_buffer[wave_index].buffer_addr;
    const auto wave_buffer_size = info.wave_buffer[wave_index].buffer_sz;
    std::vector<s16> new_samples(wave_buffer_size / sizeof(s16));
    memory.ReadBlock(wave_buffer_address, new_samples.data(), wave_buffer_size);

    switch (static_cast<Codec::PcmFormat>(info.sample_format)) {
    case Codec::PcmFormat::Int16: {
        // PCM16 is played as-is
        break;
    }
    case Codec::PcmFormat::Adpcm: {
        // Decode ADPCM to PCM16
        Codec::ADPCM_Coeff coeffs;
        memory.ReadBlock(info.additional_params_addr, coeffs.data(), sizeof(Codec::ADPCM_Coeff));
        new_samples = Codec::DecodeADPCM(reinterpret_cast<u8*>(new_samples.data()),
                                         new_samples.size() * sizeof(s16), coeffs, adpcm_state);
        break;
    }
    default:
        UNIMPLEMENTED_MSG("Unimplemented sample_format={}", info.sample_format);
        break;
    }

    switch (info.channel_count) {
    case 1:
        // 1 channel is upsampled to 2 channel
        samples.resize(new_samples.size() * 2);
        for (std::size_t index = 0; index < new_samples.size(); ++index) {
            samples[index * 2] = new_samples[index];
            samples[index * 2 + 1] = new_samples[index];
        }
        break;
    case 2: {
        // 2 channel is played as is
        samples = std::move(new_samples);
        break;
    }
    default:
        UNIMPLEMENTED_MSG("Unimplemented channel_count={}", info.channel_count);
        break;
    }

    // Only interpolate when necessary, expensive.
    if (GetInfo().sample_rate != STREAM_SAMPLE_RATE) {
        samples = Interpolate(interp_state, std::move(samples), GetInfo().sample_rate,
                              STREAM_SAMPLE_RATE);
    }

    is_refresh_pending = false;
}

void AudioRenderer::EffectState::UpdateState(Memory::Memory& memory) {
    if (info.is_new) {
        out_status.state = EffectStatus::New;
    } else {
        if (info.type == Effect::Aux) {
            ASSERT_MSG(memory.Read32(info.aux_info.return_buffer_info) == 0,
                       "Aux buffers tried to update");
            ASSERT_MSG(memory.Read32(info.aux_info.send_buffer_info) == 0,
                       "Aux buffers tried to update");
            ASSERT_MSG(memory.Read32(info.aux_info.return_buffer_base) == 0,
                       "Aux buffers tried to update");
            ASSERT_MSG(memory.Read32(info.aux_info.send_buffer_base) == 0,
                       "Aux buffers tried to update");
        }
    }
}

static constexpr s16 ClampToS16(s32 value) {
    return static_cast<s16>(std::clamp(value, -32768, 32767));
}

void AudioRenderer::QueueMixedBuffer(Buffer::Tag tag) {
    constexpr std::size_t BUFFER_SIZE{512};
    std::vector<s16> buffer(BUFFER_SIZE * stream->GetNumChannels());

    for (auto& voice : voices) {
        if (!voice.IsPlaying()) {
            continue;
        }

        std::size_t offset{};
        s64 samples_remaining{BUFFER_SIZE};
        while (samples_remaining > 0) {
            const std::vector<s16> samples{voice.DequeueSamples(samples_remaining, memory)};

            if (samples.empty()) {
                break;
            }

            samples_remaining -= samples.size() / stream->GetNumChannels();

            // TODO(FearlessTobi): Implement Surround mixing
            const auto& mix = channels[voice.GetInfo().id].GetInfo().mix;
            for (const auto& sample : samples) {
                const s32 buffer_sample{buffer[offset]};

                // index 0 is for the left ear, 1 is for the right
                const float submix = mix[offset % 2];

                buffer[offset++] = ClampToS16(
                    buffer_sample + static_cast<s32>(sample * voice.GetInfo().volume * submix));
            }
        }
    }
    audio_out->QueueBuffer(stream, tag, std::move(buffer));
}

void AudioRenderer::ReleaseAndQueueBuffers() {
    const auto released_buffers{audio_out->GetTagsAndReleaseBuffers(stream, 2)};
    for (const auto& tag : released_buffers) {
        QueueMixedBuffer(tag);
    }
}

} // namespace AudioCore
