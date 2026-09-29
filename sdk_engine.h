#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>

#define OFF_g_dxRenderer 0x023577D4
#define OFF_InternalDatabasePartition_onPartitionLoaded 0x004D5790

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

	class InternalDatabasePartition
	{
	public:
		void* m_vtable; //0x0000
		const char* m_name; //0x0004 partition path
		char _0x0008[0x34];
		EastlVector<DataContainer*> m_instances; //0x003C
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
		char _0x0000[0x1A0];
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
		Array<void> m_WidgetProperties; //0x002C UIWidgetProperty
		char* m_InstanceName; //0x0030
		Array<UINodePort*> m_Inputs; //0x0034
		Array<UINodePort*> m_Outputs; //0x0038
		bool m_AlwaysInFocus; //0x003C
		char _0x003D[3];
	};//Size=0x0040

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
}