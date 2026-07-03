// uf_viz.cpp - pump측 스냅샷 writer.
#include "uf_viz.h"

#include "viz_env.h"
#include "viz_state.h"

#include <mutex>

using namespace uniflow;

Flow_Viz::Flow_Viz(uniflow::Runtime& rt, Flow_Collector& col, Flow_TM1& tm1,
                   Flow_TM2& tm2, Fab& fab)
    : uniflow::Uniflow<Flow_Viz>(rt, "Viz"), col_(col), tm1_(tm1), tm2_(tm2),
      fab_(fab)
{
    AddTask(task_snapshot_);
}

StepResult Flow_Viz::Task_Snapshot::Step1_Tick()
{
    if (viz::Stop())
    {
        return Done();
    }
    Flow_Collector& col = flow().col_;
    Flow_TM1&       tm1 = flow().tm1_;
    Flow_TM2&       tm2 = flow().tm2_;
    Fab&            fab = flow().fab_;
    {
        std::lock_guard<std::mutex> lk(g_snap_mu);
        g_snap.tm1_x     = tm1.X();
        g_snap.tm1_y     = tm1.Y();
        g_snap.tm1_carry = tm1.Carrying();
        g_snap.tm1_wafer = tm1.Wafer();
        g_snap.tm1_phase = tm1.CurrentStepDescription();

        g_snap.tm2_x     = tm2.X();
        g_snap.tm2_y     = tm2.Y();
        g_snap.tm2_carry = tm2.Carrying();
        g_snap.tm2_wafer = tm2.Wafer();
        g_snap.tm2_phase = tm2.CurrentStepDescription();

        for (int i = 0; i < Fab::kBuf; ++i)
        {
            g_snap.buffer[i].state    = fab.buffer[i].state;
            g_snap.buffer[i].has_phys = fab.buffer[i].has_phys;
            g_snap.buffer[i].wafer    = fab.buffer[i].wafer;
        }
        for (int j = 0; j < Fab::kPM; ++j)
        {
            bool ready                = fab.pm[j].Ready();   // latch 겸
            g_snap.pm[j].has          = fab.pm[j].has;
            g_snap.pm[j].wafer        = fab.pm[j].wafer;
            g_snap.pm[j].ready        = ready;
            g_snap.pm[j].preparing    = fab.pm[j].preparing;
            g_snap.pm[j].remaining_ms = fab.pm[j].RemainingMs();
        }

        g_snap.foup_count = fab.foup_count;
        g_snap.phase      = col.Phase();
    }
    return Stay();
}
