// viz_state.cpp - 스냅샷 저장소 + 읽기 접근자.
#include "viz_state.h"

Snapshot   g_snap;
std::mutex g_snap_mu;

Snapshot ReadSnapshot()
{
    std::lock_guard<std::mutex> lk(g_snap_mu);
    return g_snap;
}
