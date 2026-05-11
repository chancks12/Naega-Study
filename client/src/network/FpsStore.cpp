#include "pch.h"
#include "network/FpsStore.h"

#include <fstream>
#include <string>
#include <shlobj.h>

void FpsStore::save(int fps) const
{
    std::ofstream out(file_path(), std::ios::trunc);
    if (out.is_open()) out << fps;
}

int FpsStore::load() const
{
    std::ifstream in(file_path());
    if (!in.is_open()) return kDefaultFps;

    int fps = kDefaultFps;
    in >> fps;
    if (fps < 1 || fps > 120) fps = kDefaultFps;
    return fps;
}

std::string FpsStore::file_path() const
{
    char path[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, path))) {
        std::string dir = std::string(path) + "\\StudySync";
        CreateDirectoryA(dir.c_str(), nullptr);
        return dir + "\\settings.dat";
    }
    return "studysync_settings.dat";
}
