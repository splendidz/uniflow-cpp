// app.h - Runtime 하나, 공유 Fab 하나, flow 네 개(TM1/TM2/Collector/Viz).
//
// Runtime 하나 == pump 스레드 하나. 다 여기 붙어서 한 스레드에서 협조적으로 돌고
// Fab을 mutex 없이 공유. 예전 같으면 TM1 워커 스레드 + TM2 워커 스레드 + 모니터
// 스레드 + 그 위 coarse SCLock이 필요.
#pragma once

#include "fab.h"
#include "uf_collector.h"
#include "uf_tm1.h"
#include "uf_tm2.h"
#include "uf_viz.h"
#include "uniflow.hpp"

class App
{
public:
    uniflow::Runtime rt;                 // pump 스레드 1개 + ConsoleObserver 트레이스
    Fab              fab;

    Flow_TM1       tm1{rt, fab};
    Flow_TM2       tm2{rt, fab};
    Flow_Collector collector{rt, fab, tm1, tm2};
    Flow_Viz       viz{rt, collector, tm1, tm2, fab};

    // collector만 기동하면 됨. 얘가 seed 하고 PM에 collect 걸고 TM1/TM2를 순서대로
    // 직접 StartFlow 한다.
    void Start()
    {
        viz.task_snapshot_.StartFlow();
        collector.task_campaign_.StartFlow();
    }

    void WaitForDone()
    {
        collector.WaitUntilIdle();
        tm1.WaitUntilIdle();
        tm2.WaitUntilIdle();
        viz.WaitUntilIdle();
    }
};
