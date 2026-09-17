//
// LockFree 헤더 리눅스 컴파일 검증 — 4단계(epoll 백엔드) 전까지 서버 본체는 리눅스에서
// 열리지 않는다. 그동안 이 타겟이 "LockFree 헤더가 아직 리눅스에서 열리는가"를 지킨다.
//
//   객체를 만들어보는 것만으로는 실제 호출된 멤버만 인스턴스화된다. 명시적 인스턴스화로
//   모든 멤버 함수를 컴파일시켜야 이식 누락이 드러난다.
//   (CLockFreeQueue는 PlacementNew=true를 static_assert로 막으므로 조합에서 제외)
//
#include "LockFreeConfig.h"

struct LfCheckPayload
{
    int    a;
    double b;
};

template class LockFree::CInternalFreeList<LfCheckPayload, false, true>;
template class LockFree::CInternalFreeList<LfCheckPayload, true,  false>;
template class LockFree::CExternalTlsFreeList<LfCheckPayload>;
template class LockFree::CLockFreeQueue<LfCheckPayload, false, true>;
template class LockFree::CLockFreeStack<LfCheckPayload, true>;
template class LockFree::CLockFreeStack<LfCheckPayload, false>;

int main()
{
    return 0;
}
