#pragma once

class CStudySyncClientView;
class CHomePanel;
class CClipsPanel;
class CStatsPanel;
class CSettingsPanel;

class CMainFrame : public CFrameWnd {
public:
    explicit CMainFrame(int user_id = 0) noexcept;
    ~CMainFrame() override;

    // HomePanel "분석 시작하기" 클릭 → 캡처 모드 진입
    void start_capture();

    // StudySyncClientView "학습 종료" 클릭 → 메인 탭으로 복귀
    void stop_capture();

    int user_id() const { return user_id_; }

    static constexpr UINT WM_STOP_CAPTURE    = WM_APP + 1;
    static constexpr UINT WM_SESSION_STARTED = WM_APP + 2;

protected:
    afx_msg int    OnCreate(LPCREATESTRUCT lpCreateStruct);
    afx_msg void   OnClose();
    afx_msg void   OnSize(UINT nType, int cx, int cy);
    afx_msg void   OnTabSelChange(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void   OnBnClickedStop();
    afx_msg LRESULT OnStopCaptureMsg(WPARAM, LPARAM);
    afx_msg LRESULT OnSessionStarted(WPARAM, LPARAM);
    DECLARE_MESSAGE_MAP()

private:
    void layout_content(int cx, int cy);
    void show_tab(int index);

    int user_id_ = 0;

    CTabCtrl tab_ctrl_;
    enum { TAB_HOME = 0, TAB_CLIPS, TAB_STATS, TAB_SETTINGS, TAB_COUNT };
    CWnd* panels_[TAB_COUNT] = {};
    int   active_tab_ = TAB_HOME;

    CStudySyncClientView* capture_view_ = nullptr;
    bool                  capturing_    = false;

    CButton btn_stop_;
    CFont   font_stop_;
};
