#pragma once

#include <cstdint>
#include <string>

enum class AlertType {
    BadPosture,
    StretchRequired,
    ForcedBreak,
    Drowsy,
    ServerNotice
};

enum class AlertTarget {
    Popup
};

struct Alert {
    AlertType type = AlertType::BadPosture;
    AlertTarget target = AlertTarget::Popup;
    std::uint64_t timestamp_ms = 0;
    std::string title;
    std::string message;
};

