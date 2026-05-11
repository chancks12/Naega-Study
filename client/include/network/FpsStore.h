#pragma once

#include <string>

// 카메라 FPS 설정을 %APPDATA%\StudySync\settings.dat 에 저장/로드.
class FpsStore {
public:
    static constexpr int kDefaultFps = 30;
    static constexpr int kTargetFps  = 30; // AI 서버가 학습한 기준 fps

    void save(int fps) const;
    int  load() const;          // 파일 없으면 kDefaultFps 반환

private:
    std::string file_path() const;
};
