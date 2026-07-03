// uf_collector.h - 회수 오케스트레이터. 예전 코드엔 없던 "단일 주인".
//
// 캠페인:
//   1) 초기 상태 seed
//   2) 웨이퍼 있는 PM 전부에 collect 명령(방출 준비 ~4.5s, 다 동시에 시작)
//   3) TM1, TM2 를 "동시에" 기동. 둘이 buffer 슬롯 상태(fab)로 대화하며 협조:
//        TM1 = buffer sweep(dummy pick) + TM2가 채운 것 drain -> FOUP
//        TM2 = 준비된 PM -> buffer 빈 칸
//      (예전처럼 phase 순차가 아니라 한 pump 스레드에서 producer/consumer 로 겹쳐 돎)
//   4) 둘 다 끝나면 검증, (GUI라) 다음 캠페인 반복
//
// 예전의 5개 회수 서브시스템(deadlock/HMER/ADE/hotlot/interlock)은 worklist 정책만
// 다른 같은 캠페인으로 환원. 대체 대상: ScdApp::WaferCollect(:1111) + Sleep 루프,
// VTRScheduler::TryNextAction 에 흩어진 캠페인.
#pragma once

#include "fab.h"
#include "uf_tm1.h"
#include "uf_tm2.h"
#include "uniflow.hpp"

class Flow_Collector : public uniflow::Uniflow<Flow_Collector>
{
public:
    Flow_Collector(uniflow::Runtime& rt, Fab& fab, Flow_TM1& tm1, Flow_TM2& tm2);

    struct Task_Campaign : uniflow::Task<Flow_Collector>
    {
        int campaign = 0;

        StepResult Entry() override { return Step1_Seed(); }

    private:
        StepResult Step1_Seed();
        StepResult Step2_FirePmCollect();  // PM들에 collect 명령(동시 준비)
        StepResult Step3_StartRobots();    // TM1/TM2 동시 기동
        StepResult Step4_WaitBoth();       // 둘 다 idle 될 때까지
        StepResult Step5_Verify();
        StepResult Step6_Pause();
    } task_campaign_;

    int                Campaign() const { return task_campaign_.campaign; }
    const std::string& Phase() const { return phase_; }
    void               SetPhase(const char* p) { phase_ = p; }

private:
    Fab&        fab_;
    Flow_TM1&   tm1_;
    Flow_TM2&   tm2_;
    std::string phase_ = "-";
};
