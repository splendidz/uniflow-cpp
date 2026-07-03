// uf_tm1.cpp - Flow_TM1 step 본문. pick <-> place 두 task.
#include "uf_tm1.h"

#include <string>

using namespace uniflow;
using namespace std::chrono_literals;

Flow_TM1::Flow_TM1(uniflow::Runtime& rt, Fab& fab)
    : uniflow::Uniflow<Flow_TM1>(rt, "TM1"), fab_(fab)
{
    AddTask(task_pick_);
    AddTask(task_place_);
}

// ============================================================================
//  Task_Pick
// ============================================================================

// 뭘 뜰지 결정. Unknown 슬롯 먼저(TM2가 쓸 빈 칸을 만들어줘야 하니까), 그 다음 TM2가
// 채운 Occupied+safe 슬롯. 단, TM2가 잡고 있는 슬롯과는 최소 2칸 마진을 둔다(가까우면
// 안 고르고 기다림). 고르는 즉시 tm1_busy_slot 을 세팅해서 TM2가 이 근처를 피하게 한다.
// 할 게 없으면 Stay(중립), 다 끝났으면 Done.
StepResult Flow_TM1::Task_Pick::Step1_Decide()
{
    Fab& f = flow().fab_;

    int u = f.FirstUnknownAway(f.tm2_busy_slot);
    if (u >= 0)
    {
        slot            = u;
        sweep           = true;
        f.tm1_busy_slot = u;   // 선택과 동시에 점유 표시(같은 step 안에서)
        return Next(UF_FN(Step2_GotoSlot));
    }
    int o = f.FirstOccupiedSafeAway(f.tm2_busy_slot);
    if (o >= 0)
    {
        slot            = o;
        sweep           = false;
        f.tm1_busy_slot = o;
        return Next(UF_FN(Step2_GotoSlot));
    }
    // 여기선 아무것도 안 잡음 - 중립에 있음
    f.tm1_busy_slot = -1;

    // Unknown 없고 뺄 Occupied 도 없음(마진 무시하고도). TM2까지 끝났으면 완전 종료.
    if (f.FirstUnknown() < 0 && f.OccupiedCount() == 0 && f.tm2_done)
    {
        Describe("TM1 done (home)");
        return Done();
    }
    // 할 건 있는데 TM2랑 너무 가깝거나(마진), TM2가 아직 채우는 중 -> 그냥 대기.
    // 화면에 hold 라고 따로 표시 안 함(멈춘 것처럼 보이니까). Describe 갱신 안 함.
    return Stay();
}

StepResult Flow_TM1::Task_Pick::Step2_GotoSlot()
{
    Describe(std::string(sweep ? "TM1 -> slot (probe) " : "TM1 -> slot (drain) ")
             + std::to_string(slot + 1));
    flow().robot_.StartMove(layout::BufferX(), layout::BufferSlotY(slot), 1100ms,
                            false, "");
    return Next(UF_FN(Step3_AtSlot));
}

StepResult Flow_TM1::Task_Pick::Step3_AtSlot()
{
    if (!flow().robot_.MoveDone())
    {
        return Stay();
    }
    if (sweep)
    {
        // 센서 없어 떠보는(dummy pick) 단계지만, 제자리 정지가 길면 버퍼에서 멈춘 것처럼
        // 보이니 지연은 거의 없게(50ms).
        Describe("dummy pick slot " + std::to_string(slot + 1));
        flow().robot_.Probe(50ms);
        return Next(UF_FN(Step4_Detect));
    }
    // drain: 아는 웨이퍼라 바로 grab
    FabSlot& s = flow().fab_.buffer[slot];
    wafer      = s.wafer;
    s.clearEmpty();
    return Next(UF_FN(Step7_ToNeutral));
}

StepResult Flow_TM1::Task_Pick::Step4_Detect()
{
    if (!flow().robot_.MoveDone())
    {
        return Stay();
    }
    FabSlot& s = flow().fab_.buffer[slot];
    if (!s.has_phys)
    {
        // 빈 칸이었음 - 확인됨(Empty). 중립으로 돌아가서 다시 결정(그래야 이 슬롯을
        // 계속 점유해 TM2를 막지 않음).
        Describe("slot " + std::to_string(slot + 1) + " empty");
        s.clearEmpty();
        return Next(UF_FN(Step5_EmptyToNeutral));
    }
    // 웨이퍼 있음 - grab
    wafer = s.wafer;
    s.clearEmpty();
    return Next(UF_FN(Step7_ToNeutral));
}

StepResult Flow_TM1::Task_Pick::Step5_EmptyToNeutral()
{
    flow().fab_.tm1_busy_slot = -1;   // 슬롯에서 빠짐 - 점유 해제
    flow().robot_.StartMove(Flow_TM1::kHomeX, Flow_TM1::kHomeY, 1100ms, false, "");
    return Next(UF_FN(Step6_AtNeutral));
}

StepResult Flow_TM1::Task_Pick::Step6_AtNeutral()
{
    if (!flow().robot_.MoveDone())
    {
        return Stay();
    }
    return Next(UF_FN(Step1_Decide));   // 중립 복귀 완료 -> 다음 결정
}

StepResult Flow_TM1::Task_Pick::Step7_ToNeutral()
{
    flow().fab_.tm1_busy_slot = -1;   // 웨이퍼 들고 슬롯에서 빠짐
    Describe("TM1 pick " + wafer + " -> neutral");
    flow().robot_.StartMove(Flow_TM1::kHomeX, Flow_TM1::kHomeY, 1100ms, true, wafer);
    return Next(UF_FN(Step8_AtNeutral));
}

StepResult Flow_TM1::Task_Pick::Step8_AtNeutral()
{
    if (!flow().robot_.MoveDone())
    {
        return Stay();
    }
    // 들고 중립 도착 -> place task 로 넘김
    return StartTask(flow().task_place_);
}

// ============================================================================
//  Task_Place  (항상 FOUP 로)
// ============================================================================

StepResult Flow_TM1::Task_Place::Step1_ToFoup()
{
    Describe("TM1 place " + flow().robot_.Wafer() + " -> FOUP");
    flow().robot_.StartMove(layout::FoupX(), layout::FoupY(), 1100ms, true,
                            flow().robot_.Wafer());
    return Next(UF_FN(Step2_AtFoup));
}

StepResult Flow_TM1::Task_Place::Step2_AtFoup()
{
    if (!flow().robot_.MoveDone())
    {
        return Stay();
    }
    flow().robot_.DropWafer();
    flow().fab_.foup_count++;   // lock-free 공유 write
    return Next(UF_FN(Step3_ToNeutral));
}

StepResult Flow_TM1::Task_Place::Step3_ToNeutral()
{
    flow().robot_.StartMove(Flow_TM1::kHomeX, Flow_TM1::kHomeY, 1100ms, false, "");
    return Next(UF_FN(Step4_AtNeutral));
}

StepResult Flow_TM1::Task_Place::Step4_AtNeutral()
{
    if (!flow().robot_.MoveDone())
    {
        return Stay();
    }
    return StartTask(flow().task_pick_);   // 다음 pick 결정으로
}
