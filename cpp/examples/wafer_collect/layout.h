// layout.h - 화면 좌표(px). 실제 장비 배치를 따라감:
//   FOUP | [TM1] | Buffer(세로 8슬롯) | [TM2] | PM1..PM8(2열 4행)
// TM1: FOUP <-> Buffer 담당,  TM2: Buffer <-> PM 담당.
//
// 로봇은 x,y 동시에 움직여서 목표 (x,y)로 곧장 간다(대각선). pick&place처럼 x 갔다
// y 가는 식이 아님.
#pragma once

namespace layout
{
    constexpr int kWinW = 1120;
    constexpr int kWinH = 680;
    constexpr int kMidY = 340;

    constexpr int kBufSlots = 8;
    constexpr int kPMs = 8;

    // FOUP (왼쪽, 세로로 긴 박스). 내려놓는 지점은 박스 오른쪽.
    constexpr int kFoupBoxL = 30;
    constexpr int kFoupBoxR = 118;

    inline double FoupX() { return 140.0; }
    inline double FoupY() { return kMidY; }

    // TM1 로봇 베이스 (FOUP와 Buffer 사이)
    constexpr int kTM1BaseX = 210;
    constexpr int kTM1BaseY = kMidY;

    // Buffer: 세로 8칸
    inline double BufferX() { return 445.0; }
    inline double BufferSlotY(int i) { return 150.0 + i * 58.0; }
    constexpr int kBufCellW = 104;
    constexpr int kBufCellH = 52;

    // TM2 로봇 베이스 (Buffer와 PM 사이)
    constexpr int kTM2BaseX = 635;
    constexpr int kTM2BaseY = kMidY;

    // PM: 2열 x 4행
    inline double PmX(int j) { return (j % 2 == 0) ? 825.0 : 980.0; }
    inline double PmY(int j) { return 180.0 + (j / 2) * 120.0; }
    constexpr int kPmCellW = 120;
    constexpr int kPmCellH = 92;
}
