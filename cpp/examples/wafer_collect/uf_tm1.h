// uf_tm1.h - TM1 로봇(FOUP <-> Buffer 담당)을 uniflow 모듈로.
//
// pick 과 place 를 별도 Task 로 분리했다. 그래야 가져갈 웨이퍼가 있으면 일단 즉시
// 떠서(pick) 중립위치(base)에서 들고 대기하다가, 놓을 준비 되면 place 한다. 두 task는
// StartTask 로 서로 넘겨준다 (pick -> place -> pick ...). 로봇 위치/들고있는 웨이퍼는
// flow 멤버(robot_)라 task 넘어가도 유지됨.
//
// TM1이 하는 일(두 가지를 한 pick 이 처리):
//   - sweep : Unknown 슬롯을 dummy pick 해서 유무 확인(센서 없음). 있으면 뜬다.
//   - drain : TM2가 채운(Occupied) 슬롯 중 safe 된 것을 뜬다.
//   pick 하면 -> 중립으로 -> place(FOUP) -> 중립 -> 다시 pick.
//
// TM1/TM2가 buffer 슬롯 상태(fab)로만 대화하며 한 pump 스레드에서 동시에 돈다.
// 예전이면 AllWafer_Withdraw_initLoc / DummyWafer_Withdraw_DoingADE 안에 모듈 순회 +
// early return 십수 개였을 자리.
#pragma once

#include "fab.h"
#include "layout.h"
#include "sim_hw.h"
#include "uniflow.hpp"

class Flow_TM1 : public uniflow::Uniflow<Flow_TM1>
{
public:
    Flow_TM1(uniflow::Runtime& rt, Fab& fab);

    struct Task_Place;   // 전방 선언 (pick 이 StartTask 로 참조)

    // pick: 뜰 웨이퍼 정하고 -> 슬롯 가서 뜨고 -> 중립으로. 그다음 place 로 넘김.
    struct Task_Pick : uniflow::Task<Flow_TM1>
    {
        int         slot = -1;
        bool        sweep = false;   // true=Unknown 떠보기, false=Occupied drain
        std::string wafer;

        void       OnEnter() override { slot = -1; sweep = false; wafer.clear(); }
        StepResult Entry() override { return Step1_Decide(); }

    private:
        StepResult Step1_Decide();        // 뭘 뜰지 결정(마진 지켜서). 없으면 Stay, 끝나면 Done
        StepResult Step2_GotoSlot();
        StepResult Step3_AtSlot();        // sweep이면 dummy pick, drain이면 바로 grab
        StepResult Step4_Detect();        // sweep 감지 결과
        StepResult Step5_EmptyToNeutral();// 빈 슬롯이었으면 중립 복귀
        StepResult Step6_AtNeutral();     // 중립 도착 -> 다시 결정
        StepResult Step7_ToNeutral();     // 들고 중립으로
        StepResult Step8_AtNeutral();     // 들고 중립 도착 -> place task
    } task_pick_;

    // place: 중립에서 FOUP 로 가서 놓고 -> 중립. 그다음 pick 으로 넘김.
    struct Task_Place : uniflow::Task<Flow_TM1>
    {
        StepResult Entry() override { return Step1_ToFoup(); }

    private:
        StepResult Step1_ToFoup();
        StepResult Step2_AtFoup();
        StepResult Step3_ToNeutral();
        StepResult Step4_AtNeutral();   // -> pick task
    } task_place_;

    // -- 뷰용 --
    double             X() const { return robot_.X(); }
    double             Y() const { return robot_.Y(); }
    bool               Carrying() const { return robot_.Carrying(); }
    const std::string& Wafer() const { return robot_.Wafer(); }

private:
    static constexpr double kHomeX = layout::kTM1BaseX;   // 중립 = base
    static constexpr double kHomeY = layout::kTM1BaseY;

    Fab&       fab_;
    sim::Robot robot_{"TM1", kHomeX, kHomeY};
};
