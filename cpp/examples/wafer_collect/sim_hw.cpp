// sim_hw.cpp - 2D 로봇 구현. 모델은 sim_hw.h 참고.
#include "sim_hw.h"

#include <algorithm>

namespace sim
{
    Robot::Robot(std::string id, double home_x, double home_y)
        : id_(std::move(id)), from_x_(home_x), from_y_(home_y), to_x_(home_x),
          to_y_(home_y), cur_x_(home_x), cur_y_(home_y)
    {
    }

    void Robot::StartMove(double tx, double ty, std::chrono::milliseconds dur,
                          bool carrying, std::string wafer)
    {
        from_x_   = cur_x_;
        from_y_   = cur_y_;
        to_x_     = tx;
        to_y_     = ty;
        dur_      = dur;
        start_    = Clock::now();
        busy_     = true;
        carrying_ = carrying;
        wafer_    = std::move(wafer);
    }

    void Robot::Probe(std::chrono::milliseconds dur)
    {
        // 제자리 멈춤 - carry 상태는 안 건드림.
        from_x_ = cur_x_;
        from_y_ = cur_y_;
        to_x_   = cur_x_;
        to_y_   = cur_y_;
        dur_    = dur;
        start_  = Clock::now();
        busy_   = true;
    }

    bool Robot::MoveDone() const
    {
        if (!busy_)
        {
            return true;
        }
        if ((Clock::now() - start_) < dur_)
        {
            return false;
        }
        auto* self   = const_cast<Robot*>(this);
        self->cur_x_ = to_x_;
        self->cur_y_ = to_y_;
        self->busy_  = false;
        return true;
    }

    double Robot::X() const
    {
        if (!busy_ || dur_.count() == 0)
        {
            return cur_x_;
        }
        double frac =
            std::chrono::duration<double>(Clock::now() - start_).count()
            / std::chrono::duration<double>(dur_).count();
        frac = std::clamp(frac, 0.0, 1.0);
        return from_x_ + (to_x_ - from_x_) * frac;
    }

    double Robot::Y() const
    {
        if (!busy_ || dur_.count() == 0)
        {
            return cur_y_;
        }
        double frac =
            std::chrono::duration<double>(Clock::now() - start_).count()
            / std::chrono::duration<double>(dur_).count();
        frac = std::clamp(frac, 0.0, 1.0);
        return from_y_ + (to_y_ - from_y_) * frac;
    }

    void Robot::DropWafer()
    {
        carrying_ = false;
        wafer_.clear();
    }
}
