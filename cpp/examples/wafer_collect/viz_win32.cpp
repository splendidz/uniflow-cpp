// viz_win32.cpp - Win32 GDI 렌더러 (Windows 전용).
// 레이아웃: FOUP | [TM1] | Buffer(세로 8칸) | [TM2] | PM1..PM8(2열 4행).
// 로봇은 베이스에서 목표 (x,y)로 대각선으로 뻗는 팔로 그린다(x,y 동시 이동).
#include "uf_viz.h"

#include "fab.h"
#include "layout.h"
#include "viz_env.h"
#include "viz_state.h"

#include <string>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace
{
    const COLORREF kBg      = RGB(22, 24, 30);
    const COLORREF kText    = RGB(214, 218, 226);
    const COLORREF kDim     = RGB(150, 156, 168);
    const COLORREF kCell    = RGB(46, 50, 60);
    const COLORREF kUnknown = RGB(74, 66, 50);   // 미확인 슬롯(센서 없음)
    const COLORREF kFoup    = RGB(40, 56, 72);
    const COLORREF kPrep    = RGB(150, 120, 50);
    const COLORREF kReady   = RGB(56, 128, 78);
    const COLORREF kTM1     = RGB(90, 150, 230);
    const COLORREF kTM2     = RGB(230, 150, 90);
    const COLORREF kWafer   = RGB(216, 186, 76);

    void FillBoxLTRB(HDC dc, int l, int t, int r, int b, COLORREF c)
    {
        RECT   rc{l, t, r, b};
        HBRUSH br = CreateSolidBrush(c);
        FillRect(dc, &rc, br);
        DeleteObject(br);
    }

    void FillCell(HDC dc, int cx, int cy, int w, int h, COLORREF c)
    {
        FillBoxLTRB(dc, cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2, c);
    }

    void Text(HDC dc, int x, int y, const std::string& s)
    {
        TextOutA(dc, x, y, s.c_str(), static_cast<int>(s.size()));
    }

    void CenterText(HDC dc, int cx, int y, const std::string& s)
    {
        TextOutA(dc, cx - static_cast<int>(s.size()) * 3, y, s.c_str(),
                 static_cast<int>(s.size()));
    }

    void Wafer(HDC dc, int cx, int cy)
    {
        HBRUSH b   = CreateSolidBrush(kWafer);
        HBRUSH old = static_cast<HBRUSH>(SelectObject(dc, b));
        Ellipse(dc, cx - 12, cy - 12, cx + 12, cy + 12);
        SelectObject(dc, old);
        DeleteObject(b);
    }

    // 로봇: 베이스(고정) -> 현재 end-effector (x,y) 로 뻗는 팔. carry면 웨이퍼.
    void Arm(HDC dc, int base_x, int base_y, int x, int y, COLORREF color, bool carry,
             const std::string& tag, const std::string& phase)
    {
        // 베이스 블록
        FillCell(dc, base_x, base_y, 26, 26, color);

        HPEN pen = CreatePen(PS_SOLID, 5, color);
        HPEN old = static_cast<HPEN>(SelectObject(dc, pen));
        MoveToEx(dc, base_x, base_y, nullptr);
        LineTo(dc, x, y);
        SelectObject(dc, old);
        DeleteObject(pen);

        // end-effector(그리퍼)
        FillCell(dc, x, y, 20, 20, color);
        if (carry)
        {
            Wafer(dc, x, y);
        }
        SetTextColor(dc, color);
        CenterText(dc, base_x, base_y - 24, tag);
        SetTextColor(dc, kText);
        Text(dc, base_x - 60, base_y + 20, phase);
    }

    void DrawScene(HDC hdc, const RECT& rc)
    {
        HDC     mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HBITMAP old = static_cast<HBITMAP>(SelectObject(mem, bmp));

        HBRUSH bg = CreateSolidBrush(kBg);
        FillRect(mem, &rc, bg);
        DeleteObject(bg);
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, kText);

        Snapshot s = ReadSnapshot();

        // 헤더 / phase 배너
        Text(mem, 20, 10,
             "wafer_collect - collection sweep  (uniflow: TM1 + TM2 + collector, 1 "
             "pump thread, no locks)");
        SetTextColor(mem, RGB(150, 200, 250));
        Text(mem, 20, 30, s.phase);
        SetTextColor(mem, kText);

        // FOUP (세로 박스)
        FillBoxLTRB(mem, layout::kFoupBoxL, 150, layout::kFoupBoxR, 540, kFoup);
        CenterText(mem, (layout::kFoupBoxL + layout::kFoupBoxR) / 2, 130, "FOUP");
        CenterText(mem, (layout::kFoupBoxL + layout::kFoupBoxR) / 2, 330,
                   std::to_string(s.foup_count));
        SetTextColor(mem, kDim);
        CenterText(mem, (layout::kFoupBoxL + layout::kFoupBoxR) / 2, 348, "collected");
        SetTextColor(mem, kText);

        // Buffer (세로 8칸). 슬롯 상태: Unknown(센서 없어 미확인) / Empty / Occupied
        CenterText(mem, static_cast<int>(layout::BufferX()), 96, "Buffer (no sensor)");
        for (int i = 0; i < Fab::kBuf; ++i)
        {
            int      cx = static_cast<int>(layout::BufferX());
            int      cy = static_cast<int>(layout::BufferSlotY(i));
            COLORREF c  = (s.buffer[i].state == SlotState::Unknown) ? kUnknown : kCell;
            FillCell(mem, cx, cy, layout::kBufCellW, layout::kBufCellH, c);
            // 칸 테두리
            HPEN   pen = CreatePen(PS_SOLID, 1, RGB(80, 86, 98));
            HPEN   op  = static_cast<HPEN>(SelectObject(mem, pen));
            HBRUSH ob  = static_cast<HBRUSH>(SelectObject(mem, GetStockObject(NULL_BRUSH)));
            Rectangle(mem, cx - layout::kBufCellW / 2, cy - layout::kBufCellH / 2,
                      cx + layout::kBufCellW / 2, cy + layout::kBufCellH / 2);
            SelectObject(mem, ob);
            SelectObject(mem, op);
            DeleteObject(pen);
            SetTextColor(mem, kDim);
            Text(mem, cx - layout::kBufCellW / 2 + 6, cy - 8, std::to_string(i + 1));
            SetTextColor(mem, kText);
            // 전지적 시점이라 실제 웨이퍼가 있으면 상태(Unknown/Occupied) 상관없이 다 보여줌.
            // Unknown 은 배경색(kUnknown)으로 "스케줄러는 아직 확정 못함"만 구분.
            if (s.buffer[i].has_phys)
            {
                Wafer(mem, cx, cy);
                CenterText(mem, cx, cy - 6, s.buffer[i].wafer);
            }
        }

        // PM (2열 4행)
        for (int j = 0; j < Fab::kPM; ++j)
        {
            int      cx = static_cast<int>(layout::PmX(j));
            int      cy = static_cast<int>(layout::PmY(j));
            COLORREF c  = kCell;
            if (s.pm[j].has && s.pm[j].ready)
            {
                c = kReady;
            }
            else if (s.pm[j].has && s.pm[j].preparing)
            {
                c = kPrep;
            }
            FillCell(mem, cx, cy, layout::kPmCellW, layout::kPmCellH, c);
            CenterText(mem, cx, cy - layout::kPmCellH / 2 - 16, "PM" + std::to_string(j + 1));
            if (s.pm[j].has)
            {
                Wafer(mem, cx, cy - 10);
                CenterText(mem, cx, cy - 16, s.pm[j].wafer);
                if (s.pm[j].ready)
                {
                    CenterText(mem, cx, cy + 14, "READY");
                }
                else if (s.pm[j].preparing)
                {
                    CenterText(mem, cx, cy + 12, "COLLECT_PROC");
                    // 남은 시간 카운트다운(초, 소수 1자리)
                    long long ms  = s.pm[j].remaining_ms;
                    char      buf[16];
                    wsprintfA(buf, "%d.%ds",
                              static_cast<int>(ms / 1000),
                              static_cast<int>((ms % 1000) / 100));
                    CenterText(mem, cx, cy + 28, buf);
                }
                else
                {
                    CenterText(mem, cx, cy + 14, "idle");
                }
            }
            else
            {
                SetTextColor(mem, kDim);
                CenterText(mem, cx, cy - 6, "empty");
                SetTextColor(mem, kText);
            }
        }

        // 중립(neutral = base) 위치 표시 - 로봇이 작업 사이에 여기서 대기/복귀
        SetTextColor(mem, kDim);
        CenterText(mem, layout::kTM1BaseX, layout::kTM1BaseY + 34, "neutral");
        CenterText(mem, layout::kTM2BaseX, layout::kTM2BaseY + 34, "neutral");
        SetTextColor(mem, kText);

        // 로봇 팔 두 개
        Arm(mem, layout::kTM1BaseX, layout::kTM1BaseY, static_cast<int>(s.tm1_x),
            static_cast<int>(s.tm1_y), kTM1, s.tm1_carry, "TM1", s.tm1_phase);
        Arm(mem, layout::kTM2BaseX, layout::kTM2BaseY, static_cast<int>(s.tm2_x),
            static_cast<int>(s.tm2_y), kTM2, s.tm2_carry, "TM2", s.tm2_phase);

        // 하단
        Text(mem, 20, layout::kWinH - 60,
             "TM1: FOUP <-> Buffer    TM2: Buffer <-> PM    PM collect prep ~4.5s "
             "(all concurrent on one thread)");
        Text(mem, 20, layout::kWinH - 42,
             "shared Fab read/written with no mutex - one pump thread");

        BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
    }

    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_CREATE:
            SetTimer(hwnd, 1, 16, nullptr);
            return 0;
        case WM_TIMER:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC         hdc = BeginPaint(hwnd, &ps);
            RECT        rc;
            GetClientRect(hwnd, &rc);
            DrawScene(hdc, rc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            KillTimer(hwnd, 1);
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcA(hwnd, msg, wp, lp);
    }
}

void RunVisualisation()
{
    const char* cls = "uniflow_wafer_collect";
    WNDCLASSA   wc{};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandle(nullptr);
    wc.lpszClassName = cls;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassA(&wc);

    HWND hwnd = CreateWindowA(cls, "wafer_collect (uniflow seminar)",
                              WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME, CW_USEDEFAULT,
                              CW_USEDEFAULT, layout::kWinW, layout::kWinH, nullptr,
                              nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOW);

    MSG msg;
    while (GetMessageA(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}
