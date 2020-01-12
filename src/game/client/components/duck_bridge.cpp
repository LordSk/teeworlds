#include <float.h>

#include "duck_bridge.h"

#include <game/client/animstate.h>
#include <game/client/render.h>
#include <game/client/components/skins.h>
#include <game/client/components/controls.h>
#include <game/client/components/sounds.h>
#include <game/client/components/camera.h>
#include <game/client/components/flow.h>
#include <game/client/components/effects.h>

#include <engine/external/zlib/zlib.h>
#include <engine/external/pnglite/pnglite.h>
#include <engine/storage.h>
#include <engine/sound.h>
#include <engine/textrender.h>
#include <engine/shared/network.h>
#include <engine/shared/growbuffer.h>
#include <engine/shared/config.h>
#include <engine/shared/compression.h>

#ifdef MOD_ZIPFILE
	#include <zip.h>
#endif

CMultiStackAllocator::CMultiStackAllocator()
{
	CStackBuffer StackBuffer;
	StackBuffer.m_pBuffer = (char*)mem_alloc(STACK_BUFFER_CAPACITY, 1);
	StackBuffer.m_Cursor = 0;

	m_aStacks.hint_size(8);
	m_aStacks.add(StackBuffer);
	m_CurrentStack = 0;
}

CMultiStackAllocator::~CMultiStackAllocator()
{
	const int StackBufferCount = m_aStacks.size();
	for(int s = 0; s < StackBufferCount; s++)
	{
		mem_free(m_aStacks[s].m_pBuffer);
	}
}

void* CMultiStackAllocator::Alloc(int Size)
{
	dbg_assert(Size <= STACK_BUFFER_CAPACITY, "Trying to alloc a large buffer");

	// if current stack is not full, alloc from it
	CStackBuffer& CurrentSb = m_aStacks[m_CurrentStack];
	if(CurrentSb.m_Cursor + Size <= STACK_BUFFER_CAPACITY)
	{
		int MemBlockStart = CurrentSb.m_Cursor;
		CurrentSb.m_Cursor += Size;
		return CurrentSb.m_pBuffer + MemBlockStart;
	}

	// else add a new stack if needed
	if(m_CurrentStack+1 >= m_aStacks.size())
	{
		CStackBuffer StackBuffer;
		StackBuffer.m_pBuffer = (char*)mem_alloc(STACK_BUFFER_CAPACITY, 1);
		StackBuffer.m_Cursor = 0;
		m_aStacks.add(StackBuffer);
	}

	// and try again
	m_CurrentStack++;
	return Alloc(Size);
}

void CMultiStackAllocator::Clear()
{
	const int StackBufferCount = m_aStacks.size();
	for(int s = 0; s < StackBufferCount; s++)
	{
		m_aStacks[s].m_Cursor = 0;
	}
	m_CurrentStack = 0;
}

CDuckBridge::CDuckBridge() : m_CurrentPacket(0, 0) // We have to do this, CMsgPacker can't be uninitialized apparently...
{

}

void CDuckBridge::Reset()
{
	for(int i = 0 ; i < m_aTextures.size(); i++)
	{
		Graphics()->UnloadTexture(&m_aTextures[i].m_Handle);
	}

	m_aTextures.clear();
	m_HudPartsShown = CHudPartsShown();
	m_CurrentPacketFlags = -1;

	// unload skin parts
	CSkins* pSkins = m_pClient->m_pSkins;
	for(int i = 0; i < m_aSkinPartsToUnload.size(); i++)
	{
		const CSkinPartName& spn = m_aSkinPartsToUnload[i];
		int Index = pSkins->FindSkinPart(spn.m_Type, spn.m_aName, false);
		if(Index != -1)
			pSkins->RemoveSkinPart(spn.m_Type, Index);
	}
	m_aSkinPartsToUnload.clear();

	// FIXME: unload sounds
	m_aSounds.clear();

	m_MousePos = vec2(Graphics()->ScreenWidth() * 0.5, Graphics()->ScreenHeight() * 0.5);
	m_IsMenuModeActive = false;
	m_DoUnloadModBecauseError = false;

	for(int i = 0; i < DrawSpace::_COUNT; i++)
	{
		m_aRenderCmdList[i].hint_size(1024);
		m_aRenderCmdList[i].set_size(0);
	}
}

void CDuckBridge::QueueSetColor(const float* pColor)
{
	CRenderCmd Cmd;
	Cmd.m_Type = CRenderCmd::SET_COLOR;
	mem_move(Cmd.m_Color, pColor, sizeof(Cmd.m_Color));
	m_aRenderCmdList[m_CurrentDrawSpace].add(Cmd);
}

void CDuckBridge::QueueSetTexture(int TextureID)
{
	CRenderCmd Cmd;
	Cmd.m_Type = CRenderCmd::SET_TEXTURE;
	Cmd.m_TextureID = TextureID;
	m_aRenderCmdList[m_CurrentDrawSpace].add(Cmd);
}

void CDuckBridge::QueueSetQuadSubSet(const float* pSubSet)
{
	CRenderCmd Cmd;
	Cmd.m_Type = CRenderCmd::SET_QUAD_SUBSET;
	mem_move(Cmd.m_QuadSubSet, pSubSet, sizeof(Cmd.m_QuadSubSet));
	m_aRenderCmdList[m_CurrentDrawSpace].add(Cmd);
}

void CDuckBridge::QueueSetQuadRotation(float Angle)
{
	CRenderCmd Cmd;
	Cmd.m_Type = CRenderCmd::SET_QUAD_ROTATION;
	Cmd.m_QuadRotation = Angle;
	m_aRenderCmdList[m_CurrentDrawSpace].add(Cmd);
}

void CDuckBridge::QueueSetFreeform(const IGraphics::CFreeformItem *pFreeform, int FreeformCount)
{
	IGraphics::CFreeformItem* pFreeformBuffer = (IGraphics::CFreeformItem*)m_FrameAllocator.Alloc(sizeof(IGraphics::CFreeformItem) * FreeformCount);
	mem_copy(pFreeformBuffer, pFreeform, sizeof(IGraphics::CFreeformItem) * FreeformCount);

	CRenderCmd Cmd;
	Cmd.m_Type = CRenderCmd::SET_FREEFORM_VERTICES;
	Cmd.m_pFreeformQuads = (float*)pFreeformBuffer;
	Cmd.m_FreeformQuadCount = FreeformCount;
	m_aRenderCmdList[m_CurrentDrawSpace].add(Cmd);
}

void CDuckBridge::QueueDrawFreeform(vec2 Pos)
{
	CRenderCmd Cmd;
	Cmd.m_Type = CRenderCmd::DRAW_FREEFORM;
	mem_move(Cmd.m_FreeformPos, &Pos, sizeof(float)*2);
	m_aRenderCmdList[m_CurrentDrawSpace].add(Cmd);
}

void CDuckBridge::QueueDrawText(const char *pStr, float FontSize, float LineWidth, float* pPos, float* pClip, float* pColors)
{
	CRenderCmd Cmd;
	Cmd.m_Type = CRenderCmd::DRAW_TEXT;

	// save string on frame allocator
	int Len = min(str_length(pStr), 4096); // reasonable limit
	char* pCopy = (char*)m_FrameAllocator.Alloc(Len+1);
	str_copy(pCopy, pStr, 4096);
	pCopy[Len] = 0;

	Cmd.m_Text.m_pStr = pCopy;
	mem_move(Cmd.m_Text.m_aColors, pColors, sizeof(float)*4);
	Cmd.m_Text.m_FontSize = FontSize;
	Cmd.m_Text.m_LineWidth = LineWidth;
	mem_move(Cmd.m_Text.m_aPos, pPos, sizeof(float)*2);
	mem_move(Cmd.m_Text.m_aClip, pClip, sizeof(float)*4);

	m_aRenderCmdList[m_CurrentDrawSpace].add(Cmd);
}

void CDuckBridge::QueueDrawCircle(vec2 Pos, float Radius)
{
	const int Polys = 32;
	IGraphics::CFreeformItem Quads[Polys];

	for(int p = 0; p < Polys; p++)
	{
		IGraphics::CFreeformItem& q = Quads[p];
		q.m_X0 = cosf((p/(float)Polys) * 2 * pi) * Radius;
		q.m_Y0 = sinf((p/(float)Polys) * 2 * pi) * Radius;
		q.m_X1 = 0;
		q.m_Y1 = 0;
		q.m_X2 = cosf(((p+1)/(float)Polys) * 2 * pi) * Radius;
		q.m_Y2 = sinf(((p+1)/(float)Polys) * 2 * pi) * Radius;
		q.m_X3 = 0;
		q.m_Y3 = 0;
	}

	QueueSetFreeform(Quads, Polys);
	QueueDrawFreeform(Pos);
}

void CDuckBridge::QueueDrawLine(vec2 Pos1, vec2 Pos2, float Thickness)
{
	vec2 Dir = normalize(Pos2 - Pos1);
	float Angle = angle(Dir);
	QueueSetQuadRotation(Angle);
	float Width = distance(Pos1, Pos2);
	float Height = Thickness;
	vec2 Center = (Pos1 + Pos2) / 2;
	QueueDrawQuadCentered(IGraphics::CQuadItem(Center.x, Center.y, Width, Height));
}

void CDuckBridge::SetHudPartsShown(CHudPartsShown hps)
{
	m_HudPartsShown = hps;
}

void CDuckBridge::QueueDrawQuad(IGraphics::CQuadItem Quad)
{
	CRenderCmd Cmd;
	Cmd.m_Type = CDuckBridge::CRenderCmd::DRAW_QUAD;
	mem_move(Cmd.m_Quad, &Quad, sizeof(Cmd.m_Quad)); // yep, this is because we don't have c++11
	m_aRenderCmdList[m_CurrentDrawSpace].add(Cmd);
}

void CDuckBridge::QueueDrawQuadCentered(IGraphics::CQuadItem Quad)
{
	CRenderCmd Cmd;
	Cmd.m_Type = CDuckBridge::CRenderCmd::DRAW_QUAD_CENTERED;
	mem_move(Cmd.m_Quad, &Quad, sizeof(Cmd.m_Quad));
	m_aRenderCmdList[m_CurrentDrawSpace].add(Cmd);
}

bool CDuckBridge::LoadTexture(const char *pTexturePath, const char* pTextureName)
{
	int Len = str_length(pTextureName);
	if(Len < 5) // .png
		return false;

	IGraphics::CTextureHandle Handle = Graphics()->LoadTexture(pTexturePath, IStorage::TYPE_SAVE, CImageInfo::FORMAT_AUTO, 0);
	uint32_t Hash = hash_fnv1a(pTextureName, Len - 4);
	CTextureHashPair Pair = { Hash, Handle };
	m_aTextures.add(Pair);
	return Handle.IsValid();
}

struct CFileBuffer
{
	const char* m_pData;
	int m_FileSize;
	int m_Cursor;

	CFileBuffer()
	{
		m_pData = 0;
		m_FileSize = 0;
		m_Cursor = 0;
	}
};

static unsigned PngReadCallback(void* output, unsigned long size, unsigned long numel, void* user_pointer)
{
	CFileBuffer& FileBuff = *(CFileBuffer*)user_pointer;
	const int ReadSize = size * numel;

	if(output)
	{
		if(FileBuff.m_Cursor + ReadSize > FileBuff.m_FileSize)
			return 0;

		mem_move(output, FileBuff.m_pData + FileBuff.m_Cursor, ReadSize);
	}

	FileBuff.m_Cursor += ReadSize;
	return ReadSize;
}

static bool OpenPNGRaw(const char *pFileData, int FileSize, CImageInfo *pImg)
{
	char aCompleteFilename[512];
	unsigned char *pBuffer;
	png_t Png; // ignore_convention

	// open file for reading
	png_init(0,0); // ignore_convention

	CFileBuffer FileBuff;
	FileBuff.m_pData = pFileData;
	FileBuff.m_FileSize = FileSize;
	int Error = png_open(&Png, PngReadCallback, &FileBuff);

	if(Error != PNG_NO_ERROR)
	{
		dbg_msg("game/png", "failed to open file. filename='%s'", aCompleteFilename);
		return false;
	}

	if(Png.depth != 8 || (Png.color_type != PNG_TRUECOLOR && Png.color_type != PNG_TRUECOLOR_ALPHA) || Png.width > (2<<12) || Png.height > (2<<12)) // ignore_convention
	{
		dbg_msg("game/png", "invalid format. filename='%s'", aCompleteFilename);
		return false;
	}

	pBuffer = (unsigned char *)mem_alloc(Png.width * Png.height * Png.bpp, 1); // ignore_convention
	png_get_data(&Png, pBuffer); // ignore_convention

	pImg->m_Width = Png.width; // ignore_convention
	pImg->m_Height = Png.height; // ignore_convention
	if(Png.color_type == PNG_TRUECOLOR) // ignore_convention
		pImg->m_Format = CImageInfo::FORMAT_RGB;
	else if(Png.color_type == PNG_TRUECOLOR_ALPHA) // ignore_convention
		pImg->m_Format = CImageInfo::FORMAT_RGBA;
	pImg->m_pData = pBuffer;
	return true;
}

bool CDuckBridge::LoadTextureRaw(const char* pTextureName, const char *pFileData, int FileSize)
{
	// load it
	IGraphics::CTextureHandle Handle;
	const IGraphics::CTextureHandle InvalidTexture;
	CImageInfo Img;

	if(OpenPNGRaw(pFileData, FileSize, &Img))
	{
		Handle = Graphics()->LoadTextureRaw(Img.m_Width, Img.m_Height, Img.m_Format, Img.m_pData, CImageInfo::FORMAT_AUTO, 0);
		mem_free(Img.m_pData);

		if(Handle.Id() != InvalidTexture.Id() && g_Config.m_Debug)
			dbg_msg("graphics/texture", "loaded %s", pTextureName);

		if(Handle.Id() == InvalidTexture.Id())
		{
			dbg_msg("graphics/texture", "failed to load texture %s", pTextureName);
			return false;
		}
	}

	// save pair
	int Len = str_length(pTextureName);
	if(Len < 5) // .png
		return false;

	uint32_t Hash = hash_fnv1a(pTextureName, Len - 4);
	CTextureHashPair Pair = { Hash, Handle };
	m_aTextures.add(Pair);
	return Handle.IsValid();
}

IGraphics::CTextureHandle CDuckBridge::GetTextureFromName(const char *pTextureName)
{
	const uint32_t SearchHash = hash_fnv1a(pTextureName, str_length(pTextureName));

	const CTextureHashPair* Pairs = m_aTextures.base_ptr();
	const int PairCount = m_aTextures.size();

	for(int i = 0; i < PairCount; i++)
	{
		if(Pairs[i].m_Hash == SearchHash)
			return Pairs[i].m_Handle;
	}

	return IGraphics::CTextureHandle();
}

void CDuckBridge::PacketCreate(int NetID, int Flags)
{
	NetID = max(NetID, 0);
	// manual contructor here needed
	m_CurrentPacket.Reset();
	m_CurrentPacket.AddInt((NETMSG_DUCK_NETOBJ) << 1 | 1);
	m_CurrentPacket.AddInt(NetID);
	m_CurrentPacketFlags = Flags;
}

void CDuckBridge::PacketPackFloat(float f)
{
	if(m_CurrentPacketFlags == -1) {
		dbg_msg("duck", "ERROR: can't add float to undefined packet");
		return;
	}

	m_CurrentPacket.AddInt(f * 256);
}

void CDuckBridge::PacketPackInt(int i)
{
	if(m_CurrentPacketFlags == -1) {
		dbg_msg("duck", "ERROR: can't add int to undefined packet");
		return;
	}

	m_CurrentPacket.AddInt(i);
}

void CDuckBridge::PacketPackString(const char *pStr, int SizeLimit)
{
	if(m_CurrentPacketFlags == -1) {
		dbg_msg("duck", "ERROR: can't add string to undefined packet");
		return;
	}

	SizeLimit = clamp(SizeLimit, 0, 1024); // reasonable limits
	m_CurrentPacket.AddString(pStr, SizeLimit);
}

void CDuckBridge::SendPacket()
{
	if(m_CurrentPacketFlags == -1) {
		dbg_msg("duck", "ERROR: can't send undefined packet");
		return;
	}

	Client()->SendMsg(&m_CurrentPacket, m_CurrentPacketFlags);
	m_CurrentPacket.Reset();
	m_CurrentPacketFlags = -1;
}

void CDuckBridge::AddSkinPart(const char *pPart, const char *pName, IGraphics::CTextureHandle Handle)
{
	int Type = -1;
	if(str_comp(pPart, "body") == 0) {
		Type = SKINPART_BODY;
	}
	else if(str_comp(pPart, "marking") == 0) {
		Type = SKINPART_MARKING;
	}
	else if(str_comp(pPart, "decoration") == 0) {
		Type = SKINPART_MARKING;
	}
	else if(str_comp(pPart, "hands") == 0) {
		Type = SKINPART_HANDS;
	}
	else if(str_comp(pPart, "feet") == 0) {
		Type = SKINPART_FEET;
	}
	else if(str_comp(pPart, "eyes") == 0) {
		Type = SKINPART_EYES;
	}

	if(Type == -1) {
		return; // part type not found
	}

	CSkins::CSkinPart SkinPart;
	SkinPart.m_OrgTexture = Handle;
	SkinPart.m_ColorTexture = Handle;
	SkinPart.m_BloodColor = vec3(1.0f, 0.0f, 0.0f);
	SkinPart.m_Flags = 0;

	char aPartName[256];
	str_truncate(aPartName, sizeof(aPartName), pName, str_length(pName) - 4);
	str_copy(SkinPart.m_aName, aPartName, sizeof(SkinPart.m_aName));

	m_pClient->m_pSkins->AddSkinPart(Type, SkinPart);

	CSkinPartName SkinPartName;
	str_copy(SkinPartName.m_aName, SkinPart.m_aName, sizeof(SkinPartName.m_aName));
	SkinPartName.m_Type = Type;
	m_aSkinPartsToUnload.add(SkinPartName);
	// FIXME: breaks loaded "skins" (invalidates indexes)
}

void CDuckBridge::AddWeapon(const CWeaponCustomJs &WcJs)
{
	// TODO: remove
#if 0
	if(WcJs.WeaponID < NUM_WEAPONS || WcJs.WeaponID >= NUM_WEAPONS_DUCK)
	{
		dbg_msg("duck", "ERROR: AddWeapon() :: Weapon ID = %d out of bounds", WcJs.WeaponID);
		return;
	}

	/*const int Count = m_aWeapons.size();
	for(int i = 0; i < Count; i++)
	{
		if(m_aWeapons[i].WeaponID == WcJs.WeaponID)
		{
			dbg_msg("duck", "ERROR: AddWeapon() :: Weapon with ID = %d exists already", WcJs.WeaponID);
			return;
		}
	}*/

	CWeaponCustom WcFind;
	WcFind.WeaponID = WcJs.WeaponID;
	plain_range_sorted<CWeaponCustom> Found = find_binary(m_aWeapons.all(), WcFind);
	if(!Found.empty())
	{
		dbg_msg("duck", "ERROR: AddWeapon() :: Weapon with ID = %d exists already", WcJs.WeaponID);
		// TODO: generate a WARNING?
		return;
	}

	IGraphics::CTextureHandle TexWeaponHandle, TexCursorHandle;

	if(WcJs.aTexWeapon[0])
	{
		TexWeaponHandle = GetTextureFromName(WcJs.aTexWeapon);
		if(!TexWeaponHandle.IsValid())
		{
			dbg_msg("duck", "ERROR: AddWeapon() :: Weapon texture '%s' not found", WcJs.aTexWeapon);
			return;
		}
	}

	if(WcJs.aTexCursor[0])
	{
		TexCursorHandle = GetTextureFromName(WcJs.aTexCursor);
		if(!TexCursorHandle.IsValid())
		{
			dbg_msg("duck", "ERROR: AddWeapon() :: Cursor texture '%s' not found", WcJs.aTexCursor);
			return;
		}
	}


	CWeaponCustom Wc;
	Wc.WeaponID = WcJs.WeaponID;
	Wc.WeaponPos = vec2(WcJs.WeaponX, WcJs.WeaponY);
	Wc.WeaponSize = vec2(WcJs.WeaponSizeX, WcJs.WeaponSizeY);
	Wc.TexWeaponHandle = TexWeaponHandle;
	Wc.TexCursorHandle = TexCursorHandle;
	Wc.HandPos = vec2(WcJs.HandX, WcJs.HandY);
	Wc.HandAngle = WcJs.HandAngle;
	Wc.Recoil = WcJs.Recoil;

	m_aWeapons.add(Wc);
#endif
}

CDuckBridge::CWeaponCustom *CDuckBridge::FindWeapon(int WeaponID)
{
	CWeaponCustom WcFind;
	WcFind.WeaponID = WeaponID;
	plain_range_sorted<CWeaponCustom> Found = find_binary(m_aWeapons.all(), WcFind);
	if(Found.empty())
		return 0;

	return &Found.front();
}

void CDuckBridge::PlaySoundAt(const char *pSoundName, float x, float y)
{
	if(!g_Config.m_SndEnable)
		return;

	int Len = str_length(pSoundName);
	const uint32_t Hash = hash_fnv1a(pSoundName, Len);
	const int SoundCount = m_aSounds.size();

	int ID = -1;
	for(int i = 0; i < SoundCount; i++)
	{
		if(m_aSounds[i].m_Hash == Hash)
		{
			ID = i;
			break;
		}
	}

	if(ID == -1)
	{
		dbg_msg("duck", "WARNING: PlaySoundAt('%s'), sound not found", pSoundName);
		return;
	}

	Sound()->PlayAt(CSounds::CHN_WORLD, m_aSounds[ID].m_Handle, 0, x, y);
}

void CDuckBridge::PlaySoundGlobal(const char *pSoundName)
{
	if(!g_Config.m_SndEnable)
		return;

	int Len = str_length(pSoundName);
	const uint32_t Hash = hash_fnv1a(pSoundName, Len);
	const int SoundCount = m_aSounds.size();

	int ID = -1;
	for(int i = 0; i < SoundCount; i++)
	{
		if(m_aSounds[i].m_Hash == Hash)
		{
			ID = i;
			break;
		}
	}

	if(ID == -1)
	{
		dbg_msg("duck", "WARNING: PlaySoundGlobal('%s'), sound not found", pSoundName);
		return;
	}

	Sound()->Play(CSounds::CHN_WORLD, m_aSounds[ID].m_Handle, 0);
}

void CDuckBridge::PlayMusic(const char *pSoundName)
{
	int Len = str_length(pSoundName);
	const uint32_t Hash = hash_fnv1a(pSoundName, Len);
	const int SoundCount = m_aSounds.size();

	int ID = -1;
	for(int i = 0; i < SoundCount; i++)
	{
		if(m_aSounds[i].m_Hash == Hash)
		{
			ID = i;
			break;
		}
	}

	if(ID == -1)
	{
		dbg_msg("duck", "WARNING: PlayMusic('%s'), sound not found", pSoundName);
		return;
	}

	Sound()->Play(CSounds::CHN_MUSIC, m_aSounds[ID].m_Handle, ISound::FLAG_LOOP);
}

CUIRect CDuckBridge::GetUiScreenRect() const
{
	return *UI()->Screen();
}

vec2 CDuckBridge::GetScreenSize() const
{
	return vec2(Graphics()->ScreenWidth(), Graphics()->ScreenHeight());
}

vec2 CDuckBridge::GetPixelScale() const
{
	float OriScreenX0, OriScreenY0, OriScreenX1, OriScreenY1;
	float ScreenX0, ScreenY0, ScreenX1, ScreenY1;
	Graphics()->GetScreen(&OriScreenX0, &OriScreenY0, &OriScreenX1, &OriScreenY1);

	float MappedScreenWidth;
	float MappedScreenHeight;

	switch(m_CurrentDrawSpace)
	{
		case DrawSpace::GAME:
		case DrawSpace::GAME_FOREGROUND:
		{
			CMapItemGroup Group;
			Group.m_OffsetX = 0;
			Group.m_OffsetY = 0;
			Group.m_ParallaxX = 100;
			Group.m_ParallaxY = 100;
			Group.m_UseClipping = false;
			RenderTools()->MapScreenToGroup(0, 0, &Group, GetCameraZoom());

			float ScreenX0, ScreenY0, ScreenX1, ScreenY1;
			Graphics()->GetScreen(&ScreenX0, &ScreenY0, &ScreenX1, &ScreenY1);
			MappedScreenWidth = ScreenX1-ScreenX0;
			MappedScreenHeight = ScreenY1-ScreenY0;

		} break;

		case DrawSpace::HUD:
		{
			CUIRect Rect = *UI()->Screen();
			MappedScreenWidth = Rect.w;
			MappedScreenHeight = Rect.h;
		} break;

		default:
			dbg_assert(0, "case not handled");
			break;
	}

	// restore screen
	Graphics()->MapScreen(OriScreenX0, OriScreenY0, OriScreenX1, OriScreenY1);

	float FakeToScreenX = Graphics()->ScreenWidth()/MappedScreenWidth;
	float FakeToScreenY = Graphics()->ScreenHeight()/MappedScreenHeight;
	return vec2(1.0/FakeToScreenX, 1.0/FakeToScreenY);
}

vec2 CDuckBridge::GetCameraPos() const
{
	return *m_pClient->m_pCamera->GetCenter();
}

float CDuckBridge::GetCameraZoom() const
{
	return m_pClient->m_pCamera->GetZoom();
}

vec2 CDuckBridge::GetUiMousePos() const
{
	vec2 Pos = m_MousePos;
	Pos.x = clamp(Pos.x, 0.0f, (float)Graphics()->ScreenWidth());
	Pos.y = clamp(Pos.y, 0.0f, (float)Graphics()->ScreenHeight());
	CUIRect Rect = *UI()->Screen();
	Graphics()->MapScreen(0.0f, 0.0f, Rect.w, Rect.h);
	Pos.x *= Rect.w / Graphics()->ScreenWidth();
	Pos.y *= Rect.h / Graphics()->ScreenHeight();
	return Pos;
}

int CDuckBridge::GetBaseTextureHandle(int ImgID) const
{
	ImgID = clamp(ImgID, 0, NUM_IMAGES-1);
	return *(int*)&g_pData->m_aImages[ImgID].m_Id;
}

static void GetSpriteSubSet(const CDataSprite& Spr, float* pOutSubSet)
{
	int x = Spr.m_X;
	int y = Spr.m_Y;
	int w = Spr.m_W;
	int h = Spr.m_H;
	int cx = Spr.m_pSet->m_Gridx;
	int cy = Spr.m_pSet->m_Gridy;

	float x1 = x/(float)cx;
	float x2 = (x+w-1/32.0f)/(float)cx;
	float y1 = y/(float)cy;
	float y2 = (y+h-1/32.0f)/(float)cy;

	pOutSubSet[0] = x1;
	pOutSubSet[1] = y1;
	pOutSubSet[2] = x2;
	pOutSubSet[3] = y2;
}

void CDuckBridge::GetBaseSpritSubset(int SpriteID, float* pSubSet) const
{
	SpriteID = clamp(SpriteID, 0, NUM_SPRITES-1);
	CDataSprite Spr = g_pData->m_aSprites[SpriteID];
	GetSpriteSubSet(Spr, pSubSet);
}

void CDuckBridge::GetBaseSpritScale(int SpriteID, float *pOutScale) const
{
	SpriteID = clamp(SpriteID, 0, NUM_SPRITES-1);
	CDataSprite Spr = g_pData->m_aSprites[SpriteID];
	int x = Spr.m_X;
	int y = Spr.m_Y;
	int w = Spr.m_W;
	int h = Spr.m_H;

	float f = sqrtf(h*h + w*w);
	float ScaleW = w/f;
	float ScaleH = h/f;
	pOutScale[0] = ScaleW;
	pOutScale[1] = ScaleH;
}

bool CDuckBridge::GetSkinPart(int PartID, const char *pPartName, IGraphics::CTextureHandle *pOrgText, IGraphics::CTextureHandle *pColorText) const
{
	int SkinPartID = m_pClient->m_pSkins->FindSkinPart(PartID, pPartName, true);
	if(SkinPartID < 0)
		return false;

	const CSkins::CSkinPart* pSkinPart = m_pClient->m_pSkins->GetSkinPart(PartID, SkinPartID);
	*pColorText = pSkinPart->m_OrgTexture;
	*pOrgText = pSkinPart->m_ColorTexture;
	return true;
}

vec2 CDuckBridge::GetLocalCursorPos() const
{
	return m_pClient->m_pControls->m_TargetPos;
}

void CDuckBridge::SetMenuModeActive(bool Active)
{
	m_IsMenuModeActive = Active;
}

vec2 CDuckBridge::CalculateTextSize(const char *pStr, float FontSize, float LineWidth)
{
	float ScreenX0, ScreenY0, ScreenX1, ScreenY1;
	Graphics()->GetScreen(&ScreenX0, &ScreenY0, &ScreenX1, &ScreenY1);

	switch(m_CurrentDrawSpace)
	{
		case DrawSpace::GAME:
		case DrawSpace::GAME_FOREGROUND:
		{
			CMapItemGroup Group;
			Group.m_OffsetX = 0;
			Group.m_OffsetY = 0;
			Group.m_ParallaxX = 100;
			Group.m_ParallaxY = 100;
			Group.m_UseClipping = false;
			RenderTools()->MapScreenToGroup(0, 0, &Group, GetCameraZoom());
		} break;

		case DrawSpace::HUD:
		{
			CUIRect Rect = *UI()->Screen();
			Graphics()->MapScreen(0.0f, 0.0f, Rect.w, Rect.h);
		} break;

		default:
			dbg_assert(0, "case not handled");
			break;
	}

	float aRect[4] = {FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX};
	CTextCursor Cursor;
	TextRender()->SetCursor(&Cursor, 0, 0, FontSize, TEXTFLAG_RENDER|TEXTFLAG_ALLOW_NEWLINE);
	Cursor.m_LineWidth = LineWidth;
	TextRender()->TextCalculateRect(&Cursor, pStr, -1, aRect);

	const float BaseLine = TextRender()->TextGetLineBaseY(&Cursor);

	// TODO: is this even useful at all?
	Graphics()->MapScreen(ScreenX0, ScreenY0, ScreenX1, ScreenY1); // restore screen

	float Width = aRect[2]-aRect[0];
	if(LineWidth > 0)
		Width = min(Width, LineWidth);

	float Height = max(aRect[3]-aRect[1], BaseLine);

	return vec2(Width, Height);
}

void CDuckBridge::ScriptError(int ErrorLevel, const char *format, ...)
{
	char aBuffer[1024];

	va_list ap;
	va_start(ap, format);
	int Len = vsnprintf(aBuffer, sizeof(aBuffer), format, ap);
	va_end(ap);

	aBuffer[Len] = 0;

	const char* pErrorStr = "WARNING";
	if(ErrorLevel == JsErrorLvl::ERROR)
		pErrorStr = "ERROR";
	else if(ErrorLevel == JsErrorLvl::CRITICAL)
		pErrorStr = "CRITICAL";

	dbg_msg("script", "[%s] %.*s", pErrorStr, Len, aBuffer);

	// send error to server
	PacketCreate(0x10001, MSGFLAG_VITAL|MSGFLAG_FLUSH);
	PacketPackInt(ErrorLevel);
	PacketPackString(aBuffer, sizeof(aBuffer));
	SendPacket();

	CJsErrorStr ErrStr;
	str_copy(ErrStr.m_aText, aBuffer, sizeof(ErrStr.m_aText));
	ErrStr.m_Level = ErrorLevel;
	ErrStr.m_Time = Client()->LocalTime();
	m_aJsErrors.add(ErrStr);

	// if the error is more serious than a warning, go to spectators
	// TODO: maybe disconnect instead?
	if(ErrorLevel > JsErrorLvl::WARNING)
	{
		m_pClient->SendSwitchTeam(-1);
		m_DoUnloadModBecauseError = true;

#ifdef CONF_DEBUG
		dbg_break();
#endif
	}
}

bool CDuckBridge::RenderSetDrawSpace(int Space)
{
	if(Space < 0 || Space >= DrawSpace::_COUNT)
		return false;

	m_CurrentDrawSpace = Space;
	return true;
}

void CDuckBridge::RenderDrawSpace(int Space)
{
	dbg_assert(Space >=0 && Space < DrawSpace::_COUNT, "Space is out of bounds");

	const int CmdCount = m_aRenderCmdList[Space].size();
	const CRenderCmd* aCmds = m_aRenderCmdList[Space].base_ptr();

	// TODO: merge CRenderSpace and DrawSpace
	CRenderSpace& RenderSpace = m_aRenderSpace[Space];
	float* pWantColor = RenderSpace.m_aWantColor;
	float* pWantQuadSubSet = RenderSpace.m_aWantQuadSubSet;

	int FakeID = -12345;
	IGraphics::CTextureHandle FakeTexture = *(IGraphics::CTextureHandle*)&FakeID;
	IGraphics::CTextureHandle CurrentTexture;
	int CurrentTextureWrap = CRenderSpace::WRAP_CLAMP;

	int FlushCount = 0;

	Graphics()->TextureClear();
	Graphics()->WrapClamp();
	Graphics()->QuadsBegin();

	for(int i = 0; i < CmdCount; i++)
	{
		const CRenderCmd& Cmd = aCmds[i];

		switch(Cmd.m_Type)
		{
			case CRenderCmd::SET_COLOR: {
				mem_move(pWantColor, Cmd.m_Color, sizeof(Cmd.m_Color));
			} break;

			case CRenderCmd::SET_TEXTURE: {
				RenderSpace.m_WantTextureID = Cmd.m_TextureID;
			} break;

			case CRenderCmd::SET_TEXTURE_WRAP: {
				RenderSpace.m_TextureWrap = Cmd.m_TextureWrap;
			} break;

			case CRenderCmd::SET_QUAD_SUBSET: {
				mem_move(pWantQuadSubSet, Cmd.m_QuadSubSet, sizeof(Cmd.m_QuadSubSet));
			} break;

			case CRenderCmd::SET_QUAD_ROTATION: {
				RenderSpace.m_WantQuadRotation = Cmd.m_QuadRotation;
			} break;

			case CRenderCmd::SET_FREEFORM_VERTICES: {
				RenderSpace.m_FreeformQuadCount = min(Cmd.m_FreeformQuadCount, CRenderSpace::FREEFORM_MAX_COUNT-1);
				mem_move(RenderSpace.m_aFreeformQuads, Cmd.m_pFreeformQuads, RenderSpace.m_FreeformQuadCount * sizeof(RenderSpace.m_aFreeformQuads[0]));
			} break;

			case CRenderCmd::DRAW_QUAD_CENTERED:
			case CRenderCmd::DRAW_QUAD:
			case CRenderCmd::DRAW_FREEFORM: {
				if(CurrentTexture.Id() != RenderSpace.m_WantTextureID)
				{
					Graphics()->QuadsEnd(); // Flush
					FlushCount++;

					if(RenderSpace.m_WantTextureID < 0)
					{
						Graphics()->TextureClear();
						CurrentTexture.Invalidate();
					}
					else
					{
						Graphics()->TextureSet(*(IGraphics::CTextureHandle*)&RenderSpace.m_WantTextureID);
						CurrentTexture = *(IGraphics::CTextureHandle*)&RenderSpace.m_WantTextureID;
					}

					Graphics()->QuadsBegin();
				}

				if(CurrentTextureWrap != RenderSpace.m_TextureWrap)
				{
					CurrentTextureWrap = RenderSpace.m_TextureWrap;

					if(CurrentTextureWrap == CRenderSpace::WRAP_CLAMP)
						Graphics()->WrapClamp();
					else
						Graphics()->WrapNormal();
				}

				Graphics()->SetColor(pWantColor[0] * pWantColor[3], pWantColor[1] * pWantColor[3], pWantColor[2] * pWantColor[3], pWantColor[3]);

				Graphics()->QuadsSetSubset(pWantQuadSubSet[0], pWantQuadSubSet[1], pWantQuadSubSet[2], pWantQuadSubSet[3]);

				Graphics()->QuadsSetRotation(RenderSpace.m_WantQuadRotation);
				RenderSpace.m_WantQuadRotation = 0; // reset here

				if(Cmd.m_Type == CRenderCmd::DRAW_QUAD_CENTERED)
					Graphics()->QuadsDraw((IGraphics::CQuadItem*)&Cmd.m_Quad, 1);
				else if(Cmd.m_Type == CRenderCmd::DRAW_FREEFORM)
				{
					// TODO: is the position even useful here?
					IGraphics::CFreeformItem aTransFreeform[CRenderSpace::FREEFORM_MAX_COUNT];
					const vec2 FfPos(Cmd.m_FreeformPos[0], Cmd.m_FreeformPos[1]);

					// transform freeform object based on position
					for(int f = 0; f < RenderSpace.m_FreeformQuadCount; f++)
					{
						IGraphics::CFreeformItem& ff = aTransFreeform[f];
						IGraphics::CFreeformItem& rff = RenderSpace.m_aFreeformQuads[f];
						ff.m_X0 = rff.m_X0 + FfPos.x;
						ff.m_X1 = rff.m_X1 + FfPos.x;
						ff.m_X2 = rff.m_X2 + FfPos.x;
						ff.m_X3 = rff.m_X3 + FfPos.x;
						ff.m_Y0 = rff.m_Y0 + FfPos.y;
						ff.m_Y1 = rff.m_Y1 + FfPos.y;
						ff.m_Y2 = rff.m_Y2 + FfPos.y;
						ff.m_Y3 = rff.m_Y3 + FfPos.y;
					}
					Graphics()->QuadsDrawFreeform(aTransFreeform, RenderSpace.m_FreeformQuadCount);
				}
				else
					Graphics()->QuadsDrawTL((IGraphics::CQuadItem*)&Cmd.m_Quad, 1);
			} break;

			case CRenderCmd::DRAW_TEXT:
			{
				Graphics()->QuadsEnd(); // Flush
				CurrentTexture = FakeTexture;

				const CTextInfo& Text = Cmd.m_Text;
				const bool DoClipping = Text.m_aClip[2] > 0 && Text.m_aClip[3] > 0;

				// clip
				if(DoClipping)
				{
					float Points[4];
					Graphics()->GetScreen(&Points[0], &Points[1], &Points[2], &Points[3]);
					float x0 = (Text.m_aClip[0] - Points[0]) / (Points[2]-Points[0]);
					float y0 = (Text.m_aClip[1] - Points[1]) / (Points[3]-Points[1]);
					float x1 = ((Text.m_aClip[0]+Text.m_aClip[2]) - Points[0]) / (Points[2]-Points[0]);
					float y1 = ((Text.m_aClip[1]+Text.m_aClip[3]) - Points[1]) / (Points[3]-Points[1]);

					if(x1 < 0.0f || x0 > 1.0f || y1 < 0.0f || y0 > 1.0f)
						continue;

					Graphics()->ClipEnable((int)(x0*Graphics()->ScreenWidth()), (int)(y0*Graphics()->ScreenHeight()),
						(int)((x1-x0)*Graphics()->ScreenWidth()), (int)((y1-y0)*Graphics()->ScreenHeight()));
				}


				float PosX = Text.m_aPos[0];
				float PosY = Text.m_aPos[1];

				CTextCursor Cursor;
				TextRender()->SetCursor(&Cursor, PosX, PosY, Text.m_FontSize, TEXTFLAG_RENDER|TEXTFLAG_ALLOW_NEWLINE);
				Cursor.m_LineWidth = Text.m_LineWidth;

				vec4 TextColor(Text.m_aColors[0], Text.m_aColors[1], Text.m_aColors[2], Text.m_aColors[3]);
				vec4 ShadowColor(0, 0, 0, 0);
				TextRender()->TextShadowed(&Cursor, Text.m_pStr, -1, vec2(0,0), ShadowColor, TextColor);

				if(DoClipping)
					Graphics()->ClipDisable();

				Graphics()->QuadsBegin();
			} break;

			default:
				dbg_assert(0, "Render command type not handled");
		}
	}

	Graphics()->QuadsEnd(); // flush
	Graphics()->WrapNormal();
	FlushCount++;

	//dbg_msg("duck", "flush count = %d", FlushCount);

	m_aRenderCmdList[Space].set_size(0);
	RenderSpace = CRenderSpace();
}

void CDuckBridge::CharacterCorePreTick(CCharacterCore** apCharCores)
{
#if 0
	if(!IsLoaded())
		return;

	duk_context* pCtx = m_Backend.Ctx();

	if(!m_Backend.GetJsFunction("OnCharacterCorePreTick")) {
		return;
	}

	// arguments (array[CCharacterCore object], array[CNetObj_PlayerInput object])
	duk_idx_t ArrayCharCoresIdx = duk_push_array(pCtx);
	for(int c = 0; c < MAX_CLIENTS; c++)
	{
		if(apCharCores[c])
			DuktapePushCharacterCore(pCtx, apCharCores[c]);
		else
			duk_push_null(pCtx);
		duk_put_prop_index(pCtx, ArrayCharCoresIdx, c);
	}

	duk_idx_t ArrayInputIdx = duk_push_array(pCtx);
	for(int c = 0; c < MAX_CLIENTS; c++)
	{
		if(apCharCores[c])
			DuktapePushNetObjPlayerInput(pCtx, &apCharCores[c]->m_Input);
		else
			duk_push_null(pCtx);
		duk_put_prop_index(pCtx, ArrayInputIdx, c);
	}

	m_Backend.CallJsFunction(2);

	if(!m_Backend.HasJsFunctionReturned()) {
		duk_pop(pCtx);
		return;
	}

	// EXPECTS RETURN: [array[CCharacterCore object], array[CNetObj_PlayerInput object]]
	if(duk_get_prop_index(pCtx, -1, 0))
	{
		for(int c = 0; c < MAX_CLIENTS; c++)
		{
			if(duk_get_prop_index(pCtx, -1, c))
			{
				if(!duk_is_null(pCtx, -1))
					DuktapeReadCharacterCore(pCtx, -1, apCharCores[c]);
				duk_pop(pCtx);
			}
		}
		duk_pop(pCtx);
	}
	if(duk_get_prop_index(pCtx, -1, 1))
	{
		for(int c = 0; c < MAX_CLIENTS; c++)
		{
			if(duk_get_prop_index(pCtx, -1, c))
			{
				if(!duk_is_null(pCtx, -1))
					DuktapeReadNetObjPlayerInput(pCtx, -1, &apCharCores[c]->m_Input);
				duk_pop(pCtx);
			}
		}
		duk_pop(pCtx);
	}

	duk_pop(pCtx);
#endif
}

void CDuckBridge::CharacterCorePostTick(CCharacterCore** apCharCores)
{
#if 0
	if(!IsLoaded())
		return;

	duk_context* pCtx = m_Backend.Ctx();

	if(!m_Backend.GetJsFunction("OnCharacterCorePostTick")) {
		return;
	}

	// arguments (array[CCharacterCore object], array[CNetObj_PlayerInput object])
	duk_idx_t ArrayCharCoresIdx = duk_push_array(pCtx);
	for(int c = 0; c < MAX_CLIENTS; c++)
	{
		if(apCharCores[c])
			DuktapePushCharacterCore(pCtx, apCharCores[c]);
		else
			duk_push_null(pCtx);
		duk_put_prop_index(pCtx, ArrayCharCoresIdx, c);
	}

	duk_idx_t ArrayInputIdx = duk_push_array(pCtx);
	for(int c = 0; c < MAX_CLIENTS; c++)
	{
		if(apCharCores[c])
			DuktapePushNetObjPlayerInput(pCtx, &apCharCores[c]->m_Input);
		else
			duk_push_null(pCtx);
		duk_put_prop_index(pCtx, ArrayInputIdx, c);
	}

	m_Backend.CallJsFunction(2);

	if(!m_Backend.HasJsFunctionReturned()) {
		duk_pop(pCtx);
		return;
	}

	// EXPECTS RETURN: [array[CCharacterCore object], array[CNetObj_PlayerInput object]]
	if(duk_get_prop_index(pCtx, -1, 0))
	{
		for(int c = 0; c < MAX_CLIENTS; c++)
		{
			if(duk_get_prop_index(pCtx, -1, c))
			{
				if(!duk_is_null(pCtx, -1))
					DuktapeReadCharacterCore(pCtx, -1, apCharCores[c]);
				duk_pop(pCtx);
			}
		}
		duk_pop(pCtx);
	}
	if(duk_get_prop_index(pCtx, -1, 1))
	{
		for(int c = 0; c < MAX_CLIENTS; c++)
		{
			if(duk_get_prop_index(pCtx, -1, c))
			{
				if(!duk_is_null(pCtx, -1))
					DuktapeReadNetObjPlayerInput(pCtx, -1, &apCharCores[c]->m_Input);
				duk_pop(pCtx);
			}
		}
		duk_pop(pCtx);
	}

	duk_pop(pCtx);
#endif
}

void CDuckBridge::Predict(CWorldCore* pWorld)
{
	// TODO: fix this
}

void CDuckBridge::RenderPlayerWeapon(int WeaponID, vec2 Pos, float AttachAngle, float Angle, CTeeRenderInfo* pRenderInfo, float RecoilAlpha)
{
	const CWeaponCustom* pWeap = FindWeapon(WeaponID);
	if(!pWeap)
		return;

	if(!pWeap->TexWeaponHandle.IsValid())
		return;

	vec2 Dir = direction(Angle);

	Graphics()->TextureSet(pWeap->TexWeaponHandle);
	Graphics()->QuadsBegin();
	Graphics()->QuadsSetRotation(AttachAngle + Angle);

	if(Dir.x < 0)
		Graphics()->QuadsSetSubset(0, 1, 1, 0);

	vec2 p;
	p = Pos + Dir * pWeap->WeaponPos.x - Dir * RecoilAlpha * pWeap->Recoil;
	p.y += pWeap->WeaponPos.y;
	IGraphics::CQuadItem QuadItem(p.x, p.y, pWeap->WeaponSize.x, pWeap->WeaponSize.y);
	Graphics()->QuadsDraw(&QuadItem, 1);

	Graphics()->QuadsEnd();

	RenderTools()->RenderTeeHand(pRenderInfo, p, Dir, pWeap->HandAngle, pWeap->HandPos);
}

void CDuckBridge::RenderWeaponCursor(int WeaponID, vec2 Pos)
{
	const CWeaponCustom* pWeap = FindWeapon(WeaponID);
	if(!pWeap)
		return;

	if(!pWeap->TexCursorHandle.IsValid())
		return;

	Graphics()->TextureSet(pWeap->TexCursorHandle);
	Graphics()->QuadsBegin();

	// render cursor
	float CursorSize = 45.25483399593904156165;
	IGraphics::CQuadItem QuadItem(Pos.x, Pos.y, CursorSize, CursorSize);
	Graphics()->QuadsDraw(&QuadItem, 1);

	Graphics()->QuadsEnd();
}

void CDuckBridge::RenderWeaponAmmo(int WeaponID, vec2 Pos)
{
	// TODO: do ammo?
}

void CDuckBridge::OnNewSnapshot()
{
	if(!IsLoaded())
		return;

	// reset snap
	m_SnapPrev = m_Snap;
	m_Snap.Clear();

	int Num = Client()->SnapNumItems(IClient::SNAP_CURRENT);
	for(int Index = 0; Index < Num; Index++)
	{
		IClient::CSnapItem Item;
		const void *pData = Client()->SnapGetItem(IClient::SNAP_CURRENT, Index, &Item);

		if(Item.m_Type >= NUM_NETOBJTYPES)
		{
			const int ID = Item.m_ID;
			const int Type = Item.m_Type - NUM_NETOBJTYPES;
			const int Size = Item.m_DataSize;

			if(Type == CNetObj_DuckCharCoreExtra::NET_ID && Size == sizeof(CNetObj_DuckCharCoreExtra))
			{
				if(ID >= 0 && ID < MAX_CLIENTS)
				{
					m_Snap.m_aCharCoreExtra[ID] = *(CNetObj_DuckCharCoreExtra*)pData;
				}
				else if(g_Config.m_Debug)
				{
					dbg_msg("duck", "snapshot error, DuckCharCoreExtra ID out of range (%d)", ID);
				}
			}
			else if(Type == CNetObj_DuckCustomCore::NET_ID && Size == sizeof(CNetObj_DuckCustomCore))
			{
				m_Snap.m_aCustomCores.add(*(CNetObj_DuckCustomCore*)pData);
			}
			else if(Type == CNetObj_DuckPhysJoint::NET_ID && Size == sizeof(CNetObj_DuckPhysJoint))
			{
				m_Snap.m_aJoints.add(*(CNetObj_DuckPhysJoint*)pData);
			}
			else if(Type == CNetObj_DuckPhysicsLawsGroup::NET_ID && Size == sizeof(CNetObj_DuckPhysicsLawsGroup))
			{
				m_Snap.m_aPhysicsLawsGroups.add(*(CNetObj_DuckPhysicsLawsGroup*)pData);
			}

			m_Backend.OnDuckSnapItem(Type, ID, (void*)pData, Size);
		}
		else
		{
			m_Backend.OnSnapItem(Item.m_Type, Item.m_ID, (void*)pData);
		}
	}

	//dbg_msg("duck", "custom cores count = %d", m_Snap.m_aCustomCores.size());
	//dbg_msg("duck", "plg count = %d", m_Snap.m_aPhysicsLawsGroups.size());
}

bool CDuckBridge::OnRenderPlayer(const CNetObj_Character *pPrevChar, const CNetObj_Character *pPlayerChar, const CNetObj_PlayerInfo *pPrevInfo, const CNetObj_PlayerInfo *pPlayerInfo, int ClientID)
{
	// Originally copied from CPlayers::RenderPlayer(...)

	CNetObj_Character Prev;
	CNetObj_Character Cur;
	Prev = *pPrevChar;
	Cur = *pPlayerChar;

	CNetObj_PlayerInfo pInfo = *pPlayerInfo;
	CTeeRenderInfo RenderInfo = m_pClient->m_aClients[ClientID].m_RenderInfo;

	// set size
	RenderInfo.m_Size = 64.0f;

	float IntraTick = Client()->IntraGameTick();

	if(Prev.m_Angle < pi*-128 && Cur.m_Angle > pi*128)
		Prev.m_Angle += 2*pi*256;
	else if(Prev.m_Angle > pi*128 && Cur.m_Angle < pi*-128)
		Cur.m_Angle += 2*pi*256;
	float Angle = mix((float)Prev.m_Angle, (float)Cur.m_Angle, IntraTick)/256.0f;

	if(m_pClient->m_LocalClientID == ClientID && Client()->State() != IClient::STATE_DEMOPLAYBACK)
	{
		// just use the direct input if it's local player we are rendering
		Angle = angle(m_pClient->m_pControls->m_MousePos);
	}

	// use preditect players if needed
	if(m_pClient->m_LocalClientID == ClientID && g_Config.m_ClPredict && Client()->State() != IClient::STATE_DEMOPLAYBACK)
	{
		if(!m_pClient->m_Snap.m_pLocalCharacter ||
			(m_pClient->m_Snap.m_pGameData && m_pClient->m_Snap.m_pGameData->m_GameStateFlags&(GAMESTATEFLAG_PAUSED|GAMESTATEFLAG_ROUNDOVER|GAMESTATEFLAG_GAMEOVER)))
		{
		}
		else
		{
			// apply predicted results
			m_pClient->m_PredictedChar.Write(&Cur);
			m_pClient->m_PredictedPrevChar.Write(&Prev);
			IntraTick = Client()->PredIntraGameTick();
		}
	}

	vec2 Direction = direction(Angle);
	vec2 Position = mix(vec2(Prev.m_X, Prev.m_Y), vec2(Cur.m_X, Cur.m_Y), IntraTick);
	vec2 Vel = mix(vec2(Prev.m_VelX/256.0f, Prev.m_VelY/256.0f), vec2(Cur.m_VelX/256.0f, Cur.m_VelY/256.0f), IntraTick);

	RenderInfo.m_GotAirJump = Cur.m_Jumped&2?0:1;

	bool Stationary = Cur.m_VelX <= 1 && Cur.m_VelX >= -1;
	bool InAir = !Collision()->CheckPoint(Cur.m_X, Cur.m_Y+16);
	bool WantOtherDir = (Cur.m_Direction == -1 && Vel.x > 0) || (Cur.m_Direction == 1 && Vel.x < 0);

	// evaluate animation
	const float WalkTimeMagic = 100.0f;
	float WalkTime =
		((Position.x >= 0)
			? fmod(Position.x, WalkTimeMagic)
			: WalkTimeMagic - fmod(-Position.x, WalkTimeMagic))
		/ WalkTimeMagic;
	CAnimState State;
	State.Set(&g_pData->m_aAnimations[ANIM_BASE], 0);

	if(InAir)
		State.Add(&g_pData->m_aAnimations[ANIM_INAIR], 0, 1.0f); // TODO: some sort of time here
	else if(Stationary)
		State.Add(&g_pData->m_aAnimations[ANIM_IDLE], 0, 1.0f); // TODO: some sort of time here
	else if(!WantOtherDir)
		State.Add(&g_pData->m_aAnimations[ANIM_WALK], WalkTime, 1.0f);


	static float s_LastGameTickTime = Client()->GameTickTime();
	if(m_pClient->m_Snap.m_pGameData && !(m_pClient->m_Snap.m_pGameData->m_GameStateFlags&GAMESTATEFLAG_PAUSED))
		s_LastGameTickTime = Client()->GameTickTime();
	if (Cur.m_Weapon == WEAPON_HAMMER)
	{
		float ct = (Client()->PrevGameTick()-Cur.m_AttackTick)/(float)SERVER_TICK_SPEED + s_LastGameTickTime;
		State.Add(&g_pData->m_aAnimations[ANIM_HAMMER_SWING], clamp(ct*5.0f,0.0f,1.0f), 1.0f);
	}
	if (Cur.m_Weapon == WEAPON_NINJA)
	{
		float ct = (Client()->PrevGameTick()-Cur.m_AttackTick)/(float)SERVER_TICK_SPEED + s_LastGameTickTime;
		State.Add(&g_pData->m_aAnimations[ANIM_NINJA_SWING], clamp(ct*2.0f,0.0f,1.0f), 1.0f);
	}

#if 0 // TESTING PURPOSES
	// draw gun
	float Recoil = 0.0f;
	{
		Graphics()->TextureSet(g_pData->m_aImages[IMAGE_GAME].m_Id);
		Graphics()->QuadsBegin();
		Graphics()->QuadsSetRotation(State.GetAttach()->m_Angle*pi*2+Angle);

		// normal weapons
		int iw = clamp(Cur.m_Weapon, 0, NUM_WEAPONS-1);
		RenderTools()->SelectSprite(g_pData->m_Weapons.m_aId[iw].m_pSpriteBody, Direction.x < 0 ? SPRITE_FLAG_FLIP_Y : 0);

		vec2 Dir = Direction;
		vec2 p;
		if (Cur.m_Weapon == WEAPON_HAMMER)
		{
			// Static position for hammer
			p = Position + vec2(State.GetAttach()->m_X, State.GetAttach()->m_Y);
			p.y += g_pData->m_Weapons.m_aId[iw].m_Offsety;
			// if attack is under way, bash stuffs
			if(Direction.x < 0)
			{
				Graphics()->QuadsSetRotation(-pi/2-State.GetAttach()->m_Angle*pi*2);
				p.x -= g_pData->m_Weapons.m_aId[iw].m_Offsetx;
			}
			else
			{
				Graphics()->QuadsSetRotation(-pi/2+State.GetAttach()->m_Angle*pi*2);
			}
			RenderTools()->DrawSprite(p.x, p.y, g_pData->m_Weapons.m_aId[iw].m_VisualSize);
		}
		else if (Cur.m_Weapon == WEAPON_NINJA)
		{
			p = Position;
			p.y += g_pData->m_Weapons.m_aId[iw].m_Offsety;

			if(Direction.x < 0)
			{
				Graphics()->QuadsSetRotation(-pi/2-State.GetAttach()->m_Angle*pi*2);
				p.x -= g_pData->m_Weapons.m_aId[iw].m_Offsetx;
				//m_pClient->m_pEffects->PowerupShine(p+vec2(32,0), vec2(32,12));
			}
			else
			{
				Graphics()->QuadsSetRotation(-pi/2+State.GetAttach()->m_Angle*pi*2);
				//m_pClient->m_pEffects->PowerupShine(p-vec2(32,0), vec2(32,12));
			}
			RenderTools()->DrawSprite(p.x, p.y, g_pData->m_Weapons.m_aId[iw].m_VisualSize);

			// HADOKEN
			if ((Client()->GameTick()-Cur.m_AttackTick) <= (SERVER_TICK_SPEED / 6) && g_pData->m_Weapons.m_aId[iw].m_NumSpriteMuzzles)
			{
				int IteX = random_int() % g_pData->m_Weapons.m_aId[iw].m_NumSpriteMuzzles;
				static int s_LastIteX = IteX;
				{
					if(m_pClient->m_Snap.m_pGameData && m_pClient->m_Snap.m_pGameData->m_GameStateFlags&GAMESTATEFLAG_PAUSED)
						IteX = s_LastIteX;
					else
						s_LastIteX = IteX;
				}
				if(g_pData->m_Weapons.m_aId[iw].m_aSpriteMuzzles[IteX])
				{
					vec2 Dir = vec2(pPlayerChar->m_X,pPlayerChar->m_Y) - vec2(pPrevChar->m_X, pPrevChar->m_Y);
					Dir = normalize(Dir);
					float HadokenAngle = angle(Dir);
					Graphics()->QuadsSetRotation(HadokenAngle );
					//float offsety = -data->weapons[iw].muzzleoffsety;
					RenderTools()->SelectSprite(g_pData->m_Weapons.m_aId[iw].m_aSpriteMuzzles[IteX], 0);
					vec2 DirY(-Dir.y,Dir.x);
					p = Position;
					float OffsetX = g_pData->m_Weapons.m_aId[iw].m_Muzzleoffsetx;
					p -= Dir * OffsetX;
					RenderTools()->DrawSprite(p.x, p.y, 160.0f);
				}
			}
		}
		else
		{
			// TODO: should be an animation
			Recoil = 0;
			static float s_LastIntraTick = IntraTick;
			if(m_pClient->m_Snap.m_pGameData && !(m_pClient->m_Snap.m_pGameData->m_GameStateFlags&GAMESTATEFLAG_PAUSED))
				s_LastIntraTick = IntraTick;

			float a = (Client()->GameTick()-Cur.m_AttackTick+s_LastIntraTick)/5.0f;
			if(a < 1)
				Recoil = sinf(a*pi);
			p = Position + Dir * g_pData->m_Weapons.m_aId[iw].m_Offsetx - Dir*Recoil*10.0f;
			p.y += g_pData->m_Weapons.m_aId[iw].m_Offsety;
			RenderTools()->DrawSprite(p.x, p.y, g_pData->m_Weapons.m_aId[iw].m_VisualSize);
		}

		if (Cur.m_Weapon == WEAPON_GUN || Cur.m_Weapon == WEAPON_SHOTGUN)
		{
			// check if we're firing stuff
			if(g_pData->m_Weapons.m_aId[iw].m_NumSpriteMuzzles)//prev.attackticks)
			{
				float Alpha = 0.0f;
				int Phase1Tick = (Client()->GameTick() - Cur.m_AttackTick);
				if (Phase1Tick < (g_pData->m_Weapons.m_aId[iw].m_Muzzleduration + 3))
				{
					float t = ((((float)Phase1Tick) + IntraTick)/(float)g_pData->m_Weapons.m_aId[iw].m_Muzzleduration);
					Alpha = mix(2.0f, 0.0f, min(1.0f,max(0.0f,t)));
				}

				int IteX = random_int() % g_pData->m_Weapons.m_aId[iw].m_NumSpriteMuzzles;
				static int s_LastIteX = IteX;
				{
					if(m_pClient->m_Snap.m_pGameData && m_pClient->m_Snap.m_pGameData->m_GameStateFlags&GAMESTATEFLAG_PAUSED)
						IteX = s_LastIteX;
					else
						s_LastIteX = IteX;
				}
				if (Alpha > 0.0f && g_pData->m_Weapons.m_aId[iw].m_aSpriteMuzzles[IteX])
				{
					float OffsetY = -g_pData->m_Weapons.m_aId[iw].m_Muzzleoffsety;
					RenderTools()->SelectSprite(g_pData->m_Weapons.m_aId[iw].m_aSpriteMuzzles[IteX], Direction.x < 0 ? SPRITE_FLAG_FLIP_Y : 0);
					if(Direction.x < 0)
						OffsetY = -OffsetY;

					vec2 DirY(-Dir.y,Dir.x);
					vec2 MuzzlePos = p + Dir * g_pData->m_Weapons.m_aId[iw].m_Muzzleoffsetx + DirY * OffsetY;

					RenderTools()->DrawSprite(MuzzlePos.x, MuzzlePos.y, g_pData->m_Weapons.m_aId[iw].m_VisualSize);
				}
			}
		}
		Graphics()->QuadsEnd();

		switch (Cur.m_Weapon)
		{
			case WEAPON_GUN: RenderTools()->RenderTeeHand(&RenderInfo, p, Direction, -3*pi/4, vec2(-15, 4)); break;
			case WEAPON_SHOTGUN: RenderTools()->RenderTeeHand(&RenderInfo, p, Direction, -pi/2, vec2(-5, 4)); break;
			case WEAPON_GRENADE: RenderTools()->RenderTeeHand(&RenderInfo, p, Direction, -pi/2, vec2(-4, 7)); break;
		}

	}

	/*
	if(pInfo.m_PlayerFlags&PLAYERFLAG_CHATTING)
	{
		Graphics()->TextureSet(g_pData->m_aImages[IMAGE_EMOTICONS].m_Id);
		Graphics()->QuadsBegin();
		RenderTools()->SelectSprite(SPRITE_DOTDOT);
		IGraphics::CQuadItem QuadItem(Position.x + 24, Position.y - 40, 64,64);
		Graphics()->QuadsDraw(&QuadItem, 1);
		Graphics()->QuadsEnd();
	}
	*/

	//RenderInfo.m_aTextures[0].Invalidate();
	RenderTools()->RenderTee(&State, &RenderInfo, Cur.m_Emote, Direction, Position);

	/*if (m_pClient->m_aClients[ClientID].m_EmoticonStart != -1 && m_pClient->m_aClients[ClientID].m_EmoticonStart + 2 * Client()->GameTickSpeed() > Client()->GameTick())
	{
		Graphics()->TextureSet(g_pData->m_aImages[IMAGE_EMOTICONS].m_Id);
		Graphics()->QuadsBegin();

		int SinceStart = Client()->GameTick() - m_pClient->m_aClients[ClientID].m_EmoticonStart;
		int FromEnd = m_pClient->m_aClients[ClientID].m_EmoticonStart + 2 * Client()->GameTickSpeed() - Client()->GameTick();

		float a = 1;

		if (FromEnd < Client()->GameTickSpeed() / 5)
			a = FromEnd / (Client()->GameTickSpeed() / 5.0);

		float h = 1;
		if (SinceStart < Client()->GameTickSpeed() / 10)
			h = SinceStart / (Client()->GameTickSpeed() / 10.0);

		float Wiggle = 0;
		if (SinceStart < Client()->GameTickSpeed() / 5)
			Wiggle = SinceStart / (Client()->GameTickSpeed() / 5.0);

		float WiggleAngle = sinf(5*Wiggle);

		Graphics()->QuadsSetRotation(pi/6*WiggleAngle);

		Graphics()->SetColor(1.0f * a, 1.0f * a, 1.0f * a, a);
		// client_datas::emoticon is an offset from the first emoticon
		RenderTools()->SelectSprite(SPRITE_OOP + m_pClient->m_aClients[ClientID].m_Emoticon);
		IGraphics::CQuadItem QuadItem(Position.x, Position.y - 23 - 32*h, 64, 64*h);
		Graphics()->QuadsDraw(&QuadItem, 1);
		Graphics()->QuadsEnd();
	}*/

#endif


	float Recoil = 0;
	if(Cur.m_Weapon >= WEAPON_GUN && Cur.m_Weapon <= WEAPON_GRENADE)
	{
		static float s_LastIntraTick = IntraTick;
		if(m_pClient->m_Snap.m_pGameData && !(m_pClient->m_Snap.m_pGameData->m_GameStateFlags&GAMESTATEFLAG_PAUSED))
			s_LastIntraTick = IntraTick;

		float a = (Client()->GameTick()-Cur.m_AttackTick+s_LastIntraTick)/5.0f;
		if(a < 1)
			Recoil = sinf(a*pi);
	}

	const int iw = clamp(Cur.m_Weapon, 0, NUM_WEAPONS-1);

	CWeaponSpriteInfo WeaponSprite;
	WeaponSprite.m_ID = Cur.m_Weapon;
	WeaponSprite.m_Recoil = Recoil;

	// TODO: add velocity
	// TODO: pack position, dir, and velocity into an "inter" (for interpolated) property

	RenderSetDrawSpace(DrawSpace::PLAYER + ClientID);
	return m_Backend.OnRenderPlayer(&State, &RenderInfo, Position, Direction, Cur.m_Emote, &WeaponSprite, Prev, Cur, ClientID);
}

void CDuckBridge::OnUpdatePlayer(const CNetObj_Character *pPrevChar, const CNetObj_Character *pPlayerChar, const CNetObj_PlayerInfo *pPrevInfo, const CNetObj_PlayerInfo *pPlayerInfo, int ClientID)
{
	// Originally copied from CPlayers::RenderPlayer(...)

	CNetObj_Character Prev;
	CNetObj_Character Player;
	Prev = *pPrevChar;
	Player = *pPlayerChar;

	CNetObj_PlayerInfo pInfo = *pPlayerInfo;
	CTeeRenderInfo RenderInfo = m_pClient->m_aClients[ClientID].m_RenderInfo;

	// set size
	RenderInfo.m_Size = 64.0f;

	float IntraTick = Client()->IntraGameTick();

	if(Prev.m_Angle < pi*-128 && Player.m_Angle > pi*128)
		Prev.m_Angle += 2*pi*256;
	else if(Prev.m_Angle > pi*128 && Player.m_Angle < pi*-128)
		Player.m_Angle += 2*pi*256;
	float Angle = mix((float)Prev.m_Angle, (float)Player.m_Angle, IntraTick)/256.0f;

	if(m_pClient->m_LocalClientID == ClientID && Client()->State() != IClient::STATE_DEMOPLAYBACK)
	{
		// just use the direct input if it's local player we are rendering
		Angle = angle(m_pClient->m_pControls->m_MousePos);
	}

	// use preditect players if needed
	if(m_pClient->m_LocalClientID == ClientID && g_Config.m_ClPredict && Client()->State() != IClient::STATE_DEMOPLAYBACK)
	{
		if(!m_pClient->m_Snap.m_pLocalCharacter ||
			(m_pClient->m_Snap.m_pGameData && m_pClient->m_Snap.m_pGameData->m_GameStateFlags&(GAMESTATEFLAG_PAUSED|GAMESTATEFLAG_ROUNDOVER|GAMESTATEFLAG_GAMEOVER)))
		{
		}
		else
		{
			// apply predicted results
			m_pClient->m_PredictedChar.Write(&Player);
			m_pClient->m_PredictedPrevChar.Write(&Prev);
			IntraTick = Client()->PredIntraGameTick();
		}
	}

	vec2 Direction = direction(Angle);
	vec2 Position = mix(vec2(Prev.m_X, Prev.m_Y), vec2(Player.m_X, Player.m_Y), IntraTick);
	vec2 Vel = mix(vec2(Prev.m_VelX/256.0f, Prev.m_VelY/256.0f), vec2(Player.m_VelX/256.0f, Player.m_VelY/256.0f), IntraTick);

	// particle flow interaction
	m_pClient->m_pFlow->Add(Position, Vel*100.0f, 10.0f);

	bool Stationary = Player.m_VelX <= 1 && Player.m_VelX >= -1;
	bool InAir = !Collision()->CheckPoint(Player.m_X, Player.m_Y+16);
	bool WantOtherDir = (Player.m_Direction == -1 && Vel.x > 0) || (Player.m_Direction == 1 && Vel.x < 0);

	// evaluate animation
	const float WalkTimeMagic = 100.0f;
	float WalkTime =
		((Position.x >= 0)
			? fmod(Position.x, WalkTimeMagic)
			: WalkTimeMagic - fmod(-Position.x, WalkTimeMagic))
		/ WalkTimeMagic;
	CAnimState State;
	State.Set(&g_pData->m_aAnimations[ANIM_BASE], 0);

	if(InAir)
		State.Add(&g_pData->m_aAnimations[ANIM_INAIR], 0, 1.0f); // TODO: some sort of time here
	else if(Stationary)
		State.Add(&g_pData->m_aAnimations[ANIM_IDLE], 0, 1.0f); // TODO: some sort of time here
	else if(!WantOtherDir)
		State.Add(&g_pData->m_aAnimations[ANIM_WALK], WalkTime, 1.0f);

	static float s_LastGameTickTime = Client()->GameTickTime();
	if(m_pClient->m_Snap.m_pGameData && !(m_pClient->m_Snap.m_pGameData->m_GameStateFlags&GAMESTATEFLAG_PAUSED))
		s_LastGameTickTime = Client()->GameTickTime();
	if (Player.m_Weapon == WEAPON_HAMMER)
	{
		float ct = (Client()->PrevGameTick()-Player.m_AttackTick)/(float)SERVER_TICK_SPEED + s_LastGameTickTime;
		State.Add(&g_pData->m_aAnimations[ANIM_HAMMER_SWING], clamp(ct*5.0f,0.0f,1.0f), 1.0f);
	}
	if (Player.m_Weapon == WEAPON_NINJA)
	{
		float ct = (Client()->PrevGameTick()-Player.m_AttackTick)/(float)SERVER_TICK_SPEED + s_LastGameTickTime;
		State.Add(&g_pData->m_aAnimations[ANIM_NINJA_SWING], clamp(ct*2.0f,0.0f,1.0f), 1.0f);
	}

	// do skidding
	if(!InAir && WantOtherDir && length(Vel*50) > 500.0f)
	{
		static int64 SkidSoundTime = 0;
		if(time_get()-SkidSoundTime > time_freq()/10)
		{
			m_pClient->m_pSounds->PlayAt(CSounds::CHN_WORLD, SOUND_PLAYER_SKID, 0.25f, Position);
			SkidSoundTime = time_get();
		}

		m_pClient->m_pEffects->SkidTrail(
			Position+vec2(-Player.m_Direction*6,12),
			vec2(-Player.m_Direction*100*length(Vel),-50)
		);
	}

	// render the "shadow" tee
	if(m_pClient->m_LocalClientID == ClientID && g_Config.m_Debug)
	{
		vec2 GhostPosition = mix(vec2(pPrevChar->m_X, pPrevChar->m_Y), vec2(pPlayerChar->m_X, pPlayerChar->m_Y), Client()->IntraGameTick());
		CTeeRenderInfo Ghost = RenderInfo;
		for(int p = 0; p < NUM_SKINPARTS; p++)
			Ghost.m_aColors[p].a *= 0.5f;
		RenderTools()->RenderTee(&State, &Ghost, Player.m_Emote, Direction, GhostPosition); // render ghost
	}

	if(pInfo.m_PlayerFlags&PLAYERFLAG_CHATTING)
	{
		Graphics()->TextureSet(g_pData->m_aImages[IMAGE_EMOTICONS].m_Id);
		Graphics()->QuadsBegin();
		RenderTools()->SelectSprite(SPRITE_DOTDOT);
		IGraphics::CQuadItem QuadItem(Position.x + 24, Position.y - 40, 64,64);
		Graphics()->QuadsDraw(&QuadItem, 1);
		Graphics()->QuadsEnd();
	}

	// emotes
	if (m_pClient->m_aClients[ClientID].m_EmoticonStart != -1 && m_pClient->m_aClients[ClientID].m_EmoticonStart + 2 * Client()->GameTickSpeed() > Client()->GameTick())
	{
		Graphics()->TextureSet(g_pData->m_aImages[IMAGE_EMOTICONS].m_Id);
		Graphics()->QuadsBegin();

		int SinceStart = Client()->GameTick() - m_pClient->m_aClients[ClientID].m_EmoticonStart;
		int FromEnd = m_pClient->m_aClients[ClientID].m_EmoticonStart + 2 * Client()->GameTickSpeed() - Client()->GameTick();

		float a = 1;

		if (FromEnd < Client()->GameTickSpeed() / 5)
			a = FromEnd / (Client()->GameTickSpeed() / 5.0);

		float h = 1;
		if (SinceStart < Client()->GameTickSpeed() / 10)
			h = SinceStart / (Client()->GameTickSpeed() / 10.0);

		float Wiggle = 0;
		if (SinceStart < Client()->GameTickSpeed() / 5)
			Wiggle = SinceStart / (Client()->GameTickSpeed() / 5.0);

		float WiggleAngle = sinf(5*Wiggle);

		Graphics()->QuadsSetRotation(pi/6*WiggleAngle);

		Graphics()->SetColor(1.0f * a, 1.0f * a, 1.0f * a, a);
		// client_datas::emoticon is an offset from the first emoticon
		RenderTools()->SelectSprite(SPRITE_OOP + m_pClient->m_aClients[ClientID].m_Emoticon);
		IGraphics::CQuadItem QuadItem(Position.x, Position.y - 23 - 32*h, 64, 64*h);
		Graphics()->QuadsDraw(&QuadItem, 1);
		Graphics()->QuadsEnd();
    }
}

bool CDuckBridge::OnBind(int Stroke, const char *pCmd)
{
    return m_Backend.OnBind(Stroke, pCmd);
}

bool CDuckBridge::IsModAlreadyInstalled(const SHA256_DIGEST *pModSha256)
{
	char aSha256Str[SHA256_MAXSTRSIZE];
	sha256_str(*pModSha256, aSha256Str, sizeof(aSha256Str));

	char aModFilePath[512];
	str_copy(aModFilePath, "mods/", sizeof(aModFilePath));
	str_append(aModFilePath, aSha256Str, sizeof(aModFilePath));
	str_append(aModFilePath, ".mod", sizeof(aModFilePath));

	IOHANDLE ModFile = Storage()->OpenFile(aModFilePath, IOFLAG_READ, IStorage::TYPE_SAVE);
	if(ModFile)
	{
		io_close(ModFile);
		dbg_msg("duck", "mod is already installed on disk");
		return true;
	}
	return false;
}

bool CDuckBridge::ExtractAndInstallModZipBuffer(const CGrowBuffer *pHttpZipData, const SHA256_DIGEST *pModSha256)
{
#ifdef MOD_ZIPFILE
	dbg_msg("unzip", "EXTRACTING AND INSTALLING MOD");

	char aUserModsPath[512];
	Storage()->GetCompletePath(IStorage::TYPE_SAVE, "mods", aUserModsPath, sizeof(aUserModsPath));
	fs_makedir(aUserModsPath); // Teeworlds/mods (user storage)

	// TODO: reduce folder hash string length?
	SHA256_DIGEST Sha256 = sha256(pHttpZipData->m_pData, pHttpZipData->m_Size);
	char aSha256Str[SHA256_MAXSTRSIZE];
	sha256_str(Sha256, aSha256Str, sizeof(aSha256Str));

	if(Sha256 != *pModSha256)
	{
		dbg_msg("duck", "mod url sha256 and server sent mod sha256 mismatch, received sha256=%s", aSha256Str);
		// TODO: display error message
		return false;
	}

	char aModRootPath[512];
	str_copy(aModRootPath, aUserModsPath, sizeof(aModRootPath));
	str_append(aModRootPath, "/", sizeof(aModRootPath));
	str_append(aModRootPath, aSha256Str, sizeof(aModRootPath));


	// FIXME: remove this
	/*
	str_append(aUserModsPath, "/temp.zip", sizeof(aUserModsPath));
	IOHANDLE File = io_open(aUserMoodsPath, IOFLAG_WRITE);
	io_write(File, pHttpZipData->m_pData, pHttpZipData->m_Size);
	io_close(File);

	zip *pZipArchive = zip_open(aUserMoodsPath, 0, &Error);
	if(pZipArchive == NULL)
	{
		char aErrorBuff[512];
		zip_error_to_str(aErrorBuff, sizeof(aErrorBuff), Error, errno);
		dbg_msg("unzip", "Error opening '%s' [%s]", aUserMoodsPath, aErrorBuff);
		return false;
	}*/

	zip_error_t ZipError;
	zip_error_init(&ZipError);
	zip_source_t* pZipSrc = zip_source_buffer_create(pHttpZipData->m_pData, pHttpZipData->m_Size, 1, &ZipError);
	if(!pZipSrc)
	{
		dbg_msg("unzip", "Error creating zip source [%s]", zip_error_strerror(&ZipError));
		zip_error_fini(&ZipError);
		return false;
	}

	dbg_msg("unzip", "OPENING zip source %s", aSha256Str);

	// int Error = 0;
	zip *pZipArchive = zip_open_from_source(pZipSrc, 0, &ZipError);
	if(pZipArchive == NULL)
	{
		dbg_msg("unzip", "Error opening source [%s]", zip_error_strerror(&ZipError));
		zip_source_free(pZipSrc);
		zip_error_fini(&ZipError);
		return false;
	}
	zip_error_fini(&ZipError);

	dbg_msg("unzip", "CREATE directory '%s'", aModRootPath);
	if(fs_makedir(aModRootPath) != 0)
	{
		dbg_msg("unzip", "Failed to create directory '%s'", aModRootPath);
		return false;
	}

	const int EntryCount = zip_get_num_entries(pZipArchive, 0);

	// find required files
	const char* aRequiredFiles[] = {
		MAIN_SCRIPT_FILE,
		"mod_info.json"
	};
	const int RequiredFilesCount = sizeof(aRequiredFiles)/sizeof(aRequiredFiles[0]);

	int FoundRequiredFilesCount = 0;
	for(int i = 0; i < EntryCount && FoundRequiredFilesCount < RequiredFilesCount; i++)
	{
		zip_stat_t EntryStat;
		if(zip_stat_index(pZipArchive, i, 0, &EntryStat) != 0)
			continue;

		const int NameLen = str_length(EntryStat.name);
		if(EntryStat.name[NameLen-1] != '/')
		{
			for(int r = 0; r < RequiredFilesCount; r++)
			{
				 // TODO: can 2 files have the same name?
				if(str_comp(EntryStat.name, aRequiredFiles[r]) == 0)
					FoundRequiredFilesCount++;
			}
		}
	}

	if(FoundRequiredFilesCount != RequiredFilesCount)
	{
		dbg_msg("duck", "mod is missing a required file, required files are: ");
		for(int r = 0; r < RequiredFilesCount; r++)
		{
			dbg_msg("duck", "    - %s", aRequiredFiles[r]);
		}
		return false;
	}

	// walk zip file tree and extract
	for(int i = 0; i < EntryCount; i++)
	{
		zip_stat_t EntryStat;
		if(zip_stat_index(pZipArchive, i, 0, &EntryStat) != 0)
			continue;

		// TODO: remove
		dbg_msg("unzip", "- name: %s, size: %llu, mtime: [%u]", EntryStat.name, EntryStat.size, (unsigned int)EntryStat.mtime);

		// TODO: sanitize folder name
		const int NameLen = str_length(EntryStat.name);
		if(EntryStat.name[NameLen-1] == '/')
		{
			// create sub directory
			char aSubFolder[512];
			str_copy(aSubFolder, aModRootPath, sizeof(aSubFolder));
			str_append(aSubFolder, "/", sizeof(aSubFolder));
			str_append(aSubFolder, EntryStat.name, sizeof(aSubFolder));

			dbg_msg("unzip", "CREATE SUB directory '%s'", aSubFolder);
			if(fs_makedir(aSubFolder) != 0)
			{
				dbg_msg("unzip", "Failed to create directory '%s'", aSubFolder);
				return false;
			}
		}
		else
		{
			// filter by extension
			if(!(str_endswith(EntryStat.name, SCRIPTFILE_EXT) || str_endswith(EntryStat.name, ".json") || str_endswith(EntryStat.name, ".png") || str_endswith(EntryStat.name, ".wv")))
				continue;

			// TODO: verify file type? Might be very expensive to do so.
			zip_file_t* pFileZip = zip_fopen_index(pZipArchive, i, 0);
			if(!pFileZip)
			{
				dbg_msg("unzip", "Error reading file '%s'", EntryStat.name);
				return false;
			}

			// create file on disk
			char aFilePath[256];
			str_copy(aFilePath, aModRootPath, sizeof(aFilePath));
			str_append(aFilePath, "/", sizeof(aFilePath));
			str_append(aFilePath, EntryStat.name, sizeof(aFilePath));

			IOHANDLE FileExtracted = io_open(aFilePath, IOFLAG_WRITE);
			if(!FileExtracted)
			{
				dbg_msg("unzip", "Error creating file '%s'", aFilePath);
				return false;
			}

			// read zip file data and write to file on disk
			char aReadBuff[1024];
			unsigned ReadCurrentSize = 0;
			while(ReadCurrentSize != EntryStat.size)
			{
				const int ReadLen = zip_fread(pFileZip, aReadBuff, sizeof(aReadBuff));
				if(ReadLen < 0)
				{
					dbg_msg("unzip", "Error reading file '%s'", EntryStat.name);
					return false;
				}
				io_write(FileExtracted, aReadBuff, ReadLen);
				ReadCurrentSize += ReadLen;
			}

			io_close(FileExtracted);
			zip_fclose(pFileZip);
		}
	}

	zip_source_close(pZipSrc);
	// NOTE: no need to call zip_source_free(pZipSrc), HttpBuffer::Release() already frees up the buffer

	//zip_close(pZipArchive);

#if 0
	unzFile ZipFile = unzOpen64(aPath);
	unz_global_info GlobalInfo;
	int r = unzGetGlobalInfo(ZipFile, &GlobalInfo);
	if(r != UNZ_OK)
	{
		dbg_msg("unzip", "could not read file global info (%d)", r);
		unzClose(ZipFile);
		dbg_break();
		return false;
	}

	for(int i = 0; i < GlobalInfo.number_entry; i++)
	{
		// Get info about current file.
		unz_file_info file_info;
		char filename[256];
		if(unzGetCurrentFileInfo(ZipFile, &file_info, filename, sizeof(filename), NULL, 0, NULL, 0) != UNZ_OK)
		{
			dbg_msg("unzip", "could not read file info");
			unzClose(ZipFile);
			return false;
		}

		dbg_msg("unzip", "FILE_ENTRY %s", filename);

		/*// Check if this entry is a directory or file.
		const size_t filename_length = str_length(filename);
		if(filename[ filename_length-1 ] == '/')
		{
			// Entry is a directory, so create it.
			printf("dir:%s\n", filename);
			//mkdir(filename);
		}
		else
		{
			// Entry is a file, so extract it.
			printf("file:%s\n", filename);
			if(unzOpenCurrentFile(ZipFile) != UNZ_OK)
			{
				dbg_msg("unzip", "could not open file");
				unzClose(ZipFile);
				return false;
			}

			// Open a file to write out the data.
			FILE *out = fopen(filename, "wb");
			if(out == NULL)
			{
				dbg_msg("unzip", "could not open destination file");
				unzCloseCurrentFile(ZipFile);
				unzClose(ZipFile);
				return false;
			}

			int error = UNZ_OK;
			do
			{
			error = unzReadCurrentFile(zipfile, read_buffer, READ_SIZE);
			if(error < 0)
			{
			printf("error %d\n", error);
			unzCloseCurrentFile(zipfile);
			unzClose(zipfile);
			return -1;
			}

			// Write data to file.
			if(error > 0)
			{
			fwrite(read_buffer, error, 1, out); // You should check return of fwrite...
			}
			} while (error > 0);

			fclose(out);
		}*/

		unzCloseCurrentFile(ZipFile);

		// Go the the next entry listed in the zip file.
		if((i+1) < GlobalInfo.number_entry)
		{
			if(unzGoToNextFile(ZipFile) != UNZ_OK)
			{
				dbg_msg("unzip", "cound not read next file");
				unzClose(ZipFile);
				return false;
			}
		}
	}

	unzClose(ZipFile);
#endif

#endif
	return true;
}

bool CDuckBridge::ExtractAndInstallModCompressedBuffer(const void *pCompBuff, int CompBuffSize, const SHA256_DIGEST *pModSha256)
{
	const bool IsConfigDebug = g_Config.m_Debug;

	if(IsConfigDebug)
		dbg_msg("unzip", "EXTRACTING AND INSTALLING *COMRPESSED* MOD");

	char aUserModsPath[512];
	Storage()->GetCompletePath(IStorage::TYPE_SAVE, "mods", aUserModsPath, sizeof(aUserModsPath));
	fs_makedir(aUserModsPath); // Teeworlds/mods (user storage)

	// TODO: reduce folder hash string length?
	SHA256_DIGEST Sha256 = sha256(pCompBuff, CompBuffSize);
	char aSha256Str[SHA256_MAXSTRSIZE];
	sha256_str(Sha256, aSha256Str, sizeof(aSha256Str));

	if(Sha256 != *pModSha256)
	{
		dbg_msg("duck", "mod sha256 and server sent mod sha256 mismatch, received sha256=%s", aSha256Str);
		// TODO: display error message
		return false;
	}

	// extract
	CDuckModFile DuckModFile;
	DuckModFile.m_Sha256 = Sha256;
	DuckModFile.m_FileBuffer.Append(pCompBuff, CompBuffSize);

	CDuckModFileExtracted Extracted;
	bool r = DuckExtractFilesFromModFile(&DuckModFile, &Extracted, IsConfigDebug);
	if(!r)
	{
		dbg_msg("duck", "failed to extract mod sha256=%s", aSha256Str);
		// TODO: display error message
		return false;
	}

	// mod folder where we're going to extract the files
	char aDiskFilePath[512];
	str_format(aDiskFilePath, sizeof(aDiskFilePath), "%s/%s.mod", aUserModsPath, aSha256Str);

	IOHANDLE SavedModFile = io_open(aDiskFilePath, IOFLAG_WRITE);
	if(!SavedModFile)
	{
		dbg_msg("duck", "Error creating file '%s'", aDiskFilePath);
		return false;
	}

	io_write(SavedModFile, pCompBuff, CompBuffSize);
	io_close(SavedModFile);
	return true;
}


bool CDuckBridge::LoadModFilesFromDisk(const SHA256_DIGEST *pModSha256)
{
	if(m_ModFiles.m_Sha256 != *pModSha256)
	{
		char aModFilePath[512];
		char aSha256Str[SHA256_MAXSTRSIZE];
		Storage()->GetCompletePath(IStorage::TYPE_SAVE, "mods", aModFilePath, sizeof(aModFilePath));

		sha256_str(*pModSha256, aSha256Str, sizeof(aSha256Str));
		str_append(aModFilePath, "/", sizeof(aModFilePath));
		str_append(aModFilePath, aSha256Str, sizeof(aModFilePath));
		str_append(aModFilePath, ".mod", sizeof(aModFilePath));

		IOHANDLE ModFile = io_open(aModFilePath, IOFLAG_READ);
		if(!ModFile)
		{
			dbg_msg("duck", "could not open '%s'", aModFilePath);
			return false;
		}

		const int FileSize = (int)io_length(ModFile);
		dbg_assert(FileSize < (10*1024*1024), "File too large"); // TODO: larger

		CDuckModFile DuckModFile;
		DuckModFile.m_Sha256 = *pModSha256;
		DuckModFile.m_FileBuffer.Grow(FileSize);

		io_read(ModFile, DuckModFile.m_FileBuffer.m_pData, FileSize);
		io_close(ModFile);

		DuckModFile.m_FileBuffer.m_Size = FileSize;

		bool r = DuckExtractFilesFromModFile(&DuckModFile, &m_ModFiles, g_Config.m_Debug == 1);
		if(!r)
		{
			dbg_msg("duck", "failed to extract '%s'", aModFilePath);
			return false;
		}
	}

	// reset mod
	ModInit();

	const int FileCount = m_ModFiles.m_Files.size();
	const CDuckModFileExtracted::CFileEntry* pFileEntries = m_ModFiles.m_Files.base_ptr();
	for(int i = 0; i < FileCount; i++)
	{
		const char* pFilePath = pFileEntries[i].m_aPath;
		dbg_msg("duck", "load_file='%s'", pFilePath);

		if(str_endswith(pFilePath, SCRIPTFILE_EXT))
		{
			m_Backend.AddScriptFileItem(pFilePath, pFileEntries[i].m_pData, pFileEntries[i].m_Size);
		}
		else if(str_endswith(pFilePath, ".png"))
		{
			const bool Loaded = LoadTextureRaw(pFilePath, pFileEntries[i].m_pData, pFileEntries[i].m_Size);

			if(!Loaded)
			{
				ScriptError(JsErrorLvl::CRITICAL, "error loading png image '%s'", pFilePath);
				continue;
			}

			dbg_msg("duck", "image loaded '%s' (%x)", pFilePath, m_aTextures[m_aTextures.size()-1].m_Hash);
			// TODO: show error instead of breaking

			if(str_startswith(pFilePath, "skins/")) {
				pFilePath += 6;
				const char* pPartEnd = str_find(pFilePath, "/");
				if(!str_find(pPartEnd+1, "/")) {
					dbg_msg("duck", "skin part name = '%.*s'", pPartEnd-pFilePath, pFilePath);
					char aPart[256];
					str_format(aPart, sizeof(aPart), "%.*s", pPartEnd-pFilePath, pFilePath);
					AddSkinPart(aPart, pPartEnd+1, m_aTextures[m_aTextures.size()-1].m_Handle);
				}
			}
		}
		else if(str_endswith(pFilePath, ".wv"))
		{
			ISound::CSampleHandle SoundId = m_pClient->Sound()->LoadWVRaw(pFilePath, pFileEntries[i].m_pData, pFileEntries[i].m_Size);

			const int Len = str_length(pFilePath);
			if(Len < 4 || !SoundId.IsValid()) // .wv
			{
				dbg_msg("duck", "ERROR loading sound '%s'", pFilePath);
				continue;
			}

			uint32_t Hash = hash_fnv1a(pFilePath, Len-3);
			dbg_msg("duck", "sound loaded '%s' (%x)", pFilePath, Hash);

			CSoundHashPair Pair;
			Pair.m_Hash = Hash;
			Pair.m_Handle = SoundId;
			m_aSounds.add(Pair);
		}
	}

	m_Backend.OnModLoaded();
	m_IsModLoaded = true;
	return true;
}

bool CDuckBridge::TryLoadInstalledDuckMod(const SHA256_DIGEST *pModSha256)
{
	if(!IsModAlreadyInstalled(pModSha256))
		return false;

	bool IsLoaded = LoadModFilesFromDisk(pModSha256);
	dbg_assert(IsLoaded, "Loaded from disk: rip in peace");

	char aSha256Str[SHA256_MAXSTRSIZE];
	sha256_str(*pModSha256, aSha256Str, sizeof(aSha256Str));
	dbg_msg("duck", "mod loaded (already installed) sha256='%s'", aSha256Str);
	return IsLoaded;
}

bool CDuckBridge::InstallAndLoadDuckModFromModFile(const void *pBuffer, int BufferSize, const SHA256_DIGEST *pModSha256)
{
	dbg_assert(!IsModAlreadyInstalled(pModSha256), "mod is already installed, check it before calling this");

	bool IsUnzipped = ExtractAndInstallModCompressedBuffer(pBuffer, BufferSize, pModSha256);
	dbg_assert(IsUnzipped, "Unzipped to disk: rip in peace");

	if(!IsUnzipped)
		return false;

	bool IsLoaded = LoadModFilesFromDisk(pModSha256);
	dbg_assert(IsLoaded, "Loaded from disk: rip in peace");

	char aSha256Str[SHA256_MAXSTRSIZE];
	sha256_str(*pModSha256, aSha256Str, sizeof(aSha256Str));
	dbg_msg("duck", "mod loaded from zip buffer sha256='%s'", aSha256Str);
	return IsLoaded;
}

void CDuckBridge::OnInit()
{
	m_Backend.m_pBridge = this;
	m_CurrentDrawSpace = 0;
	m_CurrentPacketFlags = -1;
	m_RgGame.Init(this, DrawSpace::GAME);
	m_RgGameForeGround.Init(this, DrawSpace::GAME_FOREGROUND);
	m_RgHud.Init(this, DrawSpace::HUD);
	m_RgJsErrors.Init(this, DrawSpace::HUD);
	m_MousePos = vec2(Graphics()->ScreenWidth() * 0.5, Graphics()->ScreenHeight() * 0.5);
	m_IsMenuModeActive = false;
	m_DoUnloadModBecauseError = false;
}

void CDuckBridge::OnReset()
{
	//Reset();
}

void CDuckBridge::OnShutdown()
{
	m_Backend.Shutdown();
}

void CDuckBridge::OnRender()
{
	if(m_DoUnloadModBecauseError)
	{
		Unload();
		return;
	}

	if(Client()->State() != IClient::STATE_ONLINE || !IsLoaded())
		return;

	m_FrameAllocator.Clear(); // clear frame allocator

	const float LocalTime = Client()->LocalTime();
	const float IntraGameTick = Client()->IntraGameTick();

	// Call OnRender(LocalTime, IntraGameTick)
	m_Backend.OnRender(LocalTime, IntraGameTick);

	static float LastTime = LocalTime;
	static float Accumulator = 0.0f;
	const float UPDATE_RATE = 1.0/60.0;

	Accumulator += LocalTime - LastTime;
	LastTime = LocalTime;

	int UpdateCount = 0;
	while(Accumulator > UPDATE_RATE)
	{
		m_Backend.OnUpdate(LocalTime, IntraGameTick);

		Accumulator -= UPDATE_RATE;
		UpdateCount++;

		if(UpdateCount > 2) {
			Accumulator = 0.0;
			break;
		}
	}

	// detect stack leak
	if(m_Backend.IsStackLeaking())
	{
		ScriptError(JsErrorLvl::CRITICAL, "Stack leak");
	}
}

void CDuckBridge::OnMessage(int Msg, void *pRawMsg)
{
	if(!IsLoaded()) {
		return;
	}

	m_Backend.OnMessage(Msg, pRawMsg);
}

bool CDuckBridge::OnMouseMove(float x, float y)
{
	if(m_IsMenuModeActive)
	{
		m_MousePos += vec2(x, y);
		m_MousePos.x = clamp(m_MousePos.x, 0.0f, (float)Graphics()->ScreenWidth());
		m_MousePos.y = clamp(m_MousePos.y, 0.0f, (float)Graphics()->ScreenHeight());
		return true;
	}

	return false;
}

void CDuckBridge::OnStateChange(int NewState, int OldState)
{
	if(OldState != IClient::STATE_OFFLINE && NewState == IClient::STATE_OFFLINE)
	{
		Unload();
	}
}

bool CDuckBridge::OnInput(IInput::CEvent e)
{
	if(!IsLoaded()) {
		return false;
	}

	m_Backend.OnInput(e);

	if(m_IsMenuModeActive)
		return true;
	return false;
}

void CDuckBridge::ModInit()
{
	m_Backend.Reset();
	Reset();
	m_IsModLoaded = false;
	m_aJsErrors.clear();
}

void CDuckBridge::Unload()
{
	m_Backend.Shutdown();
	Reset();
	m_IsModLoaded = false;
	m_DoUnloadModBecauseError = false;
	dbg_msg("duck", "MOD UNLOAD");
}

void CDuckBridge::CRenderGroupJsErrors::OnRender()
{
	CUIRect UiRect = *UI()->Screen();
	Graphics()->MapScreen(0.0f, 0.0f, UiRect.w, UiRect.h);

	const int Count = m_pBridge->m_aJsErrors.size();
	const CDuckBridge::CJsErrorStr* aErrors = m_pBridge->m_aJsErrors.base_ptr();
	const float LocalTime = Client()->LocalTime();
	const float StayOnScreenTime = 10.f;
	const float FadeTime = 1.f;
	const float FontSize = 12;
	const float MarginX = 5;
	const float MarginY = 3;
	const float LineWidth = UiRect.w/3;
	float OffsetY = UiRect.h;

	for(int i = Count-1; i >= 0; i--)
	{
		const CDuckBridge::CJsErrorStr& Err = aErrors[i];
		float a = min((StayOnScreenTime + FadeTime - (LocalTime - Err.m_Time)) / FadeTime, 1.0f);
		if(a < 0.0f)
			continue;

		float aRect[4] = {FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX};
		CTextCursor Cursor;
		TextRender()->SetCursor(&Cursor, 0, 0, FontSize, TEXTFLAG_RENDER|TEXTFLAG_ALLOW_NEWLINE);
		Cursor.m_LineWidth = LineWidth;
		TextRender()->TextCalculateRect(&Cursor, Err.m_aText, -1, aRect);
		const vec2 Size(aRect[2] - aRect[0], aRect[3] - aRect[1]);

		Graphics()->TextureClear();
		Graphics()->QuadsBegin();

		switch(Err.m_Level) {
			case JsErrorLvl::WARNING:  Graphics()->SetColor(0.8 * a, 0.25 * a, 0, a); break;
			case JsErrorLvl::ERROR:    Graphics()->SetColor(0.8 * a, 0.0, 0, a); break;
			case JsErrorLvl::CRITICAL: Graphics()->SetColor(0.4 * a, 0, 0, a); break;
			default: dbg_assert(0, "case not handled"); break;
		}

		float BgX = UiRect.w - Size.x - MarginX*2;
		float BgY =  OffsetY - Size.y - MarginY*2;
		IGraphics::CQuadItem BgQuad(BgX, BgY, Size.x + MarginX*2, Size.y + MarginY*2);
		Graphics()->QuadsDrawTL(&BgQuad, 1);
		Graphics()->QuadsEnd();

		TextRender()->SetCursor(&Cursor, BgX + MarginX, BgY + MarginY, FontSize, TEXTFLAG_RENDER|TEXTFLAG_ALLOW_NEWLINE);
		Cursor.m_LineWidth = LineWidth;
		TextRender()->TextShadowed(&Cursor, Err.m_aText, -1, vec2(0,0), vec4(0,0,0,0), vec4(1, 1, 1, a));

		OffsetY -= Size.y + MarginY * 2;
	}
}
