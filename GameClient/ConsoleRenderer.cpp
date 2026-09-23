#include "ConsoleRenderer.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cmath>

// 콘솔에서 2셀을 차지하는 fullwidth 문자 판정 (한글·CJK 등)
static bool IsFullWidth(wchar_t ch)
{
    if (ch >= 0x1100 && ch <= 0x115F) return true;  // Hangul Jamo
    if (ch >= 0x2E80 && ch <= 0x33BF) return true;  // CJK 기호·호환
    if (ch >= 0x3400 && ch <= 0x4DBF) return true;  // CJK 확장 A
    if (ch >= 0x4E00 && ch <= 0x9FFF) return true;  // CJK 통합 한자
    if (ch >= 0xAC00 && ch <= 0xD7AF) return true;  // 한글 음절
    if (ch >= 0xF900 && ch <= 0xFAFF) return true;  // CJK 호환 한자
    if (ch >= 0xFF01 && ch <= 0xFF60) return true;  // 전각 형태
    if (ch >= 0xFFE0 && ch <= 0xFFE6) return true;  // 전각 부호
    return false;
}

// wchar_t 문자열의 콘솔 표시 폭 (셀 수) 계산
static int GetDisplayWidth(const wchar_t* text, int charLen)
{
    int width = 0;
    for (int i = 0; i < charLen; ++i)
        width += IsFullWidth(text[i]) ? 2 : 1;
    return width;
}

// colorIndex(0-6) → 콘솔 색상 속성 변환
static WORD GetColorFromIndex(uint8_t colorIndex)
{
    static constexpr WORD COLORS[] =
    {
        FOREGROUND_GREEN | FOREGROUND_INTENSITY,                            // 밝은 초록
        FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY,           // 밝은 노랑
        FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY,          // 밝은 청록
        FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY,            // 밝은 자홍
        FOREGROUND_RED | FOREGROUND_INTENSITY,                              // 밝은 빨강
        FOREGROUND_BLUE | FOREGROUND_INTENSITY,                             // 밝은 파랑
        FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY, // 밝은 흰색
    };
    return COLORS[colorIndex % 7];
}

CConsoleRenderer::CConsoleRenderer()
    : _hConsole(INVALID_HANDLE_VALUE)
{
    memset(_viewBuffer, 0, sizeof(_viewBuffer));
}

CConsoleRenderer::~CConsoleRenderer()
{
}

void CConsoleRenderer::SetZoneInfo(int mapId, int channelIndex)
{
    _mapId = mapId;
    _channelIndex = channelIndex;
}

void CConsoleRenderer::SetMapSize(int width, int height)
{
    _mapWidth = width;
    _mapHeight = height;
}

void CConsoleRenderer::SetSectorSize(int size)
{
    _sectorSize = size;
}

void CConsoleRenderer::Init()
{
    _hConsole = GetStdHandle(STD_OUTPUT_HANDLE);

    // 커서 숨기기
    CONSOLE_CURSOR_INFO cursorInfo;
    cursorInfo.dwSize = 1;
    cursorInfo.bVisible = FALSE;
    SetConsoleCursorInfo(_hConsole, &cursorInfo);

    // 콘솔 크기 설정 (채팅 입력줄 + 여유 1행)
    static constexpr SHORT CONSOLE_HEIGHT = CHAT_INPUT_ROW + 2; // 49

    // VT100 이스케이프 시퀀스로 창 크기 조정 (Windows Terminal 대응)
    // \x1b[8;rows;colst — 터미널 창을 지정 크기로 리사이즈
    DWORD mode;
    GetConsoleMode(_hConsole, &mode);
    SetConsoleMode(_hConsole, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    char vtResize[32];
    sprintf_s(vtResize, "\x1b[8;%d;%dt", CONSOLE_HEIGHT, CONSOLE_WIDTH);
    WriteConsoleA(_hConsole, vtResize, static_cast<DWORD>(strlen(vtResize)), nullptr, nullptr);

    // 버퍼 크기도 맞춤 (레거시 conhost 폴백)
    COORD bufferSize = { CONSOLE_WIDTH, CONSOLE_HEIGHT };
    SetConsoleScreenBufferSize(_hConsole, bufferSize);

    // 화면 초기화
    DWORD written;
    COORD origin = { 0, 0 };
    FillConsoleOutputCharacterW(_hConsole, L' ', CONSOLE_WIDTH * CONSOLE_HEIGHT, origin, &written);
    FillConsoleOutputAttribute(_hConsole, COLOR_DEFAULT, CONSOLE_WIDTH * CONSOLE_HEIGHT, origin, &written);
}

// ==========================================================================
// 매 프레임 전체 화면 갱신
// ==========================================================================

void CConsoleRenderer::RenderFrame(const ClientPlayer* me,
                                   const std::unordered_map<int32_t, ClientPlayer>& others,
                                   const std::vector<std::wstring>& chatLog,
                                   const std::wstring& chatInput,
                                   bool chatMode,
                                   bool zoneChanging)
{
    RenderStatusBar(me);
    RenderHelpBar();
    if (zoneChanging)
        RenderZoneTransition();
    else
        RenderGameView(me, others);
    RenderChatArea(chatLog);
    RenderChatInput(chatInput, chatMode);
}

// ==========================================================================
// 상태바 (Row 0)
// ==========================================================================

void CConsoleRenderer::RenderStatusBar(const ClientPlayer* me)
{
    wchar_t buf[CONSOLE_WIDTH + 1];

    if (me)
    {
        const wchar_t* stateStr = (me->moveState == MoveState::MOVING) ? L"MOVING" : L"IDLE";
        const wchar_t* dirStr = L"";
        switch (me->direction)
        {
        case Direction::UP:    dirStr = L"UP";    break;
        case Direction::DOWN:  dirStr = L"DOWN";  break;
        case Direction::LEFT:  dirStr = L"LEFT";  break;
        case Direction::RIGHT: dirStr = L"RIGHT"; break;
        default:               dirStr = L"-";     break;
        }

        // ★ swprintf_s 가 아니라 _TRUNCATE 다 — 넘치면 자르고 만다.
        //   Lv 를 끼우면서 한 줄이 80칸에 더 가까워졌고, 캐릭터 번호가 커지면(지금도 만 단위)
        //   swprintf_s 는 잘라 주는 대신 잘못된 인자 핸들러를 불러 프로그램을 끝낸다.
        _snwprintf_s(buf, CONSOLE_WIDTH + 1, _TRUNCATE,
            L" Zone:%d-%d  Lv:%-2d  Player:%-4d  Pos:(%6.1f,%6.1f)  Speed:%-3d  [%s %s]",
            _mapId, _channelIndex, me->level, me->playerId, me->x, me->y, me->speed, stateStr, dirStr);
    }
    else
    {
        swprintf_s(buf, CONSOLE_WIDTH + 1, L" Waiting for server...");
    }

    // 나머지를 공백으로 채우기
    size_t len = wcslen(buf);
    for (size_t i = len; i < CONSOLE_WIDTH; ++i)
        buf[i] = L' ';
    buf[CONSOLE_WIDTH] = L'\0';

    WriteTextAt(0, STATUS_ROW, buf, COLOR_STATUS, CONSOLE_WIDTH);
}

// ==========================================================================
// 조작 안내 (Row 1)
// ==========================================================================

void CConsoleRenderer::RenderHelpBar()
{
    WriteTextAt(0, HELP_ROW, L" Arrow:Move  Enter:Chat  ESC:Quit", COLOR_BORDER, CONSOLE_WIDTH);
}

// ==========================================================================
// 게임 뷰 (Row 2~22) — WriteConsoleOutputW 단일 호출
// ==========================================================================

void CConsoleRenderer::RenderGameView(const ClientPlayer* me,
                                      const std::unordered_map<int32_t, ClientPlayer>& others)
{
    // 뷰포트 원점 (월드 좌표)
    float camX = 0.0f, camY = 0.0f;
    if (me)
    {
        camX = me->x - VIEW_WIDTH / 2;
        camY = me->y - VIEW_HEIGHT / 2;

        // 카메라 클램핑: 맵 밖이 보이지 않도록 제한 (-1은 경계벽 표시 여유)
        camX = std::clamp(camX, -1.0f, static_cast<float>(_mapWidth) - VIEW_WIDTH + 1);
        camY = std::clamp(camY, -1.0f, static_cast<float>(_mapHeight) - VIEW_HEIGHT + 1);
    }

    // 맵 경계 기반 타일 채우기
    for (int row = 0; row < VIEW_HEIGHT; ++row)
    {
        for (int col = 0; col < VIEW_WIDTH; ++col)
        {
            // 뷰포트 셀에 대응하는 월드 좌표
            int worldX = static_cast<int>(std::floor(camX + col));
            int worldY = static_cast<int>(std::floor(camY + row));

            // 경계선 (맵 가장자리 1칸)
            bool isBorderX = (worldX == -1 || worldX == _mapWidth);
            bool isBorderY = (worldY == -1 || worldY == _mapHeight);
            bool isInsideX = (worldX >= 0 && worldX < _mapWidth);
            bool isInsideY = (worldY >= 0 && worldY < _mapHeight);

            if ((isBorderX && (isInsideY || isBorderY)) ||
                (isBorderY && (isInsideX || isBorderX)))
            {
                _viewBuffer[row][col].Char.UnicodeChar = L'\x2588';
                _viewBuffer[row][col].Attributes = COLOR_WALL;
            }
            else if (isInsideX && isInsideY)
            {
                _viewBuffer[row][col].Char.UnicodeChar = L'.';
                _viewBuffer[row][col].Attributes = COLOR_TILE;
            }
            else
            {
                _viewBuffer[row][col].Char.UnicodeChar = L' ';
                _viewBuffer[row][col].Attributes = COLOR_OUTSIDE;
            }
        }
    }

    // 섹터 경계선 오버레이 (타일·빈칸 위에만 덮어쓰기)
    if (_sectorSize > 0)
    {
        int camTileX0 = static_cast<int>(std::floor(camX));
        int camTileY0 = static_cast<int>(std::floor(camY));

        for (int row = 0; row < VIEW_HEIGHT; ++row)
        {
            for (int col = 0; col < VIEW_WIDTH; ++col)
            {
                int worldX = camTileX0 + col;
                int worldY = camTileY0 + row;

                bool onVertLine = (worldX % _sectorSize == 0) && (worldX > 0 && worldX < _mapWidth);
                bool onHorzLine = (worldY % _sectorSize == 0) && (worldY > 0 && worldY < _mapHeight);

                if (!onVertLine && !onHorzLine)
                    continue;

                // 타일('.') 또는 빈칸(' ') 위에만 덮어쓰기
                wchar_t existing = _viewBuffer[row][col].Char.UnicodeChar;
                if (existing != L'.' && existing != L' ')
                    continue;

                wchar_t ch;
                if (onVertLine && onHorzLine)
                    ch = L'\x253C';  // ┼
                else if (onVertLine)
                    ch = L'\x2502';  // │
                else
                    ch = L'\x2500';  // ─

                _viewBuffer[row][col].Char.UnicodeChar = ch;
                _viewBuffer[row][col].Attributes = COLOR_SECTOR_LINE;
            }
        }
    }

    // floor(camX) 사전 계산 — 벽 타일과 동일한 좌표계로 플레이어 배치
    int camTileX = static_cast<int>(std::floor(camX));
    int camTileY = static_cast<int>(std::floor(camY));

    // 다른 플레이어 배치 (서버 할당 고유 문자+색상)
    for (const auto& pair : others)
    {
        const ClientPlayer& p = pair.second;
        int sx = static_cast<int>(std::floor(p.x)) - camTileX;
        int sy = static_cast<int>(std::floor(p.y)) - camTileY;

        if (sx >= 0 && sx < VIEW_WIDTH && sy >= 0 && sy < VIEW_HEIGHT)
        {
            _viewBuffer[sy][sx].Char.UnicodeChar = static_cast<wchar_t>(p.displayChar);
            _viewBuffer[sy][sx].Attributes = GetColorFromIndex(p.colorIndex);
        }
    }

    // 스폰 파티클 이펙트 — 상하좌우 셀에 특수 문자 배치 (타일/빈칸 위에만)
    static constexpr int DX[] = { 0, 0, -1, 1 };
    static constexpr int DY[] = { -1, 1, 0, 0 };

    for (const auto& pair : others)
    {
        const ClientPlayer& p = pair.second;
        if (p.spawnEffectTimer <= 0.0f)
            continue;

        int sx = static_cast<int>(std::floor(p.x)) - camTileX;
        int sy = static_cast<int>(std::floor(p.y)) - camTileY;

        // 전반(>1.0s): 밝은 파티클, 후반(<=1.0s): 어두운 파티클
        bool bright = (p.spawnEffectTimer > 1.0f);
        wchar_t particleChar = bright ? L'\x2726' : L'\x00B7';  // ✦ or ·
        WORD particleColor = GetColorFromIndex(p.colorIndex);
        if (!bright)
            particleColor &= ~FOREGROUND_INTENSITY;  // 어두운 색으로

        for (int d = 0; d < 4; ++d)
        {
            int px = sx + DX[d];
            int py = sy + DY[d];

            if (px < 0 || px >= VIEW_WIDTH || py < 0 || py >= VIEW_HEIGHT)
                continue;

            // 타일('.') 또는 빈칸(' ') 위에만 배치 (플레이어·벽 덮지 않음)
            wchar_t existing = _viewBuffer[py][px].Char.UnicodeChar;
            if (existing != L'.' && existing != L' ')
                continue;

            _viewBuffer[py][px].Char.UnicodeChar = particleChar;
            _viewBuffer[py][px].Attributes = particleColor;
        }
    }

    // 내 캐릭터 배치 (서버 할당 문자+색상 + 반전+밑줄로 강조)
    if (me)
    {
        int mx = static_cast<int>(std::floor(me->x)) - camTileX;
        int my = static_cast<int>(std::floor(me->y)) - camTileY;

        if (mx >= 0 && mx < VIEW_WIDTH && my >= 0 && my < VIEW_HEIGHT)
        {
            _viewBuffer[my][mx].Char.UnicodeChar = static_cast<wchar_t>(me->displayChar);
            _viewBuffer[my][mx].Attributes = GetColorFromIndex(me->colorIndex)
                | COMMON_LVB_REVERSE_VIDEO | COMMON_LVB_UNDERSCORE;
        }
    }

    // 한 번에 출력
    COORD bufSize = { VIEW_WIDTH, VIEW_HEIGHT };
    COORD bufCoord = { 0, 0 };
    SMALL_RECT writeRegion = { 0, VIEW_START_ROW,
                               VIEW_WIDTH - 1,
                               static_cast<SHORT>(VIEW_START_ROW + VIEW_HEIGHT - 1) };

    WriteConsoleOutputW(_hConsole, &_viewBuffer[0][0], bufSize, bufCoord, &writeRegion);
}

// ==========================================================================
// 존 이동 전환 오버레이 — 딤 + 중앙 텍스트
// ==========================================================================

void CConsoleRenderer::RenderZoneTransition()
{
    // 게임뷰 영역 전체를 검은 배경으로 클리어
    for (int row = 0; row < VIEW_HEIGHT; ++row)
    {
        for (int col = 0; col < VIEW_WIDTH; ++col)
        {
            _viewBuffer[row][col].Char.UnicodeChar = L' ';
            _viewBuffer[row][col].Attributes = 0;
        }
    }

    // 중앙 행에 로딩 텍스트 오버레이
    static constexpr wchar_t LOADING_TEXT[] = L"\x2550\x2550\x2550 Zone Loading... \x2550\x2550\x2550";
    static constexpr int TEXT_LEN = sizeof(LOADING_TEXT) / sizeof(wchar_t) - 1; // null 제외
    int startCol = (VIEW_WIDTH - TEXT_LEN) / 2;
    int centerRow = VIEW_HEIGHT / 2;

    for (int i = 0; i < TEXT_LEN; ++i)
    {
        _viewBuffer[centerRow][startCol + i].Char.UnicodeChar = LOADING_TEXT[i];
        _viewBuffer[centerRow][startCol + i].Attributes = COLOR_STATUS;
    }

    // 딤 처리된 버퍼를 출력
    COORD bufSize = { VIEW_WIDTH, VIEW_HEIGHT };
    COORD bufCoord = { 0, 0 };
    SMALL_RECT writeRegion = { 0, VIEW_START_ROW,
                               VIEW_WIDTH - 1,
                               static_cast<SHORT>(VIEW_START_ROW + VIEW_HEIGHT - 1) };

    WriteConsoleOutputW(_hConsole, &_viewBuffer[0][0], bufSize, bufCoord, &writeRegion);
}

// ==========================================================================
// 채팅 영역 (Row 23~36)
// ==========================================================================

void CConsoleRenderer::RenderChatArea(const std::vector<std::wstring>& chatLog)
{
    // 구분선
    wchar_t separator[CONSOLE_WIDTH + 1];
    for (int i = 0; i < CONSOLE_WIDTH; ++i)
        separator[i] = L'-';
    separator[CONSOLE_WIDTH] = L'\0';
    WriteTextAt(0, CHAT_START_ROW, separator, COLOR_BORDER, CONSOLE_WIDTH);

    // 채팅 메시지 표시 (최근 MAX_CHAT_LINES-1 개)
    int chatDisplayLines = MAX_CHAT_LINES - 1; // 1줄은 구분선
    int startIdx = 0;
    if (static_cast<int>(chatLog.size()) > chatDisplayLines)
        startIdx = static_cast<int>(chatLog.size()) - chatDisplayLines;

    for (int i = 0; i < chatDisplayLines; ++i)
    {
        SHORT row = CHAT_START_ROW + 1 + static_cast<SHORT>(i);
        ClearLine(row, COLOR_DEFAULT);

        int logIdx = startIdx + i;
        if (logIdx < static_cast<int>(chatLog.size()))
        {
            WriteTextAt(1, row, chatLog[logIdx].c_str(), COLOR_CHAT_TEXT,
                        CONSOLE_WIDTH - 2);
        }
    }
}

// ==========================================================================
// 채팅 입력 (Row 37)
// ==========================================================================

void CConsoleRenderer::RenderChatInput(const std::wstring& chatInput, bool chatMode)
{
    ClearLine(CHAT_INPUT_ROW, COLOR_DEFAULT);

    wchar_t buf[CONSOLE_WIDTH + 1];
    if (chatMode)
    {
        // " > "(3셀) + 커서 "_"(1셀) = 가용 76셀
        static constexpr int MAX_VISIBLE_CELLS = CONSOLE_WIDTH - 4;

        // 입력이 가용 폭을 초과하면 뒷부분만 표시
        const wchar_t* displayStr = chatInput.c_str();
        int totalLen = static_cast<int>(chatInput.size());
        int startIdx = 0;

        int totalWidth = GetDisplayWidth(displayStr, totalLen);
        if (totalWidth > MAX_VISIBLE_CELLS)
        {
            // 뒤에서부터 MAX_VISIBLE_CELLS에 맞는 시작점 탐색
            int cells = 0;
            for (int i = totalLen - 1; i >= 0; --i)
            {
                int w = IsFullWidth(displayStr[i]) ? 2 : 1;
                if (cells + w > MAX_VISIBLE_CELLS) break;
                cells += w;
                startIdx = i;
            }
        }

        swprintf_s(buf, CONSOLE_WIDTH + 1, L" > %s_", displayStr + startIdx);
        WriteTextAt(0, CHAT_INPUT_ROW, buf, COLOR_CHAT_MODE, CONSOLE_WIDTH);
    }
    else
    {
        swprintf_s(buf, CONSOLE_WIDTH + 1, L" [Enter to chat]");
        WriteTextAt(0, CHAT_INPUT_ROW, buf, COLOR_BORDER, CONSOLE_WIDTH);
    }

    // 커서를 채팅 입력줄 끝으로 이동 (에코 문자가 게임 뷰를 오염하지 않도록)
    // 표시 중인 부분의 폭 기준으로 계산
    int visibleLen = static_cast<int>(chatInput.size());
    if (chatMode)
    {
        static constexpr int MAX_VISIBLE_CELLS = CONSOLE_WIDTH - 4;
        int totalWidth = GetDisplayWidth(chatInput.c_str(), visibleLen);
        int displayWidth = (totalWidth > MAX_VISIBLE_CELLS) ? MAX_VISIBLE_CELLS : totalWidth;
        SHORT cursorX = static_cast<SHORT>(3 + displayWidth);
        if (cursorX >= CONSOLE_WIDTH) cursorX = CONSOLE_WIDTH - 1;
        COORD cursorPos = { cursorX, CHAT_INPUT_ROW };
        SetConsoleCursorPosition(_hConsole, cursorPos);
    }
    else
    {
        COORD cursorPos = { 0, CHAT_INPUT_ROW };
        SetConsoleCursorPosition(_hConsole, cursorPos);
    }
}

// ==========================================================================
// 유틸리티
// ==========================================================================

void CConsoleRenderer::WriteTextAt(SHORT x, SHORT y, const wchar_t* text, WORD attr, int maxCells)
{
    COORD pos = { x, y };
    DWORD written;

    int charLen = static_cast<int>(wcslen(text));

    // maxCells(콘솔 셀) 기준으로 출력할 문자 수 결정
    int cells = 0;
    int len = 0;
    for (int i = 0; i < charLen; ++i)
    {
        int w = IsFullWidth(text[i]) ? 2 : 1;
        if (cells + w > maxCells) break;
        cells += w;
        len = i + 1;
    }

    WriteConsoleOutputCharacterW(_hConsole, text, len, pos, &written);

    // 속성 배열 — fullwidth 문자는 2셀분 속성 필요
    std::vector<WORD> attrs(cells, attr);
    WriteConsoleOutputAttribute(_hConsole, attrs.data(), cells, pos, &written);
}

void CConsoleRenderer::ClearLine(SHORT y, WORD attr)
{
    COORD pos = { 0, y };
    DWORD written;
    FillConsoleOutputCharacterW(_hConsole, L' ', CONSOLE_WIDTH, pos, &written);
    FillConsoleOutputAttribute(_hConsole, attr, CONSOLE_WIDTH, pos, &written);
}
