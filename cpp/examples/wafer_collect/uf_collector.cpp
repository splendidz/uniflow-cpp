// uf_collector.cpp - Flow_Collector step 본문.
#include "uf_collector.h"

#include "viz_env.h"

#include <cstdint>
#include <string>

using namespace uniflow;
using namespace std::chrono_literals;

Flow_Collector::Flow_Collector(uniflow::Runtime& rt, Fab& fab, Flow_TM1& tm1,
                               Flow_TM2& tm2)
    : uniflow::Uniflow<Flow_Collector>(rt, "Collector"), fab_(fab), tm1_(tm1),
      tm2_(tm2)
{
    AddTask(task_campaign_);
}

StepResult Flow_Collector::Task_Campaign::Step1_Seed()
{
    flow().SetPhase("seed");
    std::uint32_t seed = 1234567u + static_cast<std::uint32_t>(campaign) * 99991u;
    flow().fab_.Seed(seed);
    Describe("seed campaign " + std::to_string(campaign + 1));
    return Next(UF_FN(Step2_FirePmCollect));
}

// PM 전부에 collect 명령. 방출 준비 ~4.5s, 전부 동시 시작. 그동안 TM1이 buffer sweep
// 하니까 준비가 sweep 이랑 겹쳐 돈다.
StepResult Flow_Collector::Task_Campaign::Step2_FirePmCollect()
{
    flow().SetPhase("fire PM collect (COLLECT_PROC ~4.5s, all concurrent)");
    int fired = 0;
    for (int j = 0; j < Fab::kPM; ++j)
    {
        if (flow().fab_.pm[j].has)
        {
            flow().fab_.pm[j].StartCollect(4500ms);
            fired++;
        }
    }
    Describe("collect cmd -> " + std::to_string(fired) + " PMs");
    return Next(UF_FN(Step3_StartRobots));
}

// TM1, TM2 동시 기동. 여기부터는 둘이 알아서 buffer 상태로 협조.
StepResult Flow_Collector::Task_Campaign::Step3_StartRobots()
{
    flow().SetPhase("collecting: TM1 sweep+drain / TM2 PM->buffer (concurrent)");
    Describe("start TM1 + TM2 (concurrent)");
    flow().tm1_.task_pick_.StartFlow();
    flow().tm2_.task_pick_.StartFlow();
    return Next(UF_FN(Step4_WaitBoth));
}

StepResult Flow_Collector::Task_Campaign::Step4_WaitBoth()
{
    if (!flow().tm1_.IsIdle() || !flow().tm2_.IsIdle())
    {
        return Stay();
    }
    return Next(UF_FN(Step5_Verify));
}

StepResult Flow_Collector::Task_Campaign::Step5_Verify()
{
    campaign++;
    const bool clean =
        flow().fab_.OccupiedCount() == 0 && flow().fab_.PmWafers() == 0
        && flow().fab_.FirstUnknown() < 0;
    flow().SetPhase(clean ? "campaign done - all collected" : "campaign incomplete");
    Describe("campaign " + std::to_string(campaign) + (clean ? " done" : " INCOMPLETE"));
    if (viz::Stop())
    {
        return Done();
    }
    return Next(UF_FN(Step6_Pause));
}

StepResult Flow_Collector::Task_Campaign::Step6_Pause()
{
    if (viz::Stop())
    {
        return Done();
    }
    flow().SetPhase("next campaign in...");
    return StayTimeout(1000ms, UF_FN(Step1_Seed));
}
