#pragma once

#include "sdk_engine.h"

// hud scale
#define OFF_g_uiSystem 0x02385CE0 // IUISystem::getInstance sub_FF6120
#define OFF_UISystem_renderJob 0x0100BC40 // cdecl (UISystem*, dt): viewport vs backbuffer rect -> onFramebufferResized sub_1005E10, then UIEngine::update
#define OFF_UIMovieInstance_onViewResized 0x01766A80 // thiscall (x, y, w, h): NoScale at >= 1280x720, ShowAll below
#define OFF_UI3dIcon_updatePosition 0x0094FAF0 // thiscall (dt, Vec2*, toMovie): toMovie pixel -> movie conversion only below 720p
#define OFF_UI3dIconComp_getScreenCoordinate 0x00952470 // thiscall (dt, UI3dIcon*, UI3dPosInfo*)
#define OFF_UIHud_draw3dIcons 0x00927A30 // thiscall (dt): nametags, vehicles, markers, laser tags - every UIHud draw follows a getScreenCoordinate
#define OFF_UIKillfeed_draw 0x0092D630 // thiscall (dt): right-aligned at floor(0.95 * w) + x, layout from y = 100
#define OFF_UIMinimap_updateScale 0x009366E0 // thiscall (): m_resolutionScale = min(1, w/1280, h/720)
#define OFF_UIMinimap_render 0x0094AB90 // thiscall (const Mat4*, a3, a4): icons (drawFromAtlas) and labels (drawText sub_940E00) into the minimap texture
#define OFF_UIHud_drawFromAtlas 0x0077ACB0 // thiscall (Vec2* outSize, UIHudIconDrawParams*, time, pad, rotation)
#define OFF_UIHud_drawText 0x007849B0 // thiscall (GRectF* out, Vec2* pos, text, size, glow, halign, valign, snap): out = local text rect at the base size
#define OFF_UIScaleformRenderer_ctor 0x01770370 // thiscall (a2, a3, bool smallGlyphCache): glyph cache 1024 if set, else 2048; only caller UIEngine create sub_1770A80, whose init sub_1768640 hands it to GFxFontCacheManager::SetTextureConfig
#define OFF_UIScreenManager_initializeScreen 0x0102AAD0 // thiscall (const char* screen): the screen's WidgetNodes -> "<screen>.instance1.initializeScreen", which only sets up clips its movie already has
#define OFF_GFxMovieRoot_advance 0x01429DF0 // thiscall (dt, catchUp) -> float, vtable 0x2201DA8 slot 37; ActionScript runs here, Invoke is safe after it
#define OFF_GFxMovieRoot_display 0x014202F0 // thiscall (), vtable 0x2201DA8 slot 38; renderJob sub_176CF60 advances every movie, then displays them
#define OFF_GFxValue_getDisplayInfo 0x012F0000 // ObjectInterface thiscall (data, GFxDisplayInfo*)
#define OFF_GFxValue_setDisplayInfo 0x012F0240 // ObjectInterface thiscall (data, const GFxDisplayInfo*): only m_varsSet fields
#define OFF_GFxValue_objectRelease 0x012F5310 // ObjectInterface thiscall (GFxValue*, data)
#define OFF_GFxValue_invoke 0x012F6440 // ObjectInterface thiscall (data, GFxValue* result, name, const GFxValue* args, nargs, bool isDisplayObject)
#define OFF_GFxValue_getMember 0x012F80F0 // ObjectInterface thiscall (data, name, GFxValue* out, bool isDisplayObject)
#define OFF_GFxValue_setMember 0x012EFAE0 // ObjectInterface thiscall (data, name, const GFxValue* value, bool isDisplayObject)

// options menu rows
#define OFF_UIComponentManager_registerComponentDataKeys 0x01020B60 // thiscall (UIComponentData*, IUIComponent*): key = djb2xor(upper("UI_<short>_<source>"))
#define OFF_UISettingsComp_getDataValue 0x00864FB0 // thiscall (key, UIDataValue*) -> bool, vtable 0x20B8F84 slot 9
#define OFF_UISettingsComp_setDataValue 0x0085D740 // thiscall (key, UIDataValue*) -> bool, slot 10; rows land here via SetOptionKey
#define OFF_UIDataValue_setMember 0x0100D140 // thiscall (name, const UIDataValue*): deep copy into this value's arena
#define OFF_UIDataValue_destroy 0x01005970 // thiscall ()
#define OFF_UIDataValue_makeContainer 0x01009A70 // thiscall (count)
#define OFF_UIDataValue_setEnumItem 0x00906650 // cdecl (container, index, label, index value): {Index, Label} item
#define OFF_g_uiDataArena 0x022E8BE0 // arena object scalar UIDataValues are built with
#define OFF_g_profileOptions 0x02384D74 // ProfileOptions*, the profile options (PROF_SAVE_profile / PROF_SAVE_body)
#define OFF_SettingsGroup_setFloat 0x00FBB4D0 // thiscall (name, float): inserts unknown names (name copied)
#define OFF_SettingsGroup_getFloat 0x00FB8B40 // thiscall (name, float fallback) -> float in st0
#define OFF_ProfileOptions_groupChanged 0x00FAFE50 // thiscall (group): what UISettingsComp::setDataValue calls to save a group
#define OFF_ProfileOptions_loadProfile 0x00FBB950 // thiscall (data, size, text, textSize): merges the saved profile into the live groups (+0x10), only caller is the load-done message in onMessage sub_FBF370
#define OFF_ProfileOptions_resetProfile 0x00FBD7A0 // thiscall (): clears all 4 banks of 9 groups, refills them from the settings asset; ctor sub_FBEC40 and profile resets
#define OFF_SettingsGroup_assign 0x00FBD680 // thiscall (const SettingsGroup* source): clear + copy; banks +0x10 live, +0x250 last loaded/saved, +0x490 defaults, +0x6D0 pending save
#define OFF_vt_UIStackArena 0x020C26AC // bump allocator, cursor at +0x208, no bounds check, free is a no-op

namespace fb
{
	// GFx 3.x GViewport, copied whole by GFxMovieRoot::SetViewport sub_1426160
	struct GViewport
	{
		int m_bufferWidth; //0x0000
		int m_bufferHeight; //0x0004
		int m_left; //0x0008
		int m_top; //0x000C
		int m_width; //0x0010
		int m_height; //0x0014
		int m_scissorLeft; //0x0018
		int m_scissorTop; //0x001C
		int m_scissorWidth; //0x0020
		int m_scissorHeight; //0x0024
		float m_scale; //0x0028 NoScale visible frame = viewport * m_scale * m_aspectRatio (UpdateViewport sub_141B0D0)
		float m_aspectRatio; //0x002C
		unsigned int m_flags; //0x0030
	};//Size=0x0034

	struct GRectF
	{
		float m_left; //0x0000
		float m_top; //0x0004
		float m_right; //0x0008
		float m_bottom; //0x000C
	};//Size=0x0010

	enum GFxScaleMode { GFxScaleMode_NoScale = 0, GFxScaleMode_ShowAll = 1 };
	enum GFxAlign { GFxAlign_Center = 0 };

	// m_type & 0x40 release with ObjectInterface::ObjectRelease
	struct GFxValue
	{
		void* m_objectInterface; //0x0000
		unsigned int m_type; //0x0004 3 number, 8 display object, 0x40 managed
		union
		{
			void* m_data;
			double m_number;
		}; //0x0008
	};//Size=0x0010

	// ObjectInterface::GetDisplayInfo sub_12F0000 / SetDisplayInfo sub_12F0240
	struct alignas(16) GFxDisplayInfo
	{
		double m_x; //0x0000
		double m_y; //0x0008
		double m_rotation; //0x0010
		double m_xScale; //0x0018 percent
		double m_yScale; //0x0020
		double m_alpha; //0x0028
		bool m_visible; //0x0030
		char _0x0031[7];
		double m_z; //0x0038
		double m_xRotation; //0x0040
		double m_yRotation; //0x0048
		double m_zScale; //0x0050
		double m_fov; //0x0058
		float m_viewMatrix3D[16]; //0x0060
		float m_perspectiveMatrix3D[16]; //0x00A0
		unsigned short m_varsSet; //0x00E0 BITFLAG 1 x, 2 y, 8 xscale, 0x10 yscale
	};

	// vtable 0x2201DA8 GFxMovieRoot view slots - 5
	class GFxMovieView
	{
	public:
		VFUNC(bool, getVariable, 17, (GFxValue* out, const char* path), (this, out, path));
		VFUNC(void, setViewport, 25, (const GViewport* vp), (this, vp));
		VFUNC(void, setViewScaleMode, 27, (GFxScaleMode mode), (this, mode));
		VFUNC(void, setViewAlignment, 29, (GFxAlign align), (this, align));
		VFUNC(GRectF*, getVisibleFrameRect, 31, (GRectF* out), (this, out)); // stage units
		VFUNC(void, setSafeRect, 35, (const GRectF* rect), (this, rect));
	};

	// ctor sub_1766900, onViewResized sub_1766A80
	class UIMovieInstance
	{
	public:
		char _0x0000[0xC];
		GFxMovieView* m_movieView; //0x000C
		float m_safeAreaFractionWidth; //0x0010 ScreenSafeAreaWidth setting
		float m_safeAreaFractionHeight; //0x0014
	};

	// setWidgetFocus sub_1016470
	// this is where widget live
	class UIScreenData
	{
	public:
		char _0x0000[0x28];
		WidgetNode** m_widgetsBegin; //0x0028
		WidgetNode** m_widgetsEnd; //0x002C
		char _0x0030[0x3C];
		char m_instanceName[64]; //0x006C
	};

	// screen list scanned by showScreen's caller sub_101E1E0
	class UIScreenManager
	{
	public:
		char _0x0000[0xC];
		UIScreenData** m_screensBegin; //0x000C
		UIScreenData** m_screensEnd; //0x0010
		char _0x0014[0x1C];
		UIMovieInstance* m_rootMovie; //0x0030
	};

	// IUISystem::getInstance sub_FF6120, renderJob sub_100BC40
	class UISystem
	{
	public:
		char _0x0000[0x28];
		UIScreenManager* m_screenManager; //0x0028 UISystem vtable 0x2143A68 forwards to it
		char _0x002C[0xC];
		unsigned int m_backbufferX; //0x0038
		unsigned int m_backbufferY; //0x003C
		unsigned int m_backbufferWidth; //0x0040
		unsigned int m_backbufferHeight; //0x0044 postInit sub_100A680 zeroes it to force onFramebufferResized

		VFUNC(void, dataKeyChanged, 24, (int key, bool immediate), (this, key, immediate)); // sub_FF60B0, re-reads the key's bound rows

		static UISystem* GetInstance()
		{
			return *(UISystem**)OFF_g_uiSystem;
		}
	};

	// name -> value map, one per options page; sub_FB6090 lookup under the lock at +0
	class SettingsGroup
	{
	public:
		char _0x0000[0x40];
	};//Size=0x0040

	class ProfileOptions
	{
	public:
		char _0x0000[0x10];
		SettingsGroup m_groups[9]; //0x0010 0 audio, 1 render, 2 input, 4 gameplay

		static ProfileOptions* GetInstance()
		{
			return *(ProfileOptions**)OFF_g_profileOptions;
		}
	};

	// drawText sub_7849B0 packs both colors to bytes per call, alpha into the top byte
	class UIHud
	{
	public:
		char _0x0000[0x10];
		float m_textColor[4]; //0x0010 rgba
		float m_glowColor[4]; //0x0020 rgba
	};

	// UIHud::drawFromAtlas sub_77ACB0 params
	struct UIHudIconDrawParams
	{
		Vec2 m_pos; //0x0000
		Vec2 m_size; //0x0008 0 = atlas size
		Vec2 m_flip; //0x0010
		int m_icon; //0x0018
		int m_state; //0x001C
		float m_scale; //0x0020
		unsigned int m_color; //0x0024 vertex color, alpha in the top byte
	};//Size=0x0028

	// UIDataValue: 1 container, 3 double, 4 int, 6 C string (not owned); flags low bits 1 container, 3 scalar
	struct UIDataValue
	{
		void* m_arena; //0x0000
		char _0x0004[4];
		union
		{
			double m_number;
			int m_int;
			const char* m_string;
		}; //0x0008
		int m_type; //0x0010
		unsigned char m_flags; //0x0014
		char _0x0015[3];
	};//Size=0x0018

	// getDataValue's temporary arena sub_9067D0
	// cursor only used
	struct UIStackArena
	{
		void* m_vtable; //0x0000
		int m_refCount; //0x0004
		char m_buffer[0x200]; //0x0008
		char* m_cursor; //0x0208
	};//Size=0x020C

	// UI3dIconComp::getScreenCoordinate sub_952470 output
	struct UI3dPosInfo
	{
		Vec2 m_pos; //0x0000
		float m_scale; //0x0008 distance scale ONLY!
		float m_alpha; //0x000C
	};//Size=0x0010

	// sub_9366E0 writes min(1, w/1280, h/720) every minimap render, sub_94B060 resets it to 1
	class UIMinimap
	{
	public:
		char _0x0000[0x380];
		float m_resolutionScale; //0x0380 icon and label scale
	};

	// message expire is -1 always
	struct MessageInfo
	{
		char* m_RowTypeName; //0x0000
		uint32_t m_MessageQueueSize; //0x0004 rows kept on screen
		float m_NormalMessageTime; //0x0008
		float m_ShortMessageTime; //0x000C
	};//Size=0x0010

	// UIMessageComp reads it through getMessageInfoForType sub_92DB50: +0x20 for type 0 (chat), +0x20 + type * 0x10 up to 15
	class UIMessageCompData : public DataContainer
	{
	public:
		char _0x0008[0x14];
		float m_ScoreAggregateTime; //0x001C
		MessageInfo m_ChatMessageInfo; //0x0020
	};
}
