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

    void start_capture();
    void stop_capture();

    // 설정 패널에서 FPS 변경 → 실행 중인 뷰에 즉시 반영
    void update_camera_fps(int fps);

    int user_id() const { return user_id_; }

    static constexpr UINT WM_STOP_CAPTURE          = WM_APP + 1;
    static constexpr UINT WM_SESSION_STARTED       = WM_APP + 2;
    static constexpr UINT WM_DESTROY_CAPTURE_VIEW  = WM_APP + 3;

protected:
    afx_msg int    OnCreate(LPCREATESTRUCT lpCreateStruct);
    afx_msg void   OnClose();
    afx_msg void   OnSize(UINT nType, int cx, int cy);
    afx_msg void   OnTabSelChange(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void   OnBnClickedStop();
    afx_msg LRESULT OnStopCaptureMsg(WPARAM, LPARAM);
    afx_msg LRESULT OnSessionStarted(WPARAM, LPARAM);
    afx_msg LRESULT OnDestroyCaptureView(WPARAM, LPARAM);
    DECLARE_MESSAGE_MAP()

private:
    void layout_content(int cx, int cy);
    void show_tab(int index);

    int user_id_ = 0;

    CTabCtrl tab_ctrl_;
    enum { TAB_HOME = 0, TAB_CLIPS, TAB_STATS, TAB_SETTINGS, TAB_COUNT };
    CWnd* panels_[TAB_COUNT] = {};
    int   active_tab_ = TAB_HOME;

    CStudySyncClientView* capture_view_  = nullptr;
    bool                  capturing_    = false;
    // stop_capture() 이후 구 뷰의 스레드가 완전히 정리될 때까지 true
    // 이 플래그가 true인 동안 start_capture()를 막아 카메라 이중 점유를 방지
    bool                  tearing_down_ = false;

    CButton btn_stop_;
    CFont   font_stop_;
};
