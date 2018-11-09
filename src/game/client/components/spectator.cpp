/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include <engine/demo.h>
#include <engine/keys.h>
#include <engine/graphics.h>
#include <engine/textrender.h>
#include <engine/shared/config.h>

#include <generated/client_data.h>
#include <generated/protocol.h>

#include <game/client/animstate.h>
#include <game/client/render.h>

#include "menus.h"
#include "controls.h"
#include "spectator.h"
#include "camera.h"

static float s_TextScale = 0.8f;

void CSpectator::ConKeySpectator(IConsole::IResult *pResult, void *pUserData)
{
	CSpectator *pSelf = (CSpectator *)pUserData;
	if(pSelf->m_pClient->m_Snap.m_SpecInfo.m_Active &&
		(pSelf->Client()->State() != IClient::STATE_DEMOPLAYBACK || pSelf->DemoPlayer()->GetDemoType() == IDemoPlayer::DEMOTYPE_SERVER))
		pSelf->m_Active = pResult->GetInteger(0) != 0;
}

void CSpectator::ConSpectate(IConsole::IResult *pResult, void *pUserData)
{
	((CSpectator *)pUserData)->Spectate(pResult->GetInteger(0), pResult->GetInteger(1));
}

void CSpectator::ConSpectateNext(IConsole::IResult *pResult, void *pUserData)
{
	CSpectator *pSelf = (CSpectator *)pUserData;
	int NewSpecMode = pSelf->m_pClient->m_Snap.m_SpecInfo.m_SpecMode;
	int NewSpectatorID = -1;
	bool GotNewSpectatorID = false;

	if(NewSpecMode != SPEC_PLAYER)
		NewSpecMode = (NewSpecMode + 1) % NUM_SPECMODES;
	else
		NewSpectatorID = pSelf->m_pClient->m_Snap.m_SpecInfo.m_SpectatorID;

	switch(NewSpecMode)
	{	// drop through
	case SPEC_PLAYER:
		for(int i = NewSpectatorID + 1; i < MAX_CLIENTS; i++)
		{
			if(!pSelf->m_pClient->m_aClients[i].m_Active || pSelf->m_pClient->m_aClients[i].m_Team == TEAM_SPECTATORS)
				continue;

			NewSpectatorID = i;
			GotNewSpectatorID = true;
			break;
		}
		if(GotNewSpectatorID)
			break;
		NewSpecMode = SPEC_FLAGRED;
		NewSpectatorID = -1;
	case SPEC_FLAGRED:
	case SPEC_FLAGBLUE:
		if(pSelf->m_pClient->m_GameInfo.m_GameFlags&GAMEFLAG_FLAGS)
		{
			GotNewSpectatorID = true;
			break;
		}
		NewSpecMode = SPEC_FREEVIEW;
	case SPEC_FREEVIEW:
		GotNewSpectatorID = true;
	}

	if(GotNewSpectatorID)
		pSelf->Spectate(NewSpecMode, NewSpectatorID);
}

void CSpectator::ConSpectatePrevious(IConsole::IResult *pResult, void *pUserData)
{
	CSpectator *pSelf = (CSpectator *)pUserData;
	int NewSpecMode = pSelf->m_pClient->m_Snap.m_SpecInfo.m_SpecMode;
	int NewSpectatorID = MAX_CLIENTS;
	bool GotNewSpectatorID = false;

	if(NewSpecMode != SPEC_PLAYER)
		NewSpecMode = (NewSpecMode - 1 + NUM_SPECMODES) % NUM_SPECMODES;
	else
		NewSpectatorID = pSelf->m_pClient->m_Snap.m_SpecInfo.m_SpectatorID;

	switch(NewSpecMode)
	{	// drop through
	case SPEC_FLAGBLUE:
	case SPEC_FLAGRED:
		if(pSelf->m_pClient->m_GameInfo.m_GameFlags&GAMEFLAG_FLAGS)
		{
			NewSpectatorID = -1;
			GotNewSpectatorID = true;
			break;
		}
		NewSpecMode = SPEC_PLAYER;
		NewSpectatorID = MAX_CLIENTS;
	case SPEC_PLAYER:
		for(int i = NewSpectatorID - 1; i >= 0; i--)
		{
			if(!pSelf->m_pClient->m_aClients[i].m_Active || pSelf->m_pClient->m_aClients[i].m_Team == TEAM_SPECTATORS)
				continue;

			NewSpectatorID = i;
			GotNewSpectatorID = true;
			break;
		}
		if(GotNewSpectatorID)
			break;
		NewSpecMode = SPEC_FREEVIEW;
	case SPEC_FREEVIEW:
		NewSpectatorID = -1;
		GotNewSpectatorID = true;
	}

	if(GotNewSpectatorID)
		pSelf->Spectate(NewSpecMode, NewSpectatorID);
}

CSpectator::CSpectator()
{
	OnReset();
}

void CSpectator::OnConsoleInit()
{
	Console()->Register("+spectate", "", CFGFLAG_CLIENT, ConKeySpectator, this, "Open spectator mode selector");
	Console()->Register("spectate", "ii", CFGFLAG_CLIENT, ConSpectate, this, "Switch spectator mode");
	Console()->Register("spectate_next", "", CFGFLAG_CLIENT, ConSpectateNext, this, "Spectate the next player");
	Console()->Register("spectate_previous", "", CFGFLAG_CLIENT, ConSpectatePrevious, this, "Spectate the previous player");
}

bool CSpectator::OnMouseMove(float x, float y)
{
	// we snatch mouse movement if spectating is active

	if(!m_pClient->m_Snap.m_SpecInfo.m_Active)
		return false;

	const bool ShowMouse = m_SpecMode != FREE_VIEW;

	if(ShowMouse)
	{
		UI()->ConvertMouseMove(&x, &y);
		m_MouseScreenPos += vec2(x,y);

		// clamp mouse position to screen
		m_MouseScreenPos.x = clamp(m_MouseScreenPos.x, 0.f, (float)Graphics()->ScreenWidth() - 1.0f);
		m_MouseScreenPos.y = clamp(m_MouseScreenPos.y, 0.f, (float)Graphics()->ScreenHeight() - 1.0f);

		if(x != 0 || y != 0)
			m_MouseMoveTimer = time_get();
	}
	else
	{
		// free view
		m_pClient->m_pControls->m_MousePos += vec2(x, y);
		// -> this goes to CCamera::OnRender()
	}

	return true;
}

bool CSpectator::OnInput(IInput::CEvent InputEvent)
{
	if(!m_pClient->m_Snap.m_SpecInfo.m_Active)
		return false;

	if(InputEvent.m_Key == KEY_SPACE && InputEvent.m_Flags&IInput::FLAG_RELEASE)
	{
		if(m_SpecMode == FREE_VIEW)
			CameraOverview();
		else
			CameraFreeview();
		return true;
	}

	if(InputEvent.m_Key == KEY_MOUSE_1 && InputEvent.m_Flags&IInput::FLAG_RELEASE
	   && (m_SpecMode == FREE_VIEW || m_SpecMode == OVERVIEW)
	   && m_ClosestCharID != -1)
	{
		m_MouseScreenPos.x = 0;
		m_SpectatorID = m_ClosestCharID;
		CameraFollow();
		return true;
	}

	return false;
}

void CSpectator::OnRelease()
{
	float mx = UI()->MouseX();
	float my = UI()->MouseY();
	CUIRect Screen = *UI()->Screen();

	m_MouseScreenPos.x = (mx / Screen.w) * Graphics()->ScreenWidth();
	m_MouseScreenPos.y = (my / Screen.h) * Graphics()->ScreenHeight();
}

void CSpectator::UpdatePositions()
{
	CGameClient::CSnapState& Snap = m_pClient->m_Snap;
	CGameClient::CSnapState::CSpectateInfo& SpecInfo = Snap.m_SpecInfo;

	// spectator position
	if(SpecInfo.m_Active)
	{
		if(Client()->State() == IClient::STATE_DEMOPLAYBACK &&
		   DemoPlayer()->GetDemoType() == IDemoPlayer::DEMOTYPE_SERVER &&
			SpecInfo.m_SpectatorID != -1)
		{
			SpecInfo.m_Position = mix(
				vec2(Snap.m_aCharacters[SpecInfo.m_SpectatorID].m_Prev.m_X,
					Snap.m_aCharacters[SpecInfo.m_SpectatorID].m_Prev.m_Y),
				vec2(Snap.m_aCharacters[SpecInfo.m_SpectatorID].m_Cur.m_X,
					Snap.m_aCharacters[SpecInfo.m_SpectatorID].m_Cur.m_Y),
				Client()->IntraGameTick());
			SpecInfo.m_UsePosition = true;
		}
		else if(Snap.m_pSpectatorInfo &&
				(Client()->State() == IClient::STATE_DEMOPLAYBACK || SpecInfo.m_SpecMode != SPEC_FREEVIEW))
		{
			if(Snap.m_pPrevSpectatorInfo)
				SpecInfo.m_Position = mix(vec2(Snap.m_pPrevSpectatorInfo->m_X, Snap.m_pPrevSpectatorInfo->m_Y),
											vec2(Snap.m_pSpectatorInfo->m_X, Snap.m_pSpectatorInfo->m_Y),
											Client()->IntraGameTick());
			else
				SpecInfo.m_Position = vec2(Snap.m_pSpectatorInfo->m_X, Snap.m_pSpectatorInfo->m_Y);
			SpecInfo.m_UsePosition = true;
		}
	}

	static int64 LastUpdateTime = time_get();
	int64 Now = time_get();
	const double Delta = (Now - LastUpdateTime) / (double)time_freq();
	LastUpdateTime = Now;

	// edge scrolling
	if(m_SpecMode == OVERVIEW || m_SpecMode == FOLLOW)
	{
		vec2& CtrlMp = m_pClient->m_pControls->m_MousePos;
		const float Speed = 700.0f;
		const float Edge = 30.0f;

		if(m_MouseScreenPos.x < Edge) CtrlMp.x -= Speed * Delta;
		if(m_MouseScreenPos.y < Edge) CtrlMp.y -= Speed * Delta;
		if(m_MouseScreenPos.x > Graphics()->ScreenWidth()-Edge)  CtrlMp.x += Speed * Delta;
		if(m_MouseScreenPos.y > Graphics()->ScreenHeight()-Edge) CtrlMp.y += Speed * Delta;
	}

	// find closest character to mouse
	if(m_SpecMode == OVERVIEW || m_SpecMode == FREE_VIEW)
	{
		vec2 CamCenter = m_pClient->m_pControls->m_MousePos;
		if(m_SpecMode == OVERVIEW)
		{
			CamCenter += m_MouseScreenPos -
						 vec2(Graphics()->ScreenWidth() * 0.5f, Graphics()->ScreenHeight() * 0.5f);
		}
		m_ClosestCharID = -1;
		float ClosestDistance = 150.f;

		for(int i = 0; i < MAX_CLIENTS; ++i)
		{
			if(!Snap.m_paPlayerInfos[i] || m_pClient->m_aClients[i].m_Team == TEAM_SPECTATORS ||
			   !m_pClient->m_Snap.m_aCharacters[i].m_Active)
				continue;

			CGameClient::CSnapState::CCharacterInfo CharInfo = m_pClient->m_Snap.m_aCharacters[i];
			const vec2 CharCurPos(CharInfo.m_Cur.m_X, CharInfo.m_Cur.m_Y);
			const vec2 CharPrevPos(CharInfo.m_Prev.m_X, CharInfo.m_Prev.m_Y);
			const vec2 CharPos = mix(CharPrevPos, CharCurPos, Client()->IntraGameTick());
			const float Dist = distance(CamCenter, CharPos);

			if(Dist < ClosestDistance)
			{
				ClosestDistance = Dist;
				m_ClosestCharID = i;
				m_TargetPos = CharPos;
			}
		}
	}
}

bool CSpectator::DoButtonSelect(void* pID, const char* pLabel, CUIRect Rect, bool Selected)
{
	const bool Hovered = UI()->HotItem() == pID;
	vec4 ButtonColor = vec4(1.0f, 1.0f, 1.0f, 0.25f);
	if(Selected)
		ButtonColor = vec4(0.5f, 0.75f, 1.0f, 0.5f);
	else if(Hovered)
		ButtonColor = vec4(1.0f, 1.0f, 1.0f, 0.5f);

	RenderTools()->DrawRoundRect(&Rect, ButtonColor, 3.0f);

	UI()->DoLabel(&Rect, pLabel, Rect.h*s_TextScale*0.8f, CUI::ALIGN_CENTER);

	bool Modified = false;
	if(UI()->DoButtonLogic(pID, 0, 0, &Rect))
	{
		Modified = true;
	}

	return Modified;
}

bool CSpectator::DoPlayerButtonSelect(void* pID, CUIRect Rect, bool Selected, const char* pClan,
									  const char* pName)
{
	const bool Hovered = UI()->HotItem() == pID;
	vec4 ButtonColor = vec4(1.0f, 1.0f, 1.0f, 0.25f);
	if(Selected)
		ButtonColor = vec4(0.5f, 0.75f, 1.0f, 0.5f);
	else if(Hovered)
		ButtonColor = vec4(1.0f, 1.0f, 1.0f, 0.5f);

	RenderTools()->DrawRoundRect(&Rect, ButtonColor, 3.0f);

	bool Modified = false;
	if(UI()->DoButtonLogic(pID, 0, 0, &Rect))
	{
		Modified = true;
	}

	Rect.y += 2.0f;

	char aStr[64];
	str_format(aStr, sizeof(aStr), "%s %s", pClan, pName); // TODO position clan and name
	UI()->DoLabel(&Rect, aStr, Rect.h*s_TextScale*0.5f, CUI::ALIGN_CENTER);

	return Modified;
}

void CSpectator::CameraOverview()
{
	m_SpecMode = OVERVIEW;
	Spectate(SPEC_FREEVIEW, -1);

	if(m_MouseScreenPos.x == 0)
	{
		m_MouseScreenPos.x = Graphics()->ScreenWidth() * 0.5f;
		m_MouseScreenPos.y = Graphics()->ScreenHeight() * 0.5f;
	}
}

void CSpectator::CameraFreeview()
{
	m_SpecMode = FREE_VIEW;
	Spectate(SPEC_FREEVIEW, -1);
}

void CSpectator::CameraFollow()
{
	m_SpecMode = FOLLOW;
	if(m_SpectatorID != -1) // TODO: do more checks
	{
		Spectate(SPEC_PLAYER, m_SpectatorID);
	}

	if(m_MouseScreenPos.x == 0)
	{
		m_MouseScreenPos.x = Graphics()->ScreenWidth() * 0.5f;
		m_MouseScreenPos.y = Graphics()->ScreenHeight() * 0.5f;
	}
}

void CSpectator::DrawTargetHighlightWorldSpace(vec2 Pos, vec4 Color)
{
	CUIRect Screen;
	Graphics()->GetScreen(&Screen.x, &Screen.y, &Screen.w, &Screen.h);

	// map screen to world space
	float aPoints[4];
	const vec2 CamCenter = m_pClient->m_pCamera->m_Center;
	RenderTools()->MapScreenToWorld(CamCenter.x, CamCenter.y, 1, 1, 0, 0,
									Graphics()->ScreenAspect(), 1.0f, aPoints);
	Graphics()->MapScreen(aPoints[0], aPoints[1], aPoints[2], aPoints[3]);

	Graphics()->BlendNormal();
	Graphics()->TextureClear();

	Graphics()->QuadsBegin();
	// premultiply alpha
	Graphics()->SetColor(Color.r*Color.a, Color.g*Color.a, Color.b*Color.a, Color.a);

	const float x = Pos.x;
	const float y = Pos.y;
	const float Radius = 50.0f;
	const float Thickness = 10.0f;

	// build a ring
	IGraphics::CFreeformItem Array[32];
	int NumItems = 0;
	const int Segments = 32;
	float FSegments = (float)Segments;
	for(int i = 0; i < Segments; i++)
	{
		const float a1 = i/FSegments * 2*pi;
		const float a2 = (i+1)/FSegments * 2*pi;
		const float Ca1 = cosf(a1);
		const float Ca2 = cosf(a2);
		const float Sa1 = sinf(a1);
		const float Sa2 = sinf(a2);

		Array[NumItems++] = IGraphics::CFreeformItem(
			x+Ca1*(Radius-Thickness), y+Sa1*(Radius-Thickness),
			x+Ca2*(Radius-Thickness), y+Sa2*(Radius-Thickness),
			x+Ca1*Radius, y+Sa1*Radius,
			x+Ca2*Radius, y+Sa2*Radius
			);
		if(NumItems == 32)
		{
			Graphics()->QuadsDrawFreeform(Array, 32);
			NumItems = 0;
		}
	}
	if(NumItems)
		Graphics()->QuadsDrawFreeform(Array, NumItems);

	Graphics()->QuadsEnd();

	// restore screen
	Graphics()->MapScreen(Screen.x, Screen.y, Screen.w, Screen.h);
}

void CSpectator::OnRender()
{
	if(!m_pClient->m_Snap.m_SpecInfo.m_Active)
		return;

	if(m_pClient->m_pMenus->IsActive())
		return;

	UpdatePositions();

	const bool ShowMouse = m_SpecMode != FREE_VIEW;

	int64 Now = time_get();
	double MouseLastMovedTime = (Now - m_MouseMoveTimer) / (double)time_freq();

	// update the ui
	CUIRect *pScreen = UI()->Screen();
	float mx = (m_MouseScreenPos.x/(float)Graphics()->ScreenWidth())*pScreen->w;
	float my = (m_MouseScreenPos.y/(float)Graphics()->ScreenHeight())*pScreen->h;

	if(ShowMouse)
	{
		int MouseButtons = 0;
		if(Input()->KeyIsPressed(KEY_MOUSE_1)) MouseButtons |= 1;
		if(Input()->KeyIsPressed(KEY_MOUSE_2)) MouseButtons |= 2;
		if(Input()->KeyIsPressed(KEY_MOUSE_3)) MouseButtons |= 4;

		UI()->Update(mx, my, mx*3.0f, my*3.0f, MouseButtons);
	}


	CUIRect Screen = *UI()->Screen();
	Graphics()->MapScreen(Screen.x, Screen.y, Screen.w, Screen.h);
	const float Spacing = 3.f;

	// spectator right window
	CUIRect MainView;
	Screen.VSplitMid(0, &MainView);
	MainView.VSplitMid(0, &MainView);
	MainView.h *= 0.5f;
	MainView.y += MainView.h * 0.5f;

	if(m_SpecMode != FOLLOW)
	{
		MainView.h = 40 + Spacing * 3.f;
	}

	RenderTools()->DrawRoundRect(&MainView, vec4(0.0f, 0.0f, 0.0f, 0.5f), 5.0f);

	CUIRect LineRect, Button, Label;

	MainView.HSplitTop(Spacing, 0, &MainView);
	MainView.HSplitTop(20.f, &Label, &MainView);
	Label.x += 8.f;

	UI()->DoLabel(&Label, Localize("Camera"), Label.h*s_TextScale*0.8f, CUI::ALIGN_LEFT);

	MainView.HSplitTop(Spacing, 0, &MainView);
	MainView.HSplitTop(20.f, &LineRect, &MainView);
	LineRect.VMargin(Spacing, &LineRect);

	const float ButWidth = (LineRect.w - Spacing * 2.f) / 3.f;
	LineRect.VSplitLeft(ButWidth, &Button, &LineRect);

	static int s_ButtonFreeViewID;
	if(DoButtonSelect(&s_ButtonFreeViewID, "Free-view", Button, m_SpecMode == FREE_VIEW))
	{
		CameraFreeview();
	}

	LineRect.VSplitLeft(Spacing, 0, &LineRect);
	LineRect.VSplitLeft(ButWidth, &Button, &LineRect);

	static int s_ButtonOverViewID;
	if(DoButtonSelect(&s_ButtonOverViewID, "Overview", Button, m_SpecMode == OVERVIEW))
	{
		CameraOverview();
	}

	LineRect.VSplitLeft(Spacing, 0, &LineRect);
	LineRect.VSplitLeft(ButWidth, &Button, &LineRect);

	static int s_ButtonFollowID;
	if(DoButtonSelect(&s_ButtonFollowID, "Follow", Button, m_SpecMode == FOLLOW))
	{
		CameraFollow();
	}

	CGameClient::CSnapState& Snap = m_pClient->m_Snap;
	CGameClient::CSnapState::CSpectateInfo& SpecInfo = Snap.m_SpecInfo;

	//static CMenus::CListBoxState s_ListBoxState;
	/*UiDoListboxHeader(&s_ListBoxState, &MainView, Localize("Skins"), 20.0f, 2.0f);
	UiDoListboxStart(&s_ListBoxState, &m_RefreshSkinSelector, 50.0f, 0,
					 s_paSkinList.size(), 10, OldSelected);	*/

	if(m_SpecMode == FOLLOW)
	{
		MainView.HSplitTop(Spacing, 0, &MainView);
		MainView.HSplitTop(20.f, &LineRect, &MainView);
		LineRect.VMargin(Spacing, &LineRect);

		// flag buttons
		CUIRect ButtonFlagRed, ButtonFlagBlue;
		LineRect.VSplitMid(&ButtonFlagRed, &ButtonFlagBlue);
		ButtonFlagRed.w -= Spacing * 0.5f;
		ButtonFlagBlue.w -= Spacing * 0.5f;
		ButtonFlagBlue.x += Spacing * 0.5f;

		static int s_ButFlagRedID, s_ButFlagBlueID;
		DoButtonSelect(&s_ButFlagRedID, "Flag red", ButtonFlagRed, false);
		DoButtonSelect(&s_ButFlagBlueID, "Flag blue", ButtonFlagBlue, false);

		// player list
		CUIRect FollowListRect;
		MainView.HSplitTop(Spacing, 0, &FollowListRect);
		FollowListRect.VMargin(Spacing, &FollowListRect);

		UI()->ClipEnable(&FollowListRect);

		struct FollowPlayerInfo
		{
			int m_CID;
			const char* m_pName;
			const char* m_pClan;
			CTeeRenderInfo m_TeeInfo;
		};
		static FollowPlayerInfo FollowList[MAX_CLIENTS];
		int FollowListCount = 0;

		for(int i = 0; i < MAX_CLIENTS; ++i)
		{
			if(!Snap.m_paPlayerInfos[i] || m_pClient->m_aClients[i].m_Team == TEAM_SPECTATORS)
				continue;

			CGameClient::CClientData& Cd = m_pClient->m_aClients[i];
			FollowPlayerInfo Fpi;
			Fpi.m_CID = i;
			Fpi.m_pName = Cd.m_aName;
			Fpi.m_pClan = Cd.m_aClan;
			Fpi.m_TeeInfo = Cd.m_RenderInfo;
			Fpi.m_TeeInfo.m_Size = 35.f;
			FollowList[FollowListCount++] = Fpi;
		}

		for(int i = 0; i < FollowListCount; ++i)
		{
			FollowPlayerInfo& Fpi = FollowList[i];

			FollowListRect.HSplitTop(Spacing, 0, &FollowListRect);
			FollowListRect.HSplitTop(30.f, &LineRect, &FollowListRect);
			LineRect.VMargin(Spacing, &LineRect);

			if(DoPlayerButtonSelect(&FollowList[Fpi.m_CID], LineRect, m_SpectatorID == Fpi.m_CID,
									Fpi.m_pClan, Fpi.m_pName))
			{
				m_SpectatorID = Fpi.m_CID;
				Spectate(SPEC_PLAYER, m_SpectatorID);
			}

			RenderTools()->RenderTee(CAnimState::GetIdle(), &Fpi.m_TeeInfo, EMOTE_NORMAL,
									 vec2(1.0f, 0.0f), vec2(LineRect.x + 17.5f, LineRect.y + 17.5f));
		}

		UI()->ClipDisable();
	}

	// targeted player UI
	if((m_SpecMode == FREE_VIEW || m_SpecMode == OVERVIEW) && m_ClosestCharID != -1)
	{
		vec4 RingColor(1, 1, 1, 0.5); // TODO: based on target team ?
		DrawTargetHighlightWorldSpace(m_TargetPos, RingColor);
	}

	// draw cursor
	if(MouseLastMovedTime < 3.0 && ShowMouse)
	{
		Graphics()->TextureSet(g_pData->m_aImages[IMAGE_CURSOR].m_Id);
		Graphics()->QuadsBegin();
		Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
		IGraphics::CQuadItem QuadItem(mx, my, 24.0f, 24.0f);
		Graphics()->QuadsDrawTL(&QuadItem, 1);
		Graphics()->QuadsEnd();
	}
}

void CSpectator::OnReset()
{
	m_WasActive = false;
	m_Active = false;
	m_SpecMode = FREE_VIEW;
	m_SpectatorID = -1;
	m_MouseScreenPos = vec2(0, 0);
	m_MouseMoveTimer = 0;
}

void CSpectator::Spectate(int SpecMode, int SpectatorID)
{
	if(Client()->State() == IClient::STATE_DEMOPLAYBACK)
	{
		m_pClient->m_DemoSpecMode = clamp(SpecMode, 0, NUM_SPECMODES-1);
		m_pClient->m_DemoSpecID = clamp(SpectatorID, -1, MAX_CLIENTS-1);
		return;
	}

	if(m_pClient->m_Snap.m_SpecInfo.m_SpecMode == SpecMode &&
	   m_pClient->m_Snap.m_SpecInfo.m_SpectatorID == SpectatorID)
		return;

	CNetMsg_Cl_SetSpectatorMode Msg;
	Msg.m_SpecMode = SpecMode;
	Msg.m_SpectatorID = SpectatorID;
	Client()->SendPackMsg(&Msg, MSGFLAG_VITAL);
}
