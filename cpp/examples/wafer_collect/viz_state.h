// viz_state.h - pump -> UI 스냅샷. 스레드 넘나드는 lock은 여기 하나뿐.
#pragma once

#include "fab.h"

#include <mutex>
#include <string>

struct CellView
{
    SlotState   state = SlotState::Unknown;
    bool        has_phys = false;   // 물리적으로 웨이퍼 있음(전지적 시점이라 다 보여줌)
    std::string wafer;
};

struct PmView
{
    bool        has = false;
    std::string wafer;
    bool        preparing = false;
    bool        ready = false;
    long long   remaining_ms = 0;   // 방출 준비 남은 시간
};

struct Snapshot
{
    double      tm1_x = 0.0;
    double      tm1_y = 0.0;
    bool        tm1_carry = false;
    std::string tm1_wafer;
    std::string tm1_phase = "-";

    double      tm2_x = 0.0;
    double      tm2_y = 0.0;
    bool        tm2_carry = false;
    std::string tm2_wafer;
    std::string tm2_phase = "-";

    CellView buffer[Fab::kBuf];
    PmView   pm[Fab::kPM];

    int         foup_count = 0;
    std::string phase = "-";
};

extern Snapshot   g_snap;
extern std::mutex g_snap_mu;

Snapshot ReadSnapshot();
