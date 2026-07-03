// fab.h - 장비 물리 상태(공유). TM1 flow랑 TM2 flow가 이거 하나를 mutex 없이 같이
// 읽고 쓴다. 같은 uniflow pump 스레드에서 도니까 lock 필요 없음.
//
// 이 파일이 회수의 lock-free producer/consumer 조율판이다:
//  - TM1(consumer): buffer 슬롯을 dummy pick 으로 훑어(Unknown->Empty) FOUP으로 빼고,
//    TM2가 채운 슬롯(Occupied)도 FOUP으로 뺀다.
//  - TM2(producer): 준비된 PM 웨이퍼를 비어있는(Empty) buffer 슬롯으로 옮긴다.
//  둘이 buffer 슬롯 상태로만 대화한다. 예전 스케줄러였으면 m_v* 플래그 다발 +
//  SCLock 이었을 자리.
//
// 포인트:
//  - Buffer에는 센서가 없다. 슬롯 상태가 Unknown이면 TM1이 dummy pick 해봐야 안다.
//  - PM은 collect 명령 받고 방출 준비(~4.5s) 끝나야(ready) TM2가 가져갈 수 있다.
//  - 안전: TM2가 슬롯에 내려놓자마자 TM1이 집으면 위험. TM2가 완전히 빠져나가면
//    (retract 완료) 그 슬롯 safe=true 가 되고, 그때만 TM1이 가져간다.
#pragma once

#include <chrono>
#include <cstdint>
#include <random>
#include <string>

enum class SlotState
{
    Unknown,    // TM1이 아직 안 떠봄
    Empty,      // 확인된 빈 칸 (TM2가 채울 수 있음)
    Occupied    // 웨이퍼 있음(아는 것) (TM1이 뺄 수 있음)
};

struct FabSlot
{
    SlotState   state = SlotState::Unknown;
    bool        has_phys = false;   // 물리적으로 웨이퍼가 실제로 있나
    std::string wafer;
    bool        safe = false;       // TM2가 놓고 완전히 빠져나감 -> TM1이 가져가도 됨

    void seed(std::string w)
    {
        state    = SlotState::Unknown;
        has_phys = true;
        wafer    = std::move(w);
        safe     = false;
    }
    void clearEmpty()
    {
        state    = SlotState::Empty;
        has_phys = false;
        safe     = false;
        wafer.clear();
    }
};

// PM(챔버). collect 명령 받으면 방출 준비(~4.5s) 후 ready.
struct Chamber
{
    using Clock = std::chrono::steady_clock;

    bool        has = false;
    std::string wafer;
    bool        preparing = false;
    bool        ready = false;

    Clock::time_point         start{};
    std::chrono::milliseconds dur{0};

    void set(std::string w)
    {
        has   = true;
        wafer = std::move(w);
    }
    void clear()
    {
        has       = false;
        preparing = false;
        ready     = false;
        wafer.clear();
    }

    void StartCollect(std::chrono::milliseconds d)
    {
        if (!has)
        {
            return;
        }
        preparing = true;
        ready     = false;
        start     = Clock::now();
        dur       = d;
    }

    bool Ready() const
    {
        if (!has)
        {
            return false;
        }
        if (ready)
        {
            return true;
        }
        if (preparing && (Clock::now() - start) >= dur)
        {
            auto* self      = const_cast<Chamber*>(this);
            self->ready     = true;
            self->preparing = false;
        }
        return ready;
    }

    // 방출 준비 남은 시간(ms). 뷰에서 카운트다운 표시용.
    long long RemainingMs() const
    {
        if (!has || ready || !preparing)
        {
            return 0;
        }
        auto left = dur - (Clock::now() - start);
        auto ms   = std::chrono::duration_cast<std::chrono::milliseconds>(left).count();
        return ms < 0 ? 0 : ms;
    }
};

struct Fab
{
    static constexpr int kBuf = 8;
    static constexpr int kPM = 8;

    FabSlot buffer[kBuf];
    Chamber pm[kPM];
    int     foup_count = 0;
    bool    tm2_done = false;   // TM2가 PM 다 옮기고 종료함(TM1 종료 판단에 씀)

    // 지금 각 로봇이 잡고 있는 buffer 슬롯(-1=버퍼에 없음). 마진 계산용.
    // TM1이 slot i 를 잡고 있으면 TM2는 i, i+-1 은 못 쓰고 최소 2칸 떨어져야 함.
    int tm1_busy_slot = -1;
    int tm2_busy_slot = -1;

    static bool FarEnough(int a, int other, int margin)
    {
        if (other < 0)
        {
            return true;
        }
        int d = a - other;
        if (d < 0)
        {
            d = -d;
        }
        return d >= margin;
    }
    // other 슬롯에서 margin 이상 떨어진 첫 Unknown / Occupied+safe / Empty 슬롯.
    int FirstUnknownAway(int other, int margin = 2) const
    {
        for (int i = 0; i < kBuf; ++i)
        {
            if (buffer[i].state == SlotState::Unknown && FarEnough(i, other, margin))
            {
                return i;
            }
        }
        return -1;
    }
    int FirstOccupiedSafeAway(int other, int margin = 2) const
    {
        for (int i = 0; i < kBuf; ++i)
        {
            if (buffer[i].state == SlotState::Occupied && buffer[i].safe
                && FarEnough(i, other, margin))
            {
                return i;
            }
        }
        return -1;
    }
    int FirstEmptyAway(int other, int margin = 2) const
    {
        for (int i = 0; i < kBuf; ++i)
        {
            if (buffer[i].state == SlotState::Empty && FarEnough(i, other, margin))
            {
                return i;
            }
        }
        return -1;
    }

    int FirstUnknown() const
    {
        for (int i = 0; i < kBuf; ++i)
        {
            if (buffer[i].state == SlotState::Unknown)
            {
                return i;
            }
        }
        return -1;
    }
    int FirstOccupiedSafe() const
    {
        for (int i = 0; i < kBuf; ++i)
        {
            if (buffer[i].state == SlotState::Occupied && buffer[i].safe)
            {
                return i;
            }
        }
        return -1;
    }
    int FirstEmpty() const
    {
        for (int i = 0; i < kBuf; ++i)
        {
            if (buffer[i].state == SlotState::Empty)
            {
                return i;
            }
        }
        return -1;
    }

    int OccupiedCount() const
    {
        int n = 0;
        for (int i = 0; i < kBuf; ++i)
        {
            if (buffer[i].state == SlotState::Occupied)
            {
                ++n;
            }
        }
        return n;
    }
    int PmWafers() const
    {
        int n = 0;
        for (int j = 0; j < kPM; ++j)
        {
            if (pm[j].has)
            {
                ++n;
            }
        }
        return n;
    }
    bool HasEmpty() const { return FirstEmpty() >= 0; }

    void Reset()
    {
        for (int i = 0; i < kBuf; ++i)
        {
            buffer[i] = FabSlot{};
        }
        for (int j = 0; j < kPM; ++j)
        {
            pm[j].clear();
        }
        foup_count    = 0;
        tm2_done      = false;
        tm1_busy_slot = -1;
        tm2_busy_slot = -1;
    }

    // 초기 상태 랜덤 seed. buffer/PM에 제법 많이 깔아둠.
    void Seed(std::uint32_t seed)
    {
        Reset();
        std::mt19937                       rng(seed);
        std::uniform_int_distribution<int> coin(0, 99);
        for (int i = 0; i < kBuf; ++i)
        {
            if (coin(rng) < 55)
            {
                buffer[i].seed("B" + std::to_string(i + 1));
            }
        }
        for (int j = 0; j < kPM; ++j)
        {
            if (coin(rng) < 60)
            {
                pm[j].set("P" + std::to_string(j + 1));
            }
        }
    }
};
