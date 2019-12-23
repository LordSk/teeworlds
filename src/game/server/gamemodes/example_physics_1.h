#pragma once
#include <game/server/gamecontroller.h>
#include <game/gamecore.h>
#include <game/duck_gamecore.h>

class CGameControllerExamplePhys1 : public IGameController
{
	bool HasEnoughPlayers() const { return true; }

	CDuckWorldCore m_DuckWorldCore;
	int m_TestCoreID;

public:
	CGameControllerExamplePhys1(class CGameContext *pGameServer);

	virtual void OnPlayerConnect(CPlayer* pPlayer);
	virtual void Tick();
	virtual void Snap(int SnappingClient);
	virtual void OnDuckMessage(int MsgID, CUnpacker *pUnpacker, int ClientID);
};
