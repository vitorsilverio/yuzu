// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <array>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>

#include "common/assert.h"
#include "common/common_types.h"
#include "common/file_util.h"
#include "common/logging/log.h"

#include "core/file_sys/control_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/loader/loader.h"
#include "core/settings.h"
#include "core/telemetry_session.h"

#ifdef ENABLE_WEB_SERVICE
#include "web_service/telemetry_json.h"
#include "web_service/verify_login.h"
#endif

namespace Core {

static u64 GenerateTelemetryId() {
    u64 telemetry_id{};

    mbedtls_entropy_context entropy;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_context ctr_drbg;
    constexpr std::array<char, 18> personalization{{"yuzu Telemetry ID"}};

    mbedtls_ctr_drbg_init(&ctr_drbg);
    ASSERT(mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                 reinterpret_cast<const unsigned char*>(personalization.data()),
                                 personalization.size()) == 0);
    ASSERT(mbedtls_ctr_drbg_random(&ctr_drbg, reinterpret_cast<unsigned char*>(&telemetry_id),
                                   sizeof(u64)) == 0);

    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);

    return telemetry_id;
}

static const char* TranslateRenderer(Settings::RendererBackend backend) {
    switch (backend) {
    case Settings::RendererBackend::OpenGL:
        return "OpenGL";
    case Settings::RendererBackend::Vulkan:
        return "Vulkan";
    }
    return "Unknown";
}

u64 GetTelemetryId() {
    u64 telemetry_id{};
    const std::string filename{FileUtil::GetUserPath(FileUtil::UserPath::ConfigDir) +
                               "telemetry_id"};

    bool generate_new_id = !FileUtil::Exists(filename);
    if (!generate_new_id) {
        FileUtil::IOFile file(filename, "rb");
        if (!file.IsOpen()) {
            LOG_ERROR(Core, "failed to open telemetry_id: {}", filename);
            return {};
        }
        file.ReadBytes(&telemetry_id, sizeof(u64));
        if (telemetry_id == 0) {
            LOG_ERROR(Frontend, "telemetry_id is 0. Generating a new one.", telemetry_id);
            generate_new_id = true;
        }
    }

    if (generate_new_id) {
        FileUtil::IOFile file(filename, "wb");
        if (!file.IsOpen()) {
            LOG_ERROR(Core, "failed to open telemetry_id: {}", filename);
            return {};
        }
        telemetry_id = GenerateTelemetryId();
        file.WriteBytes(&telemetry_id, sizeof(u64));
    }

    return telemetry_id;
}

u64 RegenerateTelemetryId() {
    const u64 new_telemetry_id{GenerateTelemetryId()};
    const std::string filename{FileUtil::GetUserPath(FileUtil::UserPath::ConfigDir) +
                               "telemetry_id"};

    FileUtil::IOFile file(filename, "wb");
    if (!file.IsOpen()) {
        LOG_ERROR(Core, "failed to open telemetry_id: {}", filename);
        return {};
    }
    file.WriteBytes(&new_telemetry_id, sizeof(u64));
    return new_telemetry_id;
}

bool VerifyLogin(const std::string& username, const std::string& token) {
#ifdef ENABLE_WEB_SERVICE
    return WebService::VerifyLogin(Settings::values.web_api_url, username, token);
#else
    return false;
#endif
}

TelemetrySession::TelemetrySession() = default;

TelemetrySession::~TelemetrySession() {
    // Log one-time session end information
    const s64 shutdown_time{std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count()};
    AddField(Telemetry::FieldType::Session, "Shutdown_Time", shutdown_time);

#ifdef ENABLE_WEB_SERVICE
    auto backend = std::make_unique<WebService::TelemetryJson>(
        Settings::values.web_api_url, Settings::values.yuzu_username, Settings::values.yuzu_token);
#else
    auto backend = std::make_unique<Telemetry::NullVisitor>();
#endif

    // Complete the session, submitting to the web service backend if necessary
    field_collection.Accept(*backend);
    if (Settings::values.enable_telemetry) {
        backend->Complete();
    }
}

void TelemetrySession::AddInitialInfo(Loader::AppLoader& app_loader) {
    // Log one-time top-level information
    AddField(Telemetry::FieldType::None, "TelemetryId", GetTelemetryId());

    // Log one-time session start information
    const s64 init_time{std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count()};
    AddField(Telemetry::FieldType::Session, "Init_Time", init_time);

    u64 program_id{};
    const Loader::ResultStatus res{app_loader.ReadProgramId(program_id)};
    if (res == Loader::ResultStatus::Success) {
        const std::string formatted_program_id{fmt::format("{:016X}", program_id)};
        AddField(Telemetry::FieldType::Session, "ProgramId", formatted_program_id);

        std::string name;
        app_loader.ReadTitle(name);

        if (name.empty()) {
            auto [nacp, icon_file] = FileSys::PatchManager(program_id).GetControlMetadata();
            if (nacp != nullptr) {
                name = nacp->GetApplicationName();
            }
        }

        if (!name.empty()) {
            AddField(Telemetry::FieldType::Session, "ProgramName", name);
        }
    }

    AddField(Telemetry::FieldType::Session, "ProgramFormat",
             static_cast<u8>(app_loader.GetFileType()));

    // Log application information
    Telemetry::AppendBuildInfo(field_collection);

    // Log user system information
    Telemetry::AppendCPUInfo(field_collection);
    Telemetry::AppendOSInfo(field_collection);

    // Log user configuration information
    constexpr auto field_type = Telemetry::FieldType::UserConfig;
    AddField(field_type, "Audio_SinkId", Settings::values.sink_id);
    AddField(field_type, "Audio_EnableAudioStretching", Settings::values.enable_audio_stretching);
    AddField(field_type, "Audio_EnableRealTime", Settings::values.enable_realtime_audio);
    AddField(field_type, "Core_UseMultiCore", Settings::values.use_multi_core);
    AddField(field_type, "Renderer_Backend", TranslateRenderer(Settings::values.renderer_backend));
    AddField(field_type, "Renderer_ResolutionFactor", Settings::values.resolution_factor);
    AddField(field_type, "Renderer_UseFrameLimit", Settings::values.use_frame_limit);
    AddField(field_type, "Renderer_FrameLimit", Settings::values.frame_limit);
    AddField(field_type, "Renderer_UseDiskShaderCache", Settings::values.use_disk_shader_cache);
    AddField(field_type, "Renderer_UseAccurateGpuEmulation",
             Settings::values.use_accurate_gpu_emulation);
    AddField(field_type, "Renderer_UseAsynchronousGpuEmulation",
             Settings::values.use_asynchronous_gpu_emulation);
    AddField(field_type, "System_UseDockedMode", Settings::values.use_docked_mode);
}

bool TelemetrySession::SubmitTestcase() {
#ifdef ENABLE_WEB_SERVICE
    auto backend = std::make_unique<WebService::TelemetryJson>(
        Settings::values.web_api_url, Settings::values.yuzu_username, Settings::values.yuzu_token);
    field_collection.Accept(*backend);
    return backend->SubmitTestcase();
#else
    return false;
#endif
}

} // namespace Core
