#pragma once
#include <game/server/gamecontroller.h>
#include <game/gamecore.h>
#include <game/duck_gamecore.h>

class CGameControllerXmas : public IGameController
{
	CDuckWorldCore m_DuckWorldCore;

	bool HasEnoughPlayers() const { return true; }

public:
	CGameControllerXmas(class CGameContext *pGameServer);

	virtual void OnPlayerConnect(CPlayer* pPlayer);
	virtual void Tick();
	virtual void Snap(int SnappingClient);
	virtual void OnDuckMessage(int MsgID, CUnpacker *pUnpacker, int ClientID);
};
