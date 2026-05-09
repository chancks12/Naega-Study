#pragma once

#include <cstdint>
#include <string>

enum class ClipStoreKind {
    LocalClaimCheck
};

struct ClientTransportConfig {
    ClipStoreKind clip_store = ClipStoreKind::LocalClaimCheck;

    // Main server uses HTTP for auth, sessions, logs, and claim-check metadata.
    std::string main_server_url = "http://10.10.10.130:8081";

    // AI server uses TCP for frame upload and analysis responses.
    std::string ai_server_host = "10.10.10.98";
    std::uint16_t ai_server_port = 9500;

    std::string clip_directory = "event_clips";

    int capture_fps = 30;
    int frame_sample_interval = 6; // 30fps capture / 6 = about 5fps AI sampling.
    int jpeg_quality = 80;
    std::uint32_t local_clip_retention_days = 3;

    // AI 서버 준비 전 더미 분석결과 생성기 활성화
    // true  → DummyAnalysisGenerator 사용 (AiTcpClient 비활성)
    // false → AiTcpClient 사용 (실제 AI 서버 연결)
    bool use_dummy_ai = false;
    int  dummy_interval_ms = 200; // 더미 생성 주기 (200ms ≈ 5fps)
};
