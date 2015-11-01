#ifndef GAME_EDITOR_AUTO_MAP_H
#define GAME_EDITOR_AUTO_MAP_H

#include <stdlib.h> // rand

#include <base/tl/array.h>
#include <base/vmath.h>

#include <engine/external/json-parser/json-builder.h>


class IAutoMapper
{
protected:
	class CEditor *m_pEditor;
	int m_Type;

public:
	enum
	{
		TYPE_TILESET,
		TYPE_DOODADS,

		MAX_RULES=256
	};

	//
	IAutoMapper(class CEditor *pEditor, int Type) : m_pEditor(pEditor), m_Type(Type) {}
	virtual ~IAutoMapper() {}
	virtual void Load(const json_value &rElement) = 0;
	virtual void Proceed(class CLayerTiles *pLayer, int ConfigID) {}
	virtual void Proceed(class CLayerTiles *pLayer, int ConfigID, int Ammount) {} // for convenience purposes

	virtual int RuleSetNum() = 0;
	virtual const char* GetRuleSetName(int Index) const = 0;

	//
	int GetType() const { return m_Type; }

	static bool Random(int Value)
	{
		return ((int)((float)rand() / ((float)RAND_MAX + 1) * Value) == 1);
	}

	static const char *GetTypeName(int Type)
	{
		if(Type == TYPE_TILESET)
			return "tileset";
		else if(Type == TYPE_DOODADS)
			return "doodads";
		else
			return "";
	}
};

class CTilesetMapper: public IAutoMapper
{
	struct CRuleCondition
	{
		int m_X;
		int m_Y;
		int m_Value;

		enum
		{
			EMPTY=-2,
			FULL=-1
		};
	};

	struct CRule
	{
		int m_Index;
		int m_HFlip;
		int m_VFlip;
		int m_Random;
		int m_Rotation;

		array<CRuleCondition> m_aConditions;
	};

	struct CRuleSet
	{
		char m_aName[128];
		int m_BaseTile;

		array<CRule> m_aRules;
	};

	array<CRuleSet> m_aRuleSets;

public:
	CTilesetMapper(class CEditor *pEditor) : IAutoMapper(pEditor, TYPE_TILESET) { m_aRuleSets.clear(); }

	virtual void Load(const json_value &rElement);
	virtual void Proceed(class CLayerTiles *pLayer, int ConfigID);

	virtual int RuleSetNum() { return m_aRuleSets.size(); }
	virtual const char* GetRuleSetName(int Index) const;
};

class CDoodadsMapper: public IAutoMapper
{
public:
	struct CRule
	{
		ivec2 m_Rect;
		ivec2 m_Size;
		ivec2 m_RelativePos;

		int m_Location;
		int m_Random;

		int m_HFlip;
		int m_VFlip;

		enum
		{
			FLOOR=0,
			CEILING,
			WALLS
		};
	};

	struct CRuleSet
	{
		char m_aName[128];

		array<CRule> m_aRules;
	};

	CDoodadsMapper(class CEditor *pEditor) :  IAutoMapper(pEditor, TYPE_DOODADS) { m_aRuleSets.clear(); }

	virtual void Load(const json_value &rElement);
	virtual void Proceed(class CLayerTiles *pLayer, int ConfigID, int Amount);
	void AnalyzeGameLayer();

	virtual int RuleSetNum() { return m_aRuleSets.size(); }
	virtual const char* GetRuleSetName(int Index) const;

private:
	void PlaceDoodads(CLayerTiles *pLayer, CRule *pRule, array<array<int> > *pPositions, int Amount, int LeftWall = 0);

	array<CRuleSet> m_aRuleSets;

	array<array<int> > m_FloorIDs;
	array<array<int> > m_CeilingIDs;
	array<array<int> > m_RightWallIDs;
	array<array<int> > m_LeftWallIDs;
};

class CMapFilter
{
	struct CMFTile // custom tile because we need a bigger m_Index
	{
		int m_Index;
		char m_Flags;
		void Clear()
		{
			m_Index = CMapFilter::EMPTY;
			m_Flags = 0;
		}
	};

	int m_Width, m_Height;
	array<CMFTile> m_aFilter;
	int m_RefPointId; // reference point

	class CPattern
	{
		int m_Width, m_Height;
		array<CMFTile> m_aTiles;

	public:
		CPattern();

		void LoadTiles(const json_value& JsonVal);
		json_value* TilesToJson() const;

		void Print();

		void SetSize(int Width, int Height);
		void SetTile(int x, int y, int Index, int Flags);
		const CMFTile& GetTile(int TileID) const;
		const array<CMFTile>& GetTiles() const;
		void Clear();
		float m_Weight; // random weight
	};

	array<CPattern> m_aPatterns;

	void Apply(int TileID, CLayerTiles *pLayer);
public:

	enum
	{
		FULL=-1,
		EMPTY=0,
		MAX_SIZE=8
	};

	CMapFilter();
	explicit CMapFilter(const json_value& JsonVal);

	/**
	 * @brief Jsonify the class
	 * @return json_value* (free it)
	 */
	json_value* ToJson() const;

	void Print();

	void SetSize(int Width, int Height);
	void SetTile(int x, int y, int Index, int Flags = 0);
	void Clear();

	void AddPattern();
	void RemovePattern(int ID);
	void ClearPattern(int ID);
	void SetPatternTile(int ID, int x, int y, int Index, int Flags = 0);
	void SetPatternWeight(int ID, float Weight);
	const array<CMFTile>& GetPatternTiles(int ID) const;

	bool TryApply(int TileID, CLayerTiles *pLayer);
};

// TODO: move this?
class CAutoMapUI
{
	class CEditor *m_pEditor;

	// helper functions
	/*class IInput *Input() { return m_pInput; }
	class IClient *Client() { return m_pClient; }
	class IConsole *Console() { return m_pConsole; }*/
	class IGraphics *Graphics();
	/*class ITextRender *TextRender() { return m_pTextRender; }
	class IStorage *Storage() { return m_pStorage; }*/
	CUI *UI();

public:
	CAutoMapUI(class CEditor *pEditor);

	void Update();
	void Render();
	void RenderPatterns();
};

#endif
