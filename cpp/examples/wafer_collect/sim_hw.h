// sim_hw.h - 회수 데모용 가상 로봇. 2D 이동.
//
// 실제 장비랑 같은 규칙: 명령 던지면 바로 리턴, 끝났는지는 폴링. 블록 안 하고
// 스레드도 안 만듬. uniflow pump가 한 스레드에서 다 돌림.
//
// pick&place 예제랑 다르게 x/y 동시에 움직인다(대각선). StartMove(tx,ty)로 목표점
// 주면 X()/Y()가 시간에 따라 선형 보간된 현재 위치를 준다. 눈에 보이게 일부러
// 느리게(이송 ~1.2s, dummy pick 감지 ~0.7s) 돌린다.
#pragma once

#include <chrono>
#include <string>

namespace sim
{
    using Clock = std::chrono::steady_clock;

    class Robot
    {
    public:
        Robot(std::string id, double home_x, double home_y);

        const std::string& Id() const { return id_; }

        // (tx,ty)로 dur 동안 이동. carrying이면 wafer 들고 감(뷰 라벨).
        void StartMove(double tx, double ty, std::chrono::milliseconds dur,
                       bool carrying, std::string wafer);
        // 제자리에서 dur 동안 멈춤(dummy pick 감지 같은 거). 현재 carry 유지.
        void Probe(std::chrono::milliseconds dur);
        bool MoveDone() const;

        double             X() const;
        double             Y() const;
        bool               Carrying() const { return carrying_; }
        const std::string& Wafer() const { return wafer_; }
        void               DropWafer();

    private:
        std::string               id_;
        double                    from_x_ = 0.0;
        double                    from_y_ = 0.0;
        double                    to_x_ = 0.0;
        double                    to_y_ = 0.0;
        double                    cur_x_ = 0.0;
        double                    cur_y_ = 0.0;
        bool                      busy_ = false;
        bool                      carrying_ = false;
        std::string               wafer_;
        Clock::time_point         start_{};
        std::chrono::milliseconds dur_{0};
    };
}
