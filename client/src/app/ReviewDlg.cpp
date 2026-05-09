#include "pch.h"
#include "ReviewDlg.h"
#include "resource.h"

#include <chrono>
#include <ctime>
#include <sstream>

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

static const COLORREF kColorFeedback = RGB(220, 240, 220); // 연녹색 (피드백 완료)

BEGIN_MESSAGE_MAP(ReviewDlg, CDialog)
    ON_NOTIFY(NM_CUSTOMDRAW, IDC_LIST_EVENTS, OnNMCustomdrawListEvents)
    ON_NOTIFY(LVN_ITEMCHANGED, IDC_LIST_EVENTS, OnLvnItemchangedListEvents)
    ON_BN_CLICKED(IDC_BTN_CORRECT, OnBnClickedCorrect)
    ON_BN_CLICKED(IDC_BTN_WRONG,   OnBnClickedWrong)
END_MESSAGE_MAP()

ReviewDlg::ReviewDlg(ReviewEventStore& store, long long session_id, CWnd* parent)
    : CDialog(IDD_REVIEW, parent)
    , store_(store)
    , session_id_(session_id)
{
}

void ReviewDlg::DoDataExchange(CDataExchange* pDX)
{
    CDialog::DoDataExchange(pDX);
    DDX_Control(pDX, IDC_LIST_EVENTS, list_ctrl_);
}

BOOL ReviewDlg::OnInitDialog()
{
    CDialog::OnInitDialog();

    list_ctrl_.SetExtendedStyle(
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

    //   총 460 DLU 중: 시각(120) + 판정(170) + 피드백(170)
    list_ctrl_.InsertColumn(0, _T("시각"),   LVCFMT_LEFT, 120);
    list_ctrl_.InsertColumn(1, _T("판정"),   LVCFMT_LEFT, 170);
    list_ctrl_.InsertColumn(2, _T("피드백"), LVCFMT_LEFT, 170);

    SetDlgItemText(IDC_STATIC_REVIEW_HEADER,
        _T("이번 세션에서 감지된 이벤트입니다. 판정이 맞는지 확인해 주세요."));

    events_ = store_.sorted_events();
    populate_list();
    update_feedback_panel(-1);

    return TRUE;
}

void ReviewDlg::populate_list()
{
    list_ctrl_.DeleteAllItems();
    for (int i = 0; i < static_cast<int>(events_.size()); ++i) {
        const ReviewEvent& e = events_[i];
        list_ctrl_.InsertItem(i, format_time(e.timestamp_ms));
        list_ctrl_.SetItemText(i, 1, type_label(e.type));
        list_ctrl_.SetItemText(i, 2, feedback_label(e.feedback));
        list_ctrl_.SetItemData(i, static_cast<DWORD_PTR>(i));
    }
}

void ReviewDlg::refresh_item(int index)
{
    if (index < 0 || index >= static_cast<int>(events_.size())) return;
    const ReviewEvent& e = events_[index];
    list_ctrl_.SetItemText(index, 2, feedback_label(e.feedback));
    list_ctrl_.RedrawItems(index, index);
}

void ReviewDlg::update_feedback_panel(int index)
{
    CWnd* wnd_q    = GetDlgItem(IDC_STATIC_QUESTION);
    CWnd* wnd_ok   = GetDlgItem(IDC_BTN_CORRECT);
    CWnd* wnd_ng   = GetDlgItem(IDC_BTN_WRONG);
    CWnd* wnd_done = GetDlgItem(IDC_STATIC_FEEDBACK_DONE);

    if (index < 0 || index >= static_cast<int>(events_.size())) {
        if (wnd_q)    wnd_q->ShowWindow(SW_HIDE);
        if (wnd_ok)   wnd_ok->ShowWindow(SW_HIDE);
        if (wnd_ng)   wnd_ng->ShowWindow(SW_HIDE);
        if (wnd_done) wnd_done->ShowWindow(SW_HIDE);
        return;
    }

    const ReviewEvent& e = events_[index];

    if (e.feedback != ReviewEvent::Feedback::None) {
        if (wnd_q)    wnd_q->ShowWindow(SW_HIDE);
        if (wnd_ok)   wnd_ok->ShowWindow(SW_HIDE);
        if (wnd_ng)   wnd_ng->ShowWindow(SW_HIDE);
        if (wnd_done) {
            const CString msg = (e.feedback == ReviewEvent::Feedback::Correct)
                ? _T("✓ 올바른 판정으로 확인되었습니다.")
                : _T("✗ 오류 피드백이 접수되었습니다. (업로드 예정)");
            wnd_done->SetWindowText(msg);
            wnd_done->ShowWindow(SW_SHOW);
        }
        return;
    }

    if (wnd_q) {
        wnd_q->SetWindowText(_T("이 판정이 맞나요?"));
        wnd_q->ShowWindow(SW_SHOW);
    }
    if (wnd_ok)   wnd_ok->ShowWindow(SW_SHOW);
    if (wnd_ng)   wnd_ng->ShowWindow(SW_SHOW);
    if (wnd_done) wnd_done->ShowWindow(SW_HIDE);
}

void ReviewDlg::apply_feedback(int index, ReviewEvent::Feedback fb)
{
    if (index < 0 || index >= static_cast<int>(events_.size())) return;
    events_[index].feedback = fb;
    store_.update_feedback(events_[index].event_id, fb);
    refresh_item(index);
    update_feedback_panel(index);
}

void ReviewDlg::OnBnClickedCorrect()
{
    if (sel_index_ < 0) return;
    apply_feedback(sel_index_, ReviewEvent::Feedback::Correct);
}

void ReviewDlg::OnBnClickedWrong()
{
    if (sel_index_ < 0) return;

    if (!ConsentStore::is_consented()) {
        const int answer = AfxMessageBox(
            _T("이 영상이 모델 개선 목적으로 개발팀에 전송됩니다.\n동의하시겠습니까?"),
            MB_YESNO | MB_ICONQUESTION);
        if (answer != IDYES) return;
        ConsentStore::record_consent();
    }

    apply_feedback(sel_index_, ReviewEvent::Feedback::Wrong);

    const ReviewEvent& ev = events_[sel_index_];

    FeedbackRequest req;
    req.event_id      = ev.event_id;
    req.session_id    = session_id_;
    req.model_pred    = ev.clip_dir;
    req.user_feedback = "wrong";
    req.consent_ver   = ConsentStore::kCurrentVersion;
    req.clip_path     = ev.clip_dir;

    const FeedbackResponse resp = FeedbackApi::send(req);
    if (!resp.saved) {
        OutputDebugStringA("[ReviewDlg] FeedbackApi::send 실패\n");
    }
}

void ReviewDlg::OnLvnItemchangedListEvents(NMHDR* pNMHDR, LRESULT* pResult)
{
    const NMLISTVIEW* pNMLV = reinterpret_cast<const NMLISTVIEW*>(pNMHDR);
    if ((pNMLV->uNewState & LVIS_SELECTED) && !(pNMLV->uOldState & LVIS_SELECTED)) {
        sel_index_ = pNMLV->iItem;
        update_feedback_panel(sel_index_);
    }
    *pResult = 0;
}

void ReviewDlg::OnNMCustomdrawListEvents(NMHDR* pNMHDR, LRESULT* pResult)
{
    NMLVCUSTOMDRAW* pCD = reinterpret_cast<NMLVCUSTOMDRAW*>(pNMHDR);
    *pResult = CDRF_DODEFAULT;

    switch (pCD->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        *pResult = CDRF_NOTIFYITEMDRAW;
        break;
    case CDDS_ITEMPREPAINT: {
        const int idx = static_cast<int>(pCD->nmcd.dwItemSpec);
        if (idx >= 0 && idx < static_cast<int>(events_.size())) {
            if (events_[idx].feedback != ReviewEvent::Feedback::None) {
                pCD->clrTextBk = kColorFeedback;
            }
        }
        *pResult = CDRF_NEWFONT;
        break;
    }
    default:
        break;
    }
}

CString ReviewDlg::format_time(std::uint64_t timestamp_ms)
{
    const time_t sec = static_cast<time_t>(timestamp_ms / 1000);
    struct tm t {};
    localtime_s(&t, &sec);
    CString s;
    s.Format(_T("%02d:%02d:%02d"), t.tm_hour, t.tm_min, t.tm_sec);
    return s;
}

CString ReviewDlg::type_label(PostureEventType type)
{
    switch (type) {
    case PostureEventType::Drowsy:     return _T("졸음 감지");
    case PostureEventType::BadPosture: return _T("자세 불량");
    case PostureEventType::Absent:     return _T("자리 비움");
    default:                           return _T("알 수 없음");
    }
}

CString ReviewDlg::feedback_label(ReviewEvent::Feedback fb)
{
    switch (fb) {
    case ReviewEvent::Feedback::Correct: return _T("✓ 올바른 판정");
    case ReviewEvent::Feedback::Wrong:   return _T("✗ 오류 제출");
    default:                             return _T("—");
    }
}
