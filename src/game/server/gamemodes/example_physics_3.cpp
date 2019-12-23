#include "example_physics_3.h"
#include <engine/server.h>
#include <engine/shared/protocol.h>
#include <engine/shared/config.h>
#include <game/server/gamecontext.h>
#include <game/server/player.h>
#include <game/server/entities/character.h>
#include <game/server/entities/projectile.h>

// TODO: move
inline bool NetworkClipped(CGameContext* pGameServer, int SnappingClient, vec2 CheckPos)
{
	float dx = pGameServer->m_apPlayers[SnappingClient]->m_ViewPos.x-CheckPos.x;
	float dy = pGameServer->m_apPlayers[SnappingClient]->m_ViewPos.y-CheckPos.y;

	if(absolute(dx) > 1000.0f || absolute(dy) > 800.0f)
		return true;

	if(distance(pGameServer->m_apPlayers[SnappingClient]->m_ViewPos, CheckPos) > 1100.0f)
		return true;
	return false;
}

struct CNetObj_Bee
{
	enum { NET_ID = 0x1 };
	int m_Core1ID;
	int m_Core2ID;
	int m_Health;
};

struct CNetObj_Hive
{
	enum { NET_ID = 0x2 };
	int m_CoreID;
};

uint32_t xorshift32()
{
	static uint32_t state = time(0);
	uint32_t x = state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return state = x;
}

float randFloat(float fmin, float fmax)
{
	return fmin + ((double)xorshift32() / 0xFFFFFFFF) * (fmax - fmin);
}

const float BeeMaxVelX = 1.2;
const float BeeMinVelX = 0;
const float BeeMaxVelY = 1.2;
const float BeeMinVelY = -0.2;

struct CBee
{
	int m_CoreUID[2];
	CDuckWorldCore* m_pWorld;
	int m_Health;
	int m_BeeID;
	int m_FlightTickCount;
	int m_FlightTickTotal;
	vec2 m_LastVelY;
	vec2 m_TargetVelY;
	float m_LastVelX;
	float m_TargetVelX;

	inline CDuckCollision* Collision()
	{
		return m_pWorld->m_pCollision;
	}

	void Create(CDuckWorldCore* pWorld, vec2 Pos, int BeePlgUID, int BeeID)
	{
		m_pWorld = pWorld;

		CCustomCore* pCore1 = m_pWorld->AddCustomCore(26.25);
		pCore1->m_PlgUID = BeePlgUID;
		pCore1->m_Pos = Pos;

		CCustomCore* pCore2 = m_pWorld->AddCustomCore(26.25);
		pCore2->m_PlgUID = BeePlgUID;
		pCore2->m_Pos = Pos + vec2(36, 0);

		CDuckPhysJoint Joint;
		Joint.m_CustomCoreUID1 = pCore1->m_UID;
		Joint.m_CustomCoreUID2 = pCore2->m_UID;
		//Joint.m_Force1 = 4;
		//Joint.m_Force2 = 2;
		Joint.m_MaxDist = 60;
		m_pWorld->m_aJoints.add(Joint);

		m_CoreUID[0] = pCore1->m_UID;
		m_CoreUID[1] = pCore2->m_UID;

		m_Health = 10;
		m_BeeID = BeeID;

		m_FlightTickCount = 0;
		m_LastVelY = vec2(BeeMaxVelY, BeeMaxVelY);
		m_TargetVelY = m_LastVelY;
		m_LastVelX = BeeMaxVelX;
		m_TargetVelX = m_LastVelX;
	}

	void Tick()
	{
		/*{
		CCustomCore* pCore1 = m_pWorld->FindCustomCoreFromUID(m_CoreUID[0]);
		CCustomCore* pCore2 = m_pWorld->FindCustomCoreFromUID(m_CoreUID[1]);
		pCore1->m_Vel.y -= 1.8;
		pCore2->m_Vel.y -= 1.8;
		return; // TODO: remove
		}*/

		m_FlightTickCount--;
		if(m_FlightTickCount <= 0)
		{
			m_FlightTickTotal = 20 + xorshift32() % 60;
			m_FlightTickCount = m_FlightTickTotal;
			m_LastVelY = m_TargetVelY;
			m_TargetVelY = vec2(randFloat(BeeMinVelY, BeeMaxVelY), randFloat(BeeMinVelY, BeeMaxVelY));
			m_LastVelX = m_TargetVelX;
			m_TargetVelX = randFloat(BeeMinVelX, BeeMaxVelX);
		}

		const float a = (float)m_FlightTickCount/m_FlightTickTotal;
		vec2 MoveVel = mix(m_LastVelY, m_TargetVelY, a);

		CCustomCore* pCore1 = m_pWorld->FindCustomCoreFromUID(m_CoreUID[0]);
		CCustomCore* pCore2 = m_pWorld->FindCustomCoreFromUID(m_CoreUID[1]);
		pCore1->m_Vel.y -= MoveVel.x;
		pCore2->m_Vel.y -= MoveVel.y;

		vec2 Dir = normalize(pCore1->m_Pos - pCore2->m_Pos);
		float VelX = sign(Dir.x) * mix(m_LastVelX, m_TargetVelX, a);
		pCore1->m_Vel.x += VelX;
		pCore2->m_Vel.x += VelX;

		// turn around
		if(Collision()->CheckPoint(pCore1->m_Pos + vec2((pCore1->m_Radius + 5) * sign(pCore1->m_Vel.x), 0)))
		{
			m_TargetVelX = -m_TargetVelX;
		}

		// try to be horizontal
		// lift up the butt
		if(fabs(Dir.x) < 0.5)
		{
			pCore2->m_Vel.x -= VelX * 1.5;
		}
	}

	void Snap(CGameContext* pGameServer, int SnappinClient)
	{
		int Core1ID, Core2ID;
		CCustomCore* pCore1 = m_pWorld->FindCustomCoreFromUID(m_CoreUID[0], &Core1ID);
		CCustomCore* pCore2 = m_pWorld->FindCustomCoreFromUID(m_CoreUID[1], &Core2ID);
		vec2 Pos = pCore1->m_Pos + vec2(70, 0);

		if(NetworkClipped(pGameServer, SnappinClient, Pos))
			return;

		CNetObj_Bee* pBee = pGameServer->DuckSnapNewItem<CNetObj_Bee>(m_BeeID);
		pBee->m_Core1ID = Core1ID;
		pBee->m_Core2ID = Core2ID;
		pBee->m_Health = m_Health;

		//dbg_msg("bee", "%d %d %d", pBee->m_Core1ID, pBee->m_Core2ID, pBee->m_Health);
	}
};

struct CHive
{
	int m_HiveID;
	int m_CoreUID;
	CDuckWorldCore* m_pWorld;
	int m_Hits;

	void Create(CDuckWorldCore* pWorld, vec2 Pos, int HivePlgUID, int HiveID)
	{
		m_pWorld = pWorld;
		CCustomCore* pCore1 = m_pWorld->AddCustomCore(50);
		pCore1->m_PlgUID = HivePlgUID;
		pCore1->m_Pos = Pos;
		m_CoreUID = pCore1->m_UID;

		m_HiveID = HiveID;
		m_Hits = 3;
	}

	void Tick(CGameControllerExamplePhys3* pController)
	{
		CCustomCore* pCore1 = m_pWorld->FindCustomCoreFromUID(m_CoreUID);
		pCore1->m_Vel = vec2(0,0);

		CGameWorld* pGameWorld = &pController->GameServer()->m_World;
		IServer* pServer = pController->GameServer()->Server();

		CProjectile *p = (CProjectile *)pGameWorld->FindFirst(CGameWorld::ENTTYPE_PROJECTILE);
		for(; p; p = (CProjectile *)p->TypeNext())
		{
			int StartTick = *(int*)((char*)p + sizeof(CEntity) + 40);
			float Ct = (pServer->Tick()-StartTick)/(float)pServer->TickSpeed();
			vec2 ProjPos = p->GetPos(Ct);
			int From = p->GetOwner();

			if(distance(p->GetPos(Ct), pCore1->m_Pos) < pCore1->m_Radius + p->GetProximityRadius())
			{
				p->MarkForDestroy();
				m_Hits--;

				int64 Mask = CmaskOne(From);
				pController->GameServer()->CreateSound(pController->GameServer()->m_apPlayers[From]->m_ViewPos, SOUND_HIT, Mask);

				if(m_Hits <= 0)
				{
					m_Hits = 3;
					pController->GameServer()->CreateDamage(pCore1->m_Pos, From, ProjPos, 1, 0, false);
					pController->GameServer()->CreateSound(pCore1->m_Pos, SOUND_PLAYER_PAIN_LONG);
					pController->SpawnBeeAt(pCore1->m_Pos + vec2(0, pCore1->m_Radius + 100));
				}
			}
		}
	}

	void Snap(CGameContext* pGameServer, int SnappinClient)
	{
		int CoreID;
		CCustomCore* pCore1 = m_pWorld->FindCustomCoreFromUID(m_CoreUID, &CoreID);

		CNetObj_Hive* pHive = pGameServer->DuckSnapNewItem<CNetObj_Hive>(m_HiveID);
		pHive->m_CoreID = CoreID;
	}
};

#define MAX_BEES 64
#define MAX_HIVES 2
static CBee m_aBees[MAX_BEES];
static bool m_aBeeIsAlive[MAX_BEES];
static CHive m_aHives[MAX_HIVES];

void CGameControllerExamplePhys3::SpawnBeeAt(vec2 Pos)
{
	int BeeID = -1;

	for(int i = 0; i < MAX_BEES; i++)
	{
		if(!m_aBeeIsAlive[i]) {
			BeeID = i;
			break;
		}
	}

	if(BeeID == -1)
		return;

	CBee Bee;
	Bee.Create(&m_DuckWorldCore, Pos, m_BeePlgUID, BeeID);
	m_aBees[BeeID] = Bee;
	m_aBeeIsAlive[BeeID] = true;
}

CGameControllerExamplePhys3::CGameControllerExamplePhys3(class CGameContext *pGameServer)
: IGameController(pGameServer)
{
	m_pGameType = "EXPHYS3";
	str_copy(g_Config.m_SvMap, "duck_ex_phys_3", sizeof(g_Config.m_SvMap)); // force map

	// load duck mod
	if(!Server()->LoadDuckMod("", "", "mods/example_physics_3"))
	{
		dbg_msg("server", "failed to load duck mod");
	}

	CDuckCollision* pCollision = (CDuckCollision*)GameServer()->Collision();
	m_DuckWorldCore.Init(&GameServer()->m_World.m_Core, pCollision);

	CPhysicsLawsGroup* pPlgBee = m_DuckWorldCore.AddPhysicLawsGroup();
	pPlgBee->m_AirFriction = 0.95;
	pPlgBee->m_GroundFriction = 1.0;
	m_BeePlgUID = pPlgBee->m_UID;

	CPhysicsLawsGroup* pPlgHive = m_DuckWorldCore.AddPhysicLawsGroup();
	pPlgHive->m_Gravity = 0.0;
	pPlgHive->m_AirFriction = 0;
	pPlgHive->m_GroundFriction = 0;
	m_HivePlgUID = pPlgBee->m_UID;

	mem_zero(m_aBeeIsAlive, sizeof(m_aBeeIsAlive));

	//SpawnBeeAt(vec2(1344, 680));

	m_aHives[0].Create(&m_DuckWorldCore, vec2(1312, 638), m_HivePlgUID, 0);
	m_aHives[1].Create(&m_DuckWorldCore, vec2(1344, 1790), m_HivePlgUID, 1);
}

void CGameControllerExamplePhys3::OnPlayerConnect(CPlayer* pPlayer)
{
	IGameController::OnPlayerConnect(pPlayer);
	int ClientID = pPlayer->GetCID();
}

void CGameControllerExamplePhys3::Tick()
{
	IGameController::Tick();
	m_DuckWorldCore.Tick();

	for(int i = 0; i < MAX_BEES; i++)
	{
		if(m_aBeeIsAlive[i]) {
			m_aBees[i].Tick();
		}
	}

	for(int i = 0; i < MAX_HIVES; i++)
	{
		m_aHives[i].Tick(this);
	}
}

void CGameControllerExamplePhys3::Snap(int SnappingClient)
{
	IGameController::Snap(SnappingClient);
	m_DuckWorldCore.Snap(GameServer(), SnappingClient);

	for(int i = 0; i < MAX_BEES; i++)
	{
		if(m_aBeeIsAlive[i]) {
			m_aBees[i].Snap(GameServer(), SnappingClient);
		}
	}

	for(int i = 0; i < MAX_HIVES; i++)
	{
		m_aHives[i].Snap(GameServer(), SnappingClient);
	}
}

void CGameControllerExamplePhys3::OnDuckMessage(int MsgID, CUnpacker *pUnpacker, int ClientID)
{
	dbg_msg("duck", "DuckMessage :: NetID = 0x%x", MsgID);
}
