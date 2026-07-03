// main.cpp - wafer_collect Win32 데모 진입점.
//
// etch 스케줄러의 웨이퍼 회수 기능을 uniflow로 재구성. 실제 시퀀스:
//   레이아웃  FOUP | [TM1] | Buffer(8칸) | [TM2] | PM1..PM8
//   1) collect 호출 -> 웨이퍼 있는 PM 전부에 방출 준비 명령(동시 시작, ~4.5s)
//   2) TM1, TM2 동시에 돌린다(순차 phase 아님). buffer 슬롯 상태로 대화하는 producer/
//      consumer: TM1 = buffer dummy-sweep + drain -> FOUP, TM2 = 준비된 PM -> buffer.
//      각 로봇은 pick/place task 분리라 "웨이퍼 있으면 즉시 뜨고 중립 대기" 가능.
//
// PM 준비 + sweep + PM->buffer + buffer->FOUP 가 한 pump 스레드에서 전부 겹쳐 돈다.
// 원본 코드 매핑 / 왜 헬인지는 README_wafer_collect.md 참고. 창 닫으면 진행 중 캠페인
// 끝내고 종료.
#include "app.h"

#include "viz_env.h"

#include <iostream>

int main()
{
    std::cout << "wafer_collect (uniflow seminar) - close the window to stop.\n";

    App app;
    app.Start();

    RunVisualisation();   // 메인 스레드 Win32 렌더 루프. 창 닫힐 때까지 블록.

    viz::RequestStop();
    app.WaitForDone();

    std::cout << "done. campaigns=" << app.collector.Campaign() << "\n";
    return 0;
}
