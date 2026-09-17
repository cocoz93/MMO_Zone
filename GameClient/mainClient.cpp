//
#include "GameInstance.h"
#include "../ServerCore/Base/Common/ErrorLog.h"
#include <iostream>

int main()
{
    CGameInstance game;

    if (!game.Initialize())
    {
        LOG_ERROR_STREAM("Failed to initialize game.");
        return 1;
    }

    game.Run();
    game.Shutdown();

    std::cout << "Client terminated." << std::endl;
    return 0;
}