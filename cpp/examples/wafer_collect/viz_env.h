// viz_env.h - 창 닫힐 때 캠페인/로봇 멈추라고 알려주는 전역 stop 플래그.
#pragma once

#include <atomic>

namespace viz
{
    inline std::atomic<bool> g_stop{false};

    inline bool Stop() { return g_stop.load(); }
    inline void RequestStop() { g_stop.store(true); }
}
