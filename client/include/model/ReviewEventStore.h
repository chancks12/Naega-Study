#pragma once

#include "model/ReviewEvent.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

// 세션 중 발생한 이벤트를 복기 화면에 전달하기 위한 스레드 안전 컨테이너.
// EventUploadThread(쓰기) ↔ ReviewDlg / MainFrm(읽기·갱신)
class ReviewEventStore {
public:
    void push(const ReviewEvent& evt) {
        std::lock_guard<std::mutex> lock(mtx_);
        events_.push_back(evt);
    }

    // 정렬된 복사본 반환: timestamp 오름차순
    std::vector<ReviewEvent> sorted_events() const {
        std::lock_guard<std::mutex> lock(mtx_);
        std::vector<ReviewEvent> copy = events_;
        std::sort(copy.begin(), copy.end(), [](const ReviewEvent& a, const ReviewEvent& b) {
            return a.timestamp_ms < b.timestamp_ms;
        });
        return copy;
    }

    // 피드백 갱신 (ReviewDlg에서 호출)
    void update_feedback(const std::string& event_id, ReviewEvent::Feedback fb) {
        std::lock_guard<std::mutex> lock(mtx_);
        for (auto& e : events_) {
            if (e.event_id == event_id) {
                e.feedback = fb;
                break;
            }
        }
    }

    int count() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return static_cast<int>(events_.size());
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mtx_);
        events_.clear();
    }

private:
    mutable std::mutex        mtx_;
    std::vector<ReviewEvent>  events_;
};
