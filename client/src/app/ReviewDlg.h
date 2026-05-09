#pragma once

#include "model/ReviewEvent.h"
#include "model/ReviewEventStore.h"
#include "network/ConsentStore.h"
#include "network/FeedbackApi.h"

#include <vector>

// 세션 종료 후 복기 화면 다이얼로그.
// AI 서버가 이미 임계값 필터링을 완료한 이벤트만 표시한다.
//
// [맞아요]   → 이벤트를 'Correct'로 표시, 별도 업로드 없음
// [틀렸어요] → ConsentStore 확인 후 동의 팝업 (최초 1회)
//             → FeedbackApi::send() POST /feedback multipart/form-data

class ReviewDlg : public CDialog {
public:
    explicit ReviewDlg(ReviewEventStore& store,
                       long long session_id,
                       CWnd* parent = nullptr);

    enum { IDD = 202 }; // IDD_REVIEW

protected:
    BOOL OnInitDialog() override;
    void DoDataExchange(CDataExchange* pDX) override;

    afx_msg void OnNMCustomdrawListEvents(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void OnLvnItemchangedListEvents(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void OnBnClickedCorrect();
    afx_msg void OnBnClickedWrong();
    DECLARE_MESSAGE_MAP()

private:
    void populate_list();
    void refresh_item(int index);
    void update_feedback_panel(int index);
    void apply_feedback(int index, ReviewEvent::Feedback fb);

    static CString format_time(std::uint64_t timestamp_ms);
    static CString type_label(PostureEventType type);
    static CString feedback_label(ReviewEvent::Feedback fb);

    ReviewEventStore&        store_;
    long long                session_id_ = 0;
    std::vector<ReviewEvent> events_;

    CListCtrl list_ctrl_;
    int       sel_index_ = -1;
};
