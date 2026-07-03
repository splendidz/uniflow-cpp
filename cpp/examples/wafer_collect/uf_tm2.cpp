// uf_tm2.cpp - Flow_TM2 step 본문. pick <-> place 두 task.
#include "uf_tm2.h"

#include <string>

using namespace uniflow;
using namespace std::chrono_literals;

Flow_TM2::Flow_TM2(uniflow::Runtime& rt, Fab& fab)
    : uniflow::Uniflow<Flow_TM2>(rt, "TM2"), fab_(fab)
{
    AddTask(task_pick_);
    AddTask(task_place_);
}

// ============================================================================
//  Task_Pick
// ============================================================================

// 준비(ready)된 PM 있으면 빈 칸 여부 상관없이 즉시 뜨러 감. 없으면 대기, PM 다
// 비었으면 종료.
StepResult Flow_TM2::Task_Pick::Step1_Decide()
{
    Fab& f = flow().fab_;
    for (int j = 0; j < Fab::kPM; ++j)
    {
        if (f.pm[j].has && f.pm[j].Ready())
        {
            pm = j;
            return Next(UF_FN(Step2_GotoPm));
        }
    }
    if (f.PmWafers() == 0)
    {
        f.tm2_done = true;   // TM1 종료 판단용
        Describe("TM2 done (home)");
        return Done();
    }
    // PM 준비 대기. hold 표시 안 함(Describe 갱신 안 함).
    return Stay();
}

StepResult Flow_TM2::Task_Pick::Step2_GotoPm()
{
    Describe("TM2 -> PM" + std::to_string(pm + 1));
    flow().robot_.StartMove(layout::PmX(pm), layout::PmY(pm), 1100ms, false, "");
    return Next(UF_FN(Step3_AtPm));
}

StepResult Flow_TM2::Task_Pick::Step3_AtPm()
{
    if (!flow().robot_.MoveDone())
    {
        return Stay();
    }
    // grab (PM 즉시 비워줌)
    wafer = flow().fab_.pm[pm].wafer;
    flow().fab_.pm[pm].clear();
    return Next(UF_FN(Step4_ToNeutral));
}

StepResult Flow_TM2::Task_Pick::Step4_ToNeutral()
{
    Describe("TM2 pick " + wafer + " -> neutral");
    flow().robot_.StartMove(Flow_TM2::kHomeX, Flow_TM2::kHomeY, 1100ms, true, wafer);
    return Next(UF_FN(Step5_AtNeutral));
}

StepResult Flow_TM2::Task_Pick::Step5_AtNeutral()
{
    if (!flow().robot_.MoveDone())
    {
        return Stay();
    }
    return StartTask(flow().task_place_);
}

// ============================================================================
//  Task_Place
// ============================================================================

// 중립에서 웨이퍼 들고, buffer 빈 칸 생길 때까지 대기(Stay). 이게 "즉시 뜨고 중립
// 대기"의 핵심 - PM은 이미 비워줬고 놓을 자리만 기다림.
StepResult Flow_TM2::Task_Place::Step1_WaitSlot()
{
    // TM1이 잡고 있는 슬롯과 최소 2칸 마진 둔 빈 칸을 고른다. 없으면 중립에서 홀딩.
    int e = flow().fab_.FirstEmptyAway(flow().fab_.tm1_busy_slot);
    if (e < 0)
    {
        // 빈 칸/마진 대기. hold 표시 안 함(Describe 갱신 안 함).
        return Stay();
    }
    dst                       = e;
    flow().fab_.tm2_busy_slot = e;   // 선택과 동시에 점유(같은 step)
    return Next(UF_FN(Step2_ToSlot));
}

StepResult Flow_TM2::Task_Place::Step2_ToSlot()
{
    Describe("TM2 place " + flow().robot_.Wafer() + " -> slot " + std::to_string(dst + 1));
    flow().robot_.StartMove(layout::BufferX(), layout::BufferSlotY(dst), 1100ms, true,
                            flow().robot_.Wafer());
    return Next(UF_FN(Step3_AtSlot));
}

StepResult Flow_TM2::Task_Place::Step3_AtSlot()
{
    if (!flow().robot_.MoveDone())
    {
        return Stay();
    }
    FabSlot& s = flow().fab_.buffer[dst];
    s.state    = SlotState::Occupied;
    s.has_phys = true;
    s.wafer    = flow().robot_.Wafer();
    s.safe     = false;   // 아직 TM2가 슬롯에 있음 - TM1 접근 금지
    flow().robot_.DropWafer();
    return Next(UF_FN(Step4_Retract));
}

StepResult Flow_TM2::Task_Place::Step4_Retract()
{
    // 슬롯에서 완전히 빠져나옴(중립까지). 이게 끝나야 TM1이 가져가도 안전.
    flow().robot_.StartMove(Flow_TM2::kHomeX, Flow_TM2::kHomeY, 1100ms, false, "");
    return Next(UF_FN(Step5_AtNeutral));
}

StepResult Flow_TM2::Task_Place::Step5_AtNeutral()
{
    if (!flow().robot_.MoveDone())
    {
        return Stay();
    }
    flow().fab_.buffer[dst].safe = true;   // 완전히 빠져나옴 -> TM1이 가져가도 됨
    flow().fab_.tm2_busy_slot    = -1;     // 버퍼에서 빠짐 - 점유 해제
    return StartTask(flow().task_pick_);
}
