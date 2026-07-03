// uf_tm2.h - TM2 로봇(Buffer <-> PM 담당)을 uniflow 모듈로.
//
// pick 과 place 를 별도 Task 로 분리. 핵심은 이거다: PM이 collect 준비(ready) 되면
// TM2는 buffer에 빈 칸이 있든 없든 일단 즉시 가서 뜬다(PM을 빨리 비워줌). 그리고
// 중립위치에서 웨이퍼를 들고 대기하다가, buffer에 빈(Empty) 칸이 생기면 그때 놓는다.
//
// 안전(요구사항): TM2가 슬롯에 내려놓자마자 TM1이 집으면 위험하니, TM2가 완전히
// 빠져나가면(중립까지 retract 완료) 그 슬롯 safe=true 로 표시하고, TM1은 그때만 뜬다.
//
// TM1과 같은 pump 스레드에서 동시에 돈다(producer/consumer, lock-free).
#pragma once

#include "fab.h"
#include "layout.h"
#include "sim_hw.h"
#include "uniflow.hpp"

class Flow_TM2 : public uniflow::Uniflow<Flow_TM2>
{
public:
    Flow_TM2(uniflow::Runtime& rt, Fab& fab);

    // pick: 준비된 PM 찾으면 즉시 가서 뜨고 -> 중립. 그다음 place.
    struct Task_Pick : uniflow::Task<Flow_TM2>
    {
        int         pm = -1;
        std::string wafer;

        void       OnEnter() override { pm = -1; wafer.clear(); }
        StepResult Entry() override { return Step1_Decide(); }

    private:
        StepResult Step1_Decide();   // 준비된 PM 있으면 pick, 다 비었으면 Done
        StepResult Step2_GotoPm();
        StepResult Step3_AtPm();
        StepResult Step4_ToNeutral();
        StepResult Step5_AtNeutral();   // -> place task
    } task_pick_;

    // place: 중립에서 빈 buffer 슬롯 생길 때까지 들고 대기 -> 놓고 -> 빠져나온 뒤 safe.
    struct Task_Place : uniflow::Task<Flow_TM2>
    {
        int dst = -1;

        void       OnEnter() override { dst = -1; }
        StepResult Entry() override { return Step1_WaitSlot(); }

    private:
        StepResult Step1_WaitSlot(); // 빈 칸 날 때까지 중립에서 홀딩(Stay)
        StepResult Step2_ToSlot();
        StepResult Step3_AtSlot();   // 놓음(Occupied, safe=false)
        StepResult Step4_Retract();  // 중립으로 완전히 빠져나옴
        StepResult Step5_AtNeutral();// 빠져나왔으니 safe=true -> pick task
    } task_place_;

    // -- 뷰용 --
    double             X() const { return robot_.X(); }
    double             Y() const { return robot_.Y(); }
    bool               Carrying() const { return robot_.Carrying(); }
    const std::string& Wafer() const { return robot_.Wafer(); }

private:
    static constexpr double kHomeX = layout::kTM2BaseX;   // 중립 = base
    static constexpr double kHomeY = layout::kTM2BaseY;

    Fab&       fab_;
    sim::Robot robot_{"TM2", kHomeX, kHomeY};
};
