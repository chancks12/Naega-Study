#pragma once

#define IDR_MAINFRAME 128

// ── 다이얼로그 ─────────────────────────────────────────────────
#define IDD_LOGIN    200
#define IDD_REGISTER 201
#define IDD_REVIEW   202

// ── 로그인 컨트롤 ──────────────────────────────────────────────
#define IDC_EDIT_EMAIL           1001
#define IDC_EDIT_PASSWORD        1002
#define IDC_BTN_REGISTER         1003
#define IDC_STATIC_TITLE         1004
#define IDC_STATIC_EMAIL         1005
#define IDC_STATIC_PASSWORD      1006
#define IDC_STATIC_MSG           1007

// ── 회원가입 컨트롤 ────────────────────────────────────────────
#define IDC_EDIT_NAME            1010
#define IDC_EDIT_EMAIL2          1011
#define IDC_EDIT_PASSWORD2       1012
#define IDC_EDIT_PASSWORD_CONFIRM 1013
#define IDC_STATIC_NAME          1014
#define IDC_STATIC_MSG2          1015

// ── 복기 화면 컨트롤 ───────────────────────────────────────────
#define IDC_LIST_EVENTS          1020
#define IDC_STATIC_REVIEW_HEADER 1021
#define IDC_STATIC_QUESTION      1022
#define IDC_BTN_CORRECT          1023
#define IDC_BTN_WRONG            1024
#define IDC_STATIC_FEEDBACK_DONE 1025

// ── 메인 네비게이션 ────────────────────────────────────────────
#define IDC_TAB_MAIN            300
#define IDC_BTN_START_CAPTURE   301
#define IDC_BTN_STOP_CAPTURE    302
#define IDC_BTN_LOGOUT          303

#ifndef IDC_STATIC
#define IDC_STATIC -1
#endif
