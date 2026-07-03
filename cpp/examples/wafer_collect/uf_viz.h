// uf_viz.h - 화면용 스냅샷 찍는 flow. 렌더러도 결국 uniflow 모듈 하나.
#pragma once

#include "fab.h"
#include "uf_collector.h"
#include "uf_tm1.h"
#include "uf_tm2.h"
#include "uniflow.hpp"

class Flow_Viz : public uniflow::Uniflow<Flow_Viz>
{
public:
    Flow_Viz(uniflow::Runtime& rt, Flow_Collector& col, Flow_TM1& tm1,
             Flow_TM2& tm2, Fab& fab);

    struct Task_Snapshot : uniflow::Task<Flow_Viz>
    {
        StepResult Entry() override { return Step1_Tick(); }

    private:
        StepResult Step1_Tick();
    } task_snapshot_;

private:
    Flow_Collector& col_;
    Flow_TM1&       tm1_;
    Flow_TM2&       tm2_;
    Fab&            fab_;
};

// 메인 스레드 Win32 렌더 루프. 창 닫힐 때까지 블록.
void RunVisualisation();
