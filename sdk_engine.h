#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>

#define OFF_g_dxRenderer 0x023577D4
#define OFF_InternalDatabasePartition_onPartitionLoaded 0x004D5790
#define OFF_InternalDatabasePartition_releaseInstances 0x004E4480 // thiscall (int unused) retn 4, vtable 0x20730E0 slot 1: releases every instance (+0x3C..+0x40) as the partition unloads
#define OFF_g_shaderSystem 0x02356C90 // IShaderSystem* (DxShaderSystem): ShaderDatabase* vector at +0xC..+0x10 (addDatabase sub_6A5070, vtable 0x20981A8 slot 17); getPath slot 1, getSettings slot 3
#define OFF_g_meshVariationManager 0x023B3E88 // MeshVariationManager*, the render materials (SurfaceShaderInstance parameter blocks) of every mesh variation, built by onDatabaseLoading sub_17402D0
#define OFF_MeshSet_preRegistration 0x01737740 // thiscall (compartment) retn 4: once per loaded mesh render data, layout +0x10 (bounding min +0x10, max +0x20, name +0x58)
#define OFF_MemoryArena_alloc 0x0041BE10 // thiscall (size, align) retn 8
#define OFF_ShaderParameterBlock_setVector 0x00693930 // thiscall (index, handle, const Vec4* aligned) retn 0Ch: the engine sets block vectors again with it, bf-visual-editor holds its edits here
#define OFF_Environment_getMemoryInfo 0x004AAC10 // cdecl (unsigned* total, unsigned* available): psapi GetPerformanceInfo; PerfOverlay::update sub_66F4E0 (vtable 0x208EE74 slot 3) calls it every frame
#define OFF_HealthModuleStateRevive_enter 0x007B00E0 // thiscall (): clientHealthModule revive state, vtable 0x20AF4E0 slot 25; reviveSoldier sub_7AB4C0, "Revive" sound state
#define OFF_getLocalPlayer 0x007A8120 // cdecl () -> ClientPlayer*
#define OFF_ClientPlayer_getSoldier 0x00FE1210 // thiscall () -> ClientSoldierEntity*: the weak pointer at +0x3C0, null when not on foot
#define OFF_ClientSoldierWeaponsComponent_currentWeapon 0x01049B10 // thiscall () -> ClientSoldierWeapon*: list at +0xBC by the weapon handler's index
#define OFF_ClientSoldierWeapon_vtable 0x02144710 // dtor sub_1041760; init sub_1043080 builds an attachment holder per socket (sub_1038E90)
#define OFF_ClientSoldierAimingSimulation_updateZoom 0x010D5590 // thiscall (4 dwords) retn 10h: sets the camera fov from the zoom level when it changes
#define OFF_g_clientGameContext 0x02380B58 // +0x10 what updateZoom passes to the fov multiplier
#define CLIENT_GAME_CONTEXT_PLAYER_MANAGER 0x30 // ClientPlayerManager*, null outside a level; local player at +0xBC
#define OFF_fovMultiplier 0x00F87720 // thiscall () -> float (st0): 1.0 when sub_118DAA0 (data +0x60 byte +0x62 clear) or sub_118DAC0 (byte +0x60), else Render +0x28

namespace vfunc
{
	template <typename T, size_t index, typename... Args>
	inline T callVFunc(void* thisptr, Args... args)
	{
		using Fn = T(__thiscall***)(void*, Args...);
		return (*reinterpret_cast<Fn>(thisptr))[index](thisptr, args...);
	}
}

#define VFUNC(type, name, index, args, variables) \
	type name args { return vfunc::callVFunc<type, index>variables; }

namespace fb
{
	struct Vec2
	{
		float m_x; //0x0000
		float m_y; //0x0004
	};//Size=0x0008

	inline Vec2 vec2(float x, float y) { return Vec2{ x, y }; }
	inline Vec2 operator+(const Vec2& a, const Vec2& b) { return vec2(a.m_x + b.m_x, a.m_y + b.m_y); }
	inline Vec2 operator-(const Vec2& a, const Vec2& b) { return vec2(a.m_x - b.m_x, a.m_y - b.m_y); }
	inline Vec2 operator*(const Vec2& a, float s) { return vec2(a.m_x * s, a.m_y * s); }
	inline Vec2 operator/(const Vec2& a, float s) { const float i = 1.0f / s; return a * i; }

	struct ArrayHeader
	{
		uint32_t unk;
		uint32_t size;
	};

	template <typename T>
	class Array
	{
	public:
		T* m_firstElement;

		ArrayHeader* GetHeader() const
		{
			return m_firstElement ? reinterpret_cast<ArrayHeader*>(reinterpret_cast<char*>(m_firstElement) - sizeof(ArrayHeader)) : nullptr;
		}

		uint32_t size() const
		{
			return m_firstElement ? GetHeader()->size : 0;
		}
	};

	template <typename T>
	struct EastlVector
	{
		T* m_begin;
		T* m_end;
		T* m_capacity;
		void* m_allocator;

		T* begin() const { return m_begin; }
		T* end() const { return m_end; }
	};

	class DataContainer
	{
	public:
		void* m_vtable; //0x0000
		uint16_t m_refCnt; //0x0004
		uint16_t m_flags; //0x0006
	};//Size=0x0008

	inline const char* typeName(DataContainer* object)
	{
		void* type = vfunc::callVFunc<void*, 0>(object);
		return type ? **static_cast<const char***>(type) : nullptr;
	}

	// MeshAsset (Asset Name +0x8) Materials +0x20 -> MeshMaterial Shader +0xC (SurfaceShaderInstanceDataStruct) VectorParameters +0x8
	struct VectorShaderParameter
	{
		float m_value[4]; //0x0000
		int m_type; //0x0010 ShaderParameterType, 6 Color
		const char* m_name; //0x0014
		char _0x0018[0x8];
	};//Size=0x0020

	class InternalDatabasePartition
	{
	public:
		void* m_vtable; //0x0000
		const char* m_name; //0x0004 partition path
		char _0x0008[0x34];
		EastlVector<DataContainer*> m_instances; //0x003C
	};

	// updateZoom sub_10D5590 (and activate sub_10D2390, deactivate sub_10D2490, sub_104EB30) set the camera fov to level
	class ZoomLevelData : public DataContainer
	{
	public:
		float m_fieldOfView; //0x0008 55 on the unzoomed level
		char _0x000C[0x4E];
		bool m_allowFieldOfViewScaling; //0x005A
		char _0x005B[0x1];
	};//Size=0x005C

	class SoldierAimingSimulationData : public DataContainer
	{
	public:
		Array<ZoomLevelData*> m_zoomLevels; //0x0008 [0] unzoomed
	};

	class ClientSoldierAimingSimulation
	{
	public:
		SoldierAimingSimulationData* m_data; //0x0000
		char _0x0004[0xEC];
		int m_zoomLevel; //0x00F0 updateZoom sub_10D5590: 0 unzoomed, else the weapon's zoom level (ADS, scope)
		char _0x00F4[0xC];
		float m_fovMultiplier; //0x0100 times the zoom level's FieldOfView in updateZoom
	};

	// what sub_1038E90 keeps per socket
	struct AttachmentHolder
	{
		char _0x0000[0xC];
		struct AttachmentRecord* m_1pBegin; //0x000C
		AttachmentRecord* m_1pEnd; //0x0010
		char _0x0014[0x8];
		AttachmentRecord* m_1pZoomBegin; //0x001C
		AttachmentRecord* m_1pZoomEnd; //0x0020
		char _0x0024[0x7C];
	};//Size=0x00A0

	// sub_103F350 puts the transform on the weapon bone every frame
	struct alignas(16) AttachmentRecord
	{
		unsigned char* m_meshSet; //0x0000 MeshSet
		unsigned char* m_variation; //0x0004 MeshVariationSet of variation 0 (sub_10323C0)
		unsigned short m_renderInstance; //0x0008 sub_173C100
		char _0x000A[0x6];
		float m_transform[16]; //0x0010 the socket's Transform (WeaponRegularSocketObjectData +0x20), copied at spawn
	};//Size=0x0050

	class ClientSoldierWeapon
	{
	public:
		void* m_vtable; //0x0000 OFF_ClientSoldierWeapon_vtable
		char _0x0004[0x44];
		AttachmentHolder* m_holdersBegin; //0x0048
		AttachmentHolder* m_holdersEnd; //0x004C
		char _0x0050[0xF4];
		ClientSoldierAimingSimulation* m_aiming; //0x0144 sub_102C110, updated by sub_10D6D10
	};

	class ClientSoldierWeaponsComponent
	{
	public:
		char _0x0000[0xBC];
		ClientSoldierWeapon** m_weaponsBegin; //0x00BC
		ClientSoldierWeapon** m_weaponsEnd; //0x00C0

		ClientSoldierWeapon* currentWeapon()
		{
			return reinterpret_cast<ClientSoldierWeapon*(__thiscall*)(ClientSoldierWeaponsComponent*)>(OFF_ClientSoldierWeaponsComponent_currentWeapon)(this);
		}
	};

	class ClientSoldierEntity
	{
	public:
		char _0x0000[0x20];
		float m_health; //0x0020 getHealth sub_F87BB0 (entity vtable slot 53)
		char _0x0024[0x338];
		ClientSoldierWeaponsComponent* m_weapons; //0x035C
	};

	class ClientPlayer
	{
	public:
		ClientSoldierEntity* getSoldier()
		{
			return reinterpret_cast<ClientSoldierEntity*(__thiscall*)(ClientPlayer*)>(OFF_ClientPlayer_getSoldier)(this);
		}

		// getLocalPlayer sub_FDF0E0 reads the player manager's local player without checking the manager, which only
		// exists in a level
		static ClientPlayer* GetLocal()
		{
			const auto* context = *reinterpret_cast<const unsigned char* const*>(OFF_g_clientGameContext);
			if (!context || !*reinterpret_cast<void* const*>(context + CLIENT_GAME_CONTEXT_PLAYER_MANAGER))
				return nullptr;
			return reinterpret_cast<ClientPlayer*(__cdecl*)()>(OFF_getLocalPlayer)();
		}
	};

	class ClientSoldierHealthModule
	{
	public:
		char _0x0000[0x1B8];
		ClientSoldierEntity* m_soldier; //0x01B8
	};

	// clientHealthModule::HealthModuleStateBase, the states of ClientSoldierHealthModule's state machine (+0x414)
	class HealthModuleState
	{
	public:
		void* m_vtable; //0x0000
		char _0x0004[0x18];
		ClientSoldierHealthModule* m_module; //0x001C

		// revive state sub_7BC590
		VFUNC(void, onHealthHasChanged, 28, (float oldHealth), (this, oldHealth))
	};

	struct ScreenViewport
	{
		uint16_t m_x; //0x0000
		uint16_t m_y; //0x0002
		uint16_t m_width; //0x0004
		uint16_t m_height; //0x0006
	};//Size=0x0008

	class DxRenderer
	{
	public:
		char _0x0000[0xD8];
		void* m_device; //0x00D8 ID3D11Device*
		void* m_context; //0x00DC ID3D11DeviceContext*, the immediate one
		char _0x00E0[0xC0];
		ScreenViewport m_viewport; //0x01A0 getScreenInfo sub_6A8BA0, the rect every UI component reads

		static DxRenderer* GetInstance()
		{
			return *reinterpret_cast<DxRenderer**>(OFF_g_dxRenderer);
		}
	};

	enum WidgetVerticalAlignment { WVA_Top, WVA_Center, WVA_Bottom };
	enum WidgetHorisontalAlignment { WHA_Left, WHA_Center, WHA_Right };
	enum UIListNavigationType { loop, noLoop, sendEventTopBottom, sendEventTop, sendEventBottom, none };

	enum UIWidgetEventID
	{
		UIWidgetEventID_Released = 0x03,
		UIWidgetEventID_EnterTop = 0x0B,
		UIWidgetEventID_EnterBottom = 0x0C,
		UIWidgetEventID_OnReachedTop = 0x10,
		UIWidgetEventID_OnReachedBottom = 0x11,
	};

	class Asset : public DataContainer
	{
	public:
		char* m_Name; //0x0008
	};

	class UIWidgetAsset : public Asset
	{
	public:
		Array<void> m_WidgetEvents; //0x000C WidgetEventQueryPair
	};

	struct UIDataSourceInfo
	{
		char* m_DataName; //0x0000
		void* m_DataCategory; //0x0004 UIComponentData
		int32_t m_DataKey; //0x0008
		bool m_UseDirectAccess; //0x000C
		bool m_UpdateOnInitialize; //0x000D
		char _0x000E[2];
	};//Size=0x0010

	struct DefaultSelectionItem
	{
		UIDataSourceInfo m_DefaultSelectionQuery; //0x0000
		int32_t m_DefaultSelectionIndex; //0x0010
	};//Size=0x0014

	// one row of a UINestedListDataBinding
	struct NestedList
	{
		char* m_Label; //0x0000
		char* m_Index; //0x0004
		UIDataSourceInfo m_ListDataSource; //0x0008
		UIDataSourceInfo m_DynamicShowList; //0x0018
		Array<char*> m_StaticItems; //0x0028
		DefaultSelectionItem m_DefaultHighlighted; //0x002C
		int32_t m_RowType; //0x0040 UIListRowType
		bool m_UseAsNormalListRows; //0x0044
		bool m_HiddenOnPC; //0x0045
		bool m_HiddenOnXenon; //0x0046
		bool m_HighLightOnUpdate; //0x0047
	};//Size=0x0048

	class UIDataBinding : public DataContainer
	{
	};

	// sent to the list widget by sub_88A350 (ListIndex, NavigationType, ... as members of the row data)
	class UINestedListDataBinding : public UIDataBinding
	{
	public:
		int32_t m_ListIndex; //0x0008 flash tells a page's lists apart by it
		Array<NestedList> m_NestedLists; //0x000C
		int32_t m_RowSpacing; //0x0010
		DefaultSelectionItem m_DefaultHighlightedRow; //0x0014
		UIListNavigationType m_NavigationType; //0x0028
		int32_t m_RowType; //0x002C
		int32_t m_EmptyRowType; //0x0030
		int32_t m_SelectorWidth; //0x0034
		UIDataSourceInfo m_Visibility; //0x0038
		bool m_SendIndexWithEvent; //0x0048
		bool m_UseScrollBar; //0x0049
		bool m_DataIncludesButtonLayout; //0x004A
		bool m_ClearListAtNavigationEvent; //0x004B
		bool m_Use3DSelection; //0x004C
		bool m_InvertVisible; //0x004D
		bool m_Visible; //0x004E
		bool m_ScreenRotationEnabled; //0x004F
		bool m_HighLightOnUpdate; //0x0050
		bool m_KeepScrollOffset; //0x0051
		char _0x0052[2];
	};//Size=0x0054

	class UIGraphAsset;

	struct UIWidgetProperty
	{
		char* m_Name; //0x0000
		char* m_Value; //0x0004
	};//Size=0x0008

	class UINodeData : public DataContainer
	{
	public:
		char* m_Name; //0x0008
		UIGraphAsset* m_ParentGraph; //0x000C
		bool m_IsRootNode; //0x0010
		bool m_ParentIsScreen; //0x0011
		char _0x0012[2];
	};//Size=0x0014

	// event routed by (m_InstanceName, m_Query)
	class UINodePort : public DataContainer
	{
	public:
		char* m_Name; //0x0008
		char* m_InstanceName; //0x000C
		UIWidgetEventID m_Query; //0x0010
		bool m_AllowManualRemove; //0x0014
		char _0x0015[3];
	};//Size=0x0018

	class WidgetNode : public UINodeData
	{
	public:
		UIWidgetAsset* m_WidgetAsset; //0x0014
		int32_t m_FocusIndex; //0x0018
		int32_t m_ZDepthLevel; //0x001C
		WidgetVerticalAlignment m_VerticalAlign; //0x0020
		WidgetHorisontalAlignment m_HorisontalAlign; //0x0024
		UIDataBinding* m_DataBinding; //0x0028
		Array<UIWidgetProperty> m_WidgetProperties; //0x002C
		char* m_InstanceName; //0x0030
		Array<UINodePort*> m_Inputs; //0x0034
		Array<UINodePort*> m_Outputs; //0x0038
		bool m_AlwaysInFocus; //0x003C
		char _0x003D[3];
	};//Size=0x0040

	// writes its DataSource with the incoming event's value (or Param)
	class DataSetNode : public UINodeData
	{
	public:
		UINodePort* m_In; //0x0014
		UINodePort* m_Out; //0x0018
		char* m_Param; //0x001C
		UIDataSourceInfo m_DataSource; //0x0020
		bool m_SetToEmptyString; //0x0030
		bool m_ForceUpdate; //0x0031
		char _0x0032[2];
	};//Size=0x0034

	class UINodeConnection : public DataContainer
	{
	public:
		UINodeData* m_SourceNode; //0x0008
		UINodeData* m_TargetNode; //0x000C
		UINodePort* m_SourcePort; //0x0010
		UINodePort* m_TargetPort; //0x0014
		int32_t m_NumScreensToPop; //0x0018
	};//Size=0x001C

	// UIScreenAsset: the screen's flow graph
	class UIGraphAsset : public Asset
	{
	public:
		Array<UINodeData*> m_Nodes; //0x000C
		void* m_GlobalNode; //0x0010
		Array<UINodeConnection*> m_Connections; //0x0014
	};

	// node key: variation hash | MeshAsset NameHash << 32 (getMeshVariation sub_17398C0)
	struct MeshVariationNode
	{
		unsigned long long key; //0x0000
		unsigned char* set; //0x0008 MeshVariationSet: SurfaceShaderInstance array +0x8, count +0xC
		int refCount; //0x000C
		MeshVariationNode* next; //0x0010
	};
	struct MeshVariationManager
	{
		char _0x0000[0x20];
		void* heap; //0x0020 sub_17402D0 allocs
		char _0x0024[0x4];
		MeshVariationNode** buckets; //0x0028
		unsigned int bucketCount; //0x002C
		unsigned int elementCount; //0x0030
	};
	constexpr int SET_INSTANCES = 0x8;
	constexpr int SET_INSTANCE_COUNT = 0xC;
	constexpr int INSTANCE_STRIDE = 0xC; // SurfaceShaderInstance: SurfaceShader* +0x0, parameter block +0x4, header +0x8

	// DxShaderDatabase (vtable 0x209841C): every solution and pixel permutation of a loaded shader database; createShaders
	// sub_69E730 makes each permutation's shader from its DXBC at load (job sub_69E690)
	constexpr int SHADER_SYSTEM_DATABASES = 0xC;
	constexpr int DATABASE_SOLUTION_COUNT = 0x190;
	constexpr int DATABASE_PIXEL_COUNT = 0x194;
	constexpr int DATABASE_SOLUTIONS = 0x1A4; // DxShaderSolution[], 0x40 each
	constexpr int DATABASE_PIXEL_PERMUTATIONS = 0x1A8; // PixelShaderPermutation[], 0x2C each
	constexpr int SHADER_SOLUTION_SIZE = 0x40;
	constexpr int PIXEL_PERMUTATION_SIZE = 0x2C;

	struct PixelShaderPermutation
	{
		char _0x0000[0x1C];
		void* m_shader; //0x001C ID3D11PixelShader*, null until made from m_data
		unsigned int m_metrics; //0x0020
		const unsigned char* m_data; //0x0024 DXBC
		unsigned int m_dataSize; //0x0028
	};
	struct DxShaderSolution
	{
		char _0x0000[0x9];
		unsigned char m_surfaceType; //0x0009 3, 4 blended (DxShaderDispatcher::applyBlendState)
		unsigned char m_blendMode; //0x000A ShaderBlendMode: 0 lerp (premultiplied, ONE / INV_SRC_ALPHA), 1 additive (ONE / ONE)
		char _0x000B[0x15];
		PixelShaderPermutation* m_pixelPermutation; //0x0020
	};
}