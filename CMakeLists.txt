cmake_minimum_required(VERSION 3.20)
project(MMO LANGUAGES CXX)

# C++17 (기존 vcxproj: stdcpp17)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 단일구성 제너레이터(Ninja 등) 기본 Release — 기존 검증 기준
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# MySQL 8.0 설치 경로 (필요 시 -DMYSQL_DIR=... 로 override)
set(MYSQL_DIR "C:/Program Files/MySQL/MySQL Server 8.0" CACHE PATH "MySQL install dir")

set(SRV_DIR   ${CMAKE_CURRENT_SOURCE_DIR}/MMOServer/MMOServer)   # 게임 계층
set(CORE_BASE ${CMAKE_CURRENT_SOURCE_DIR}/ServerCore/Base)       # 토대 계층
set(CORE_NET  ${CMAKE_CURRENT_SOURCE_DIR}/ServerCore/Network)    # 전송 계층

# ─────────────────────────────────────────────────────────────
#  LockFree 저장소 위치 — 형제 저장소(github.com/cocoz93/LockFree)
#    소스에 상대경로를 박지 않는다. 박으면 폴더 배치가 코드에 들어가서
#    옮기는 순간 C1083으로 죽고, 원인이 에러 메시지에 안 보인다.
#    다른 자리에 두려면 -DLOCKFREE_DIR=<경로> 로 넘길 것.
# ─────────────────────────────────────────────────────────────
set(LOCKFREE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../LockFree/LockFree_Test"
    CACHE PATH "LockFree 저장소의 LockFree_Test 디렉터리")

if(NOT EXISTS "${LOCKFREE_DIR}/LockFree/LockFreeQueue.h")
    message(FATAL_ERROR
        "LockFree 저장소를 찾을 수 없습니다: ${LOCKFREE_DIR}\n"
        "  git clone https://github.com/cocoz93/LockFree  (이 저장소와 나란히)\n"
        "  또는 -DLOCKFREE_DIR=<LockFree_Test 경로>")
endif()

# DB 워커는 MySQL 서버가 있어야 뜬다. 4단계(네트워크 뼈대) 검증에는 불필요하므로
#   리눅스 타겟에서는 기본 꺼 둔다. 5단계 실동작에서 -DMMO_USE_DB=ON으로 켠다.
#   ※ 라이브러리 정의보다 먼저 와야 한다 — 매크로를 세 타겟에 똑같이 걸어야 ODR이 깨지지 않는다.
option(MMO_USE_DB "리눅스 서버에서 DB 워커 사용" OFF)

# ─────────────────────────────────────────────────────────────
#  플랫폼 전역 옵션 — 타겟 정의보다 먼저
#    add_compile_options는 "이후 정의되는" 타겟에만 적용된다. 라이브러리를 아래에
#    정의하므로 여기서 걸어야 세 타겟이 같은 플래그로 컴파일된다.
# ─────────────────────────────────────────────────────────────
if(UNIX)
    # 16바이트 CAS(cmpxchg16b) — LockFree의 128비트 CAS는 이 플래그가 없으면 인라인되지 않고
    #   libatomic 호출로 내려간다. LockFreeCompat.h가 #error로 막아 두긴 했지만, 여기서
    #   전역으로 걸어야 앞으로 추가될 타겟(epoll 백엔드 등)이 빠뜨릴 일이 없다.
    #   (libatomic 링크는 불필요 — __sync 경로가 인라인이라 외부 호출이 남지 않는다)
    add_compile_options(-mcx16)

    # backtrace_symbols_fd가 함수명을 찍으려면 실행 파일의 심볼이 동적 심볼 테이블에 있어야 한다.
    #   기본 링크는 실행 파일 심볼을 export하지 않아, 크래시 로그가 주소만 남고 이름이 사라진다.
    #   (Windows는 PDB를 읽어 해결하므로 대응하는 설정이 없다)
    add_link_options(-rdynamic)
endif()

# ═════════════════════════════════════════════════════════════
#  ServerBaseLib — 토대 계층 (직렬화 버퍼 · 락프리 설정 · 코어 친화도 · 로거)
#    게임도 네트워크도 모른다. 위 두 계층이 공통으로 딛는 바닥이다.
#    컴파일 설정을 PUBLIC으로 거는 이유 — 소비자 타겟에 그대로 전파되어야
#    매크로가 갈리지 않는다(갈리면 구조체 레이아웃이 달라져 조용히 깨진다).
# ═════════════════════════════════════════════════════════════
add_library(ServerBaseLib STATIC
    ${CORE_BASE}/SerialBuffer.cpp
    ${CORE_BASE}/LockFreeConfig.cpp
    ${CORE_BASE}/CoreAffinity.cpp
    ${CORE_BASE}/Common/Logger.cpp
)

target_include_directories(ServerBaseLib PUBLIC
    ${CORE_BASE}               # <RingBuffer.h> · <Common/…> · "Platform/Platform.h"
    ${SRV_DIR}/ThirdParty      # spdlog·fmt·httplib (헤더온리 번들 — git 에 없음)
    ${LOCKFREE_DIR}            # 형제 저장소 — <LockFree/…> 로 열린다
)

target_compile_features(ServerBaseLib PUBLIC cxx_std_17)

target_compile_definitions(ServerBaseLib PUBLIC
    USE_SPDLOG_LOGGER
)

if(WIN32)
    target_compile_definitions(ServerBaseLib PUBLIC
        _CONSOLE
        UNICODE
        _UNICODE
    )
endif()

if(MSVC)
    # 공통: UTF-8 소스, 표준 준수(/permissive-), SDL, 다중 컴파일
    target_compile_options(ServerBaseLib PUBLIC /utf-8 /permissive- /sdl /MP)
    # Release: 함수단위링크(/Gy)·인트린식(/Oi)·전체프로그램최적화(/GL)
    #   ※ PUBLIC이어야 라이브러리 경계를 넘어 인라인이 유지된다. PRIVATE으로 두면
    #     빌드는 되는데 경계에서 인라인이 끊겨 수치만 조용히 나빠진다.
    target_compile_options(ServerBaseLib PUBLIC $<$<CONFIG:Release>:/Gy /Oi /GL>)
endif()

if(UNIX)
    # DB 워커 스위치는 헤더에서 분기하므로 세 타겟에 똑같이 걸려야 한다(PUBLIC)
    target_compile_definitions(ServerBaseLib PUBLIC
        $<$<NOT:$<BOOL:${MMO_USE_DB}>>:USE_DB_WORKER=0>
    )
    target_link_libraries(ServerBaseLib PUBLIC pthread)
endif()

# ═════════════════════════════════════════════════════════════
#  ServerNetworkLib — 전송 계층 (수용·세션·전송 팔)
#    게임 로직을 모른다. 와이어 헤더(NetHeader.h)까지만 안다.
#    전송 팔만 플랫폼별로 갈린다 — Windows는 IOCP/RIO, Linux는 epoll.
# ═════════════════════════════════════════════════════════════
add_library(ServerNetworkLib STATIC
    ${CORE_NET}/IOCPServer.cpp
)

if(WIN32)
    target_sources(ServerNetworkLib PRIVATE
        ${CORE_NET}/Transport_Iocp.cpp   # 완료포트 팔 (RIO 빌드에서는 빈 번역단위)
        ${CORE_NET}/Transport_Rio.cpp    # RIO 팔 (IOCP 빌드에서는 빈 번역단위)
    )
else()
    target_sources(ServerNetworkLib PRIVATE
        ${CORE_NET}/Transport_Epoll.cpp
    )
endif()

target_include_directories(ServerNetworkLib PUBLIC
    ${CORE_NET}                          # "IOCPServer.h" · "RioApi.h" · "NetIoModel.h"
    ${CMAKE_CURRENT_SOURCE_DIR}/Shared   # <Protocol/NetHeader.h> — 와이어 규약은 클라와 공유
    ${SRV_DIR}                           # "BuildConfig.h" — 아직 게임 쪽에 있다(미결, 아래 주석)
)

# ⚠ 계층 분리가 여기서 하나 덜 됐다 — ServerNetworkLib 이 ${SRV_DIR}(게임)을 본다.
#   IOCPServer.h:3 이 BuildConfig.h 를 include 하는데, 그 파일에 코어 토글
#   (USE_LOCKFREE_SENDQ·USE_SEND_COALESCING)과 게임 실험 토글(USE_MEMBERSHIP_DIGEST 등)이
#   섞여 있다. 가르려면 A/B 실측 사슬을 건드려야 해서 승격(4단계) 때 함께 정한다.

target_link_libraries(ServerNetworkLib PUBLIC ServerBaseLib)

# ═════════════════════════════════════════════════════════════
#  MMOServer / mmo_server — 게임 계층 실행 파일
#    이름을 플랫폼별로 가른 채 유지한다. Run/bin 의 exe 이름과 위치에
#    실행용 배치 15개가 걸려 있어서, 통일하면 그것들이 한꺼번에 깨진다.
# ═════════════════════════════════════════════════════════════
set(GAME_SOURCES
    ${SRV_DIR}/main.cpp
    ${SRV_DIR}/GameServer.cpp
    ${SRV_DIR}/MapManager.cpp
    ${SRV_DIR}/SectorManager.cpp
    ${SRV_DIR}/Zone.cpp
    ${SRV_DIR}/Player.cpp
    ${SRV_DIR}/DB/DBWorker.cpp
)

if(WIN32)

add_executable(MMOServer ${GAME_SOURCES})

target_link_libraries(MMOServer PRIVATE ServerNetworkLib)

target_include_directories(MMOServer PRIVATE
    ${MYSQL_DIR}/include       # MySQL C API (DBWorker 전용)
)

if(MSVC)
    # 링크 옵션은 기존 MMOServer.vcxproj 가 내던 것과 같게 맞춰 둔다 — 실측 기준선이
    #   그 바이너리로 쌓여 있어서다. 2026-09-17 실측으로 확인한 두 가지:
    #
    #   /OPT:REF·/OPT:ICF 를 켜지 않는다 — 켜면 죽은 코드가 빠져 exe가 638KB 작아지지만
    #     (799,232 vs 1,477,632), 기존 vcxproj는 `/DEBUG` 탓에 링커가 이 둘을 자동으로 꺼 왔다.
    #     서버 한 대에서 도는 물건이라 크기 이득이 없다. 성능을 재 보고 싶으면 그때 A/B로.
    #
    #   /LTCG 가 아니라 /LTCG:incremental — 이 한 글자가 40,448B 를 갈랐다.
    #     기존 vcxproj가 증분 LTCG를 쓰고 있었고, 맞추니 차이가 512B(0.036%)로 줄었다.
    #     남은 512B는 lib 3개로 나눈 것 자체의 영향(분할 전후 비교에서도 같은 512B).
    target_link_options(MMOServer PRIVATE /SUBSYSTEM:CONSOLE /DEBUG
                        $<$<CONFIG:Release>:/LTCG:incremental>)
    # libmysql.lib 는 소스의 #pragma comment(lib)로 자동 링크 → lib 디렉토리만 링크경로에
    target_link_directories(MMOServer PRIVATE ${MYSQL_DIR}/lib)
endif()

# 출력물을 기존 .sln과 동일하게 Run/bin 에 (런타임 DLL·INI가 거기 있음)
set_target_properties(MMOServer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY         ${CMAKE_CURRENT_SOURCE_DIR}/Run/bin
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/Run/bin
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   ${CMAKE_CURRENT_SOURCE_DIR}/Run/bin
)
endif()  # WIN32

if(UNIX)

add_executable(mmo_server ${GAME_SOURCES})

target_link_libraries(mmo_server PRIVATE ServerNetworkLib mysqlclient)

target_include_directories(mmo_server PRIVATE
    /usr/include/mysql
)

set_target_properties(mmo_server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/Run/bin)

# ─────────────────────────────────────────────────────────────
#  epoll 에코 PoC (Linux) — Phase 2 툴체인/이벤트 루프 검증용
#  게임서버 본체와 무관한 독립 실행파일 (LockFree 포팅 전 단계)
# ─────────────────────────────────────────────────────────────
add_executable(epoll_echo_poc
    ${SRV_DIR}/poc/epoll_echo_main.cpp
)
target_compile_features(epoll_echo_poc PRIVATE cxx_std_17)
set_target_properties(epoll_echo_poc PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/Run/bin)

# LockFree 헤더 리눅스 컴파일 검증 — 서버 본체가 리눅스에서 열리기 전(4단계)까지
#   이식 상태를 지키는 회귀 방지 타겟. 헤더는 형제 저장소를 본다(LockFreeConfig.h).
add_executable(lockfree_headers_check
    ${SRV_DIR}/poc/lockfree_headers_check.cpp
    ${CORE_BASE}/LockFreeConfig.cpp
)
target_include_directories(lockfree_headers_check PRIVATE
    ${CORE_BASE}
    ${SRV_DIR}/ThirdParty
    ${LOCKFREE_DIR}
)
target_compile_features(lockfree_headers_check PRIVATE cxx_std_17)
set_target_properties(lockfree_headers_check PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/Run/bin)

endif()  # UNIX
