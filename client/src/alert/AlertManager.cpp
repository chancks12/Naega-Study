#include "pch.h"
#include "alert/AlertManager.h"

AlertManager::AlertManager(AlertQueue& alert_queue)
    : alert_queue_(alert_queue)
{
}

void AlertManager::feed_local_analysis(const AnalysisResult& result)
{
    bad_posture_streak_ = (result.neck_angle > 25.0 || !result.posture_ok) ? bad_posture_streak_ + 1 : 0;
    drowsy_streak_ = (result.ear < 0.25 || result.drowsy) ? drowsy_streak_ + 1 : 0;
    focus_low_streak_ = (result.focus_score > 0 && result.focus_score < 40) ? focus_low_streak_ + 1 : 0;

    if (bad_posture_streak_ >= 5 && !posture_alert_cooldown_) {
        posture_alert_cooldown_ = true;
        push_alert(AlertType::BadPosture, AlertTarget::Popup, result.timestamp_ms, "Posture warning", "Please correct your posture.");
    }

    if (drowsy_streak_ >= 5 && !drowsy_alert_cooldown_) {
        drowsy_alert_cooldown_ = true;
        push_alert(AlertType::Drowsy, AlertTarget::Popup, result.timestamp_ms, "Drowsy warning", "Please take a short stretch break.");
    }

    if (bad_posture_streak_ == 0) {
        posture_alert_cooldown_ = false;
    }

    if (drowsy_streak_ == 0) {
        drowsy_alert_cooldown_ = false;
    }
}

void AlertManager::feed_server_alert(const Alert& alert)
{
    alert_queue_.push(alert);
}

void AlertManager::reset()
{
    bad_posture_streak_ = 0;
    drowsy_streak_ = 0;
    focus_low_streak_ = 0;
    posture_alert_cooldown_ = false;
    drowsy_alert_cooldown_ = false;
}
// 
void AlertManager::push_alert(AlertType type, AlertTarget target, std::uint64_t timestamp_ms, const char* title, const char* message)
{
    Alert alert;
    alert.type = type;
    alert.target = target;
    alert.timestamp_ms = timestamp_ms;
    alert.title = title;
    alert.message = message;
    alert_queue_.push(std::move(alert));
}

