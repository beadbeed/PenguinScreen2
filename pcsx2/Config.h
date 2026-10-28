// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "Host/AudioStreamTypes.h"

#include "common/Pcsx2Defs.h"
#include "common/FPControl.h"

#include <array>
#include <string>
#include <optional>
#include <vector>

#define BITFIELD32() \
	union \
	{ \
		u32 bitset; \
		struct \
		{
#define BITFIELD_END \
	} \
	; \
	} \
	;

class Error;
class SettingsInterface;
class SettingsWrapper;

enum class CDVD_SourceType : uint8_t;

namespace Pad
{
	enum class ControllerType : u8;
}

struct SettingInfo
{
	using GetOptionsCallback = std::vector<std::pair<std::string, std::string>> (*)();

	enum class Type
	{
		Boolean,
		Integer,
		IntegerList,
		Float,
		String,
		StringList,
		Path,
	};

	Type type;
	const char* name;
	const char* display_name;
	const char* description;
	const char* default_value;
	const char* min_value;
	const char* max_value;
	const char* step_value;
	const char* format;
	const char* const* options;
	GetOptionsCallback get_options;
	float multiplier;

	const char* StringDefaultValue() const;
	bool BooleanDefaultValue() const;
	s32 IntegerDefaultValue() const;
	s32 IntegerMinValue() const;
	s32 IntegerMaxValue() const;
	s32 IntegerStepValue() const;
	float FloatDefaultValue() const;
	float FloatMinValue() const;
	float FloatMaxValue() const;
	float FloatStepValue() const;

	void SetDefaultValue(SettingsInterface* si, const char* section, const char* key) const;
	void CopyValue(SettingsInterface* dest_si, const SettingsInterface& src_si,
		const char* section, const char* key) const;
};

enum class GenericInputBinding : u8;

struct InputBindingInfo
{
	enum class Type : u8
	{
		Unknown,
		Button,
		Axis,
		HalfAxis,
		Motor,
		Pointer,
		Keyboard,
		Device,
		Macro,
	};

	const char* name;
	const char* display_name;
	const char* icon_name;
	Type bind_type;
	u16 bind_index;
	GenericInputBinding generic_mapping;
};

enum class GenericInputBinding : u8
{
	Unknown,

	DPadUp,
	DPadRight,
	DPadLeft,
	DPadDown,

	LeftStickUp,
	LeftStickRight,
	LeftStickDown,
	LeftStickLeft,
	L3,

	RightStickUp,
	RightStickRight,
	RightStickDown,
	RightStickLeft,
	R3,

	Triangle,
	Circle,
	Cross,
	Square,

	Select,
	Start,
	System,

	L1,
	L2,
	R1,
	R2,

	SmallMotor,
	LargeMotor,

	Count,
};

enum GamefixId
{
	GamefixId_FIRST = 0,

	Fix_FpuMultiply = GamefixId_FIRST,
	Fix_GoemonTlbMiss,
	Fix_SoftwareRendererFMV,
	Fix_SkipMpeg,
	Fix_OPHFlag,
	Fix_EETiming,
	Fix_InstantDMA,
	Fix_DMABusy,
	Fix_GIFFIFO,
	Fix_VIFFIFO,
	Fix_VIF1Stall,
	Fix_VuAddSub,
	Fix_Ibit,
	Fix_VUSync,
	Fix_VUOverflow,
	Fix_XGKick,
	Fix_BlitInternalFPS,
	Fix_FullVU0Sync,

	GamefixId_COUNT
};

enum class SpeedHack
{
	MVUFlag,
	InstantVU1,
	MTVU,
	EECycleRate,
	MaxCount,
};

enum class DebugAnalysisCondition
{
	ALWAYS,
	IF_DEBUGGER_IS_OPEN,
	NEVER
};

struct DebugSymbolSource
{
	std::string Name;
	bool ClearDuringAnalysis = false;

	friend auto operator<=>(const DebugSymbolSource& lhs, const DebugSymbolSource& rhs) = default;
};

struct DebugExtraSymbolFile
{
	std::string Path;
	std::string BaseAddress;
	std::string Condition;

	friend auto operator<=>(const DebugExtraSymbolFile& lhs, const DebugExtraSymbolFile& rhs) = default;
};

enum class DebugFunctionScanMode
{
	SCAN_ELF,
	SCAN_MEMORY,
	SKIP
};

enum class AspectRatioType : u8
{
	Stretch,
	RAuto4_3_3_2,
	R4_3,
	R16_9,
	R10_7,
	MaxCount
};

enum class FMVAspectRatioSwitchType : u8
{
	Off,
	RAuto4_3_3_2,
	R4_3,
	R16_9,
	R10_7,
	MaxCount
};

enum class MemoryCardType
{
	Empty,
	File,
	Folder,
	MaxCount
};

enum class MemoryCardFileType
{
	Unknown,
	PS2_8MB,
	PS2_16MB,
	PS2_32MB,
	PS2_64MB,
	PS1,
	MaxCount
};

enum class LimiterModeType : u8
{
	Nominal,
	Turbo,
	Slomo,
	Unlimited,
};

enum class GSRendererType : s8
{
	Auto = -1,
	DX11 = 3,
	Null = 11,
	OGL = 12,
	SW = 13,
	VK = 14,
	Metal = 17,
	DX12 = 15,
};

enum class GSVSyncMode : u8
{
	Disabled,
	FIFO,
	Mailbox,
	Count
};

enum class GSInterlaceMode : u8
{
	Automatic,
	Off,
	WeaveTFF,
	WeaveBFF,
	BobTFF,
	BobBFF,
	BlendTFF,
	BlendBFF,
	AdaptiveTFF,
	AdaptiveBFF,
	Count
};

enum class GSPostBilinearMode : u8
{
	Off,
	BilinearSmooth,
	BilinearSharp,
};

enum class BiFiltering : u8
{
	Nearest,
	Forced,
	PS2,
	Forced_But_Sprite,
};

enum class TriFiltering : s8
{
	Automatic = -1,
	Off,
	PS2,
	Forced,
};

enum class AccBlendLevel : u8
{
	Minimum,
	Basic,
	Medium,
	High,
	Full,
	Maximum,
	MaxCount
};

enum class OsdOverlayPos : u8
{
	None,
	TopLeft,
	TopCenter,
	TopRight,
	CenterLeft,
	Center,
	CenterRight,
	BottomLeft,
	BottomCenter,
	BottomRight,
};

enum class TexturePreloadingLevel : u8
{
	Off,
	Partial,
	Full,
};

enum class GSScreenshotSize : u8
{
	WindowResolution,
	InternalResolution,
	InternalResolutionUncorrected,
};

enum class GSScreenshotFormat : u8
{
	PNG,
	JPEG,
	WebP,
	Count,
};

enum class GSDumpCompressionMethod : u8
{
	Uncompressed,
	LZMA,
	Zstandard,
};

enum class SavestateCompressionMethod : u8
{
	Uncompressed = 0,
	Deflate = 1,
	Zstandard = 2
};

enum class SavestateCompressionLevel : u8
{
	Low = 0,
	Medium = 1,
	High = 2,
	VeryHigh = 3,
};

enum class GSHardwareDownloadMode : u8
{
	Enabled,
	EnabledForceFull,
	NoReadbacks,
	Unsynchronized,
	Disabled
};

enum class GSCASMode : u8
{
	Disabled,
	SharpenOnly,
	SharpenAndResize,
};

enum class GSHWAutoFlushLevel : u8
{
	Disabled,
	SpritesOnly,
	Enabled,
};

enum class GSGPUTargetCLUTMode : u8
{
	Disabled,
	Enabled,
	InsideTarget,
};

enum class GSTextureInRtMode : u8
{
	Disabled,
	InsideTargets,
	MergeTargets,
};

enum class GSLimit24BitDepth : u8
{
	Disabled,
	PrioritizeUpper,
	PrioritizeLower,
};

enum class GSBilinearDirtyMode : u8
{
	Automatic,
	ForceBilinear,
	ForceNearest,
	MaxCount
};

enum class GSHalfPixelOffset : u8
{
	Off,
	Normal,
	Special,
	SpecialAggressive,
	Native,
	NativeWTexOffset,
	MaxCount
};

enum class GSNativeScaling : u8
{
	Off,
	Normal,
	Aggressive,
	NormalUpscaled,
	AggressiveUpscaled,
	MaxCount
};

enum class GSDepthFeedbackMode : u8
{
	None      = 0,
	Auto      = 1,
	Depth     = 2,
	DepthAsRT = 3,
};

enum class AchievementOverlayPosition : u8
{
	TopLeft,
	TopCenter,
	TopRight,
	CenterLeft,
	Center,
	CenterRight,
	BottomLeft,
	BottomCenter,
	BottomRight,
	MaxCount
};

struct TraceLogsEE
{
	BITFIELD32()
	bool
		bios : 1,
		memory : 1,
		giftag : 1,
		vifcode : 1,
		mskpath3 : 1,
		r5900 : 1,
		cop0 : 1,
		cop1 : 1,
		cop2 : 1,
		cache : 1,
		knownhw : 1,
		unknownhw : 1,
		dmahw : 1,
		ipu : 1,
		dmac : 1,
		counters : 1,
		spr : 1,
		vif : 1,
		gif : 1;
	BITFIELD_END

	TraceLogsEE();

	bool operator==(const TraceLogsEE& right) const;
	bool operator!=(const TraceLogsEE& right) const;
};

struct TraceLogsIOP
{
	BITFIELD32()
	bool
		bios : 1,
		memcards : 1,
		pad : 1,
		r3000a : 1,
		cop2 : 1,
		memory : 1,
		knownhw : 1,
		unknownhw : 1,
		dmahw : 1,
		dmac : 1,
		counters : 1,
		cdvd : 1,
		mdec : 1;
	BITFIELD_END

	TraceLogsIOP();

	bool operator==(const TraceLogsIOP& right) const;
	bool operator!=(const TraceLogsIOP& right) const;
};

struct TraceLogsMISC
{
	BITFIELD32()
	bool
		sif : 1;
	BITFIELD_END

	TraceLogsMISC();

	bool operator==(const TraceLogsMISC& right) const;
	bool operator!=(const TraceLogsMISC& right) const;
};

struct TraceLogFilters
{
	bool Enabled;

	TraceLogsEE EE;
	TraceLogsIOP IOP;
	TraceLogsMISC MISC;

	TraceLogFilters();

	void LoadSave(SettingsWrapper& ini);
	void SyncToConfig() const;
	bool operator==(const TraceLogFilters& right) const;
	bool operator!=(const TraceLogFilters& right) const;
};

struct Pcsx2Config
{
	struct ProfilerOptions
	{
		BITFIELD32()
		bool
			Enabled : 1,
			RecBlocks_EE : 1,
			RecBlocks_IOP : 1,
			RecBlocks_VU0 : 1,
			RecBlocks_VU1 : 1;
		BITFIELD_END

		ProfilerOptions();
		void LoadSave(SettingsWrapper& wrap);

		bool operator==(const ProfilerOptions& right) const;
		bool operator!=(const ProfilerOptions& right) const;
	};

	struct RecompilerOptions
	{
		BITFIELD32()
		bool
			EnableEE : 1,
			EnableIOP : 1,
			EnableVU0 : 1,
			EnableVU1 : 1;

		bool
			vu0Overflow : 1,
			vu0ExtraOverflow : 1,
			vu0SignOverflow : 1,
			vu0Underflow : 1;

		bool
			vu1Overflow : 1,
			vu1ExtraOverflow : 1,
			vu1SignOverflow : 1,
			vu1Underflow : 1;

		bool
			fpuOverflow : 1,
			fpuExtraOverflow : 1,
			fpuFullMode : 1;

		bool
			EnableEECache : 1;
		bool
			EnableFastmem : 1;
		bool
			PauseOnTLBMiss : 1;
		BITFIELD_END

		RecompilerOptions();
		void ApplySanityCheck();

		void LoadSave(SettingsWrapper& wrap);

		bool operator==(const RecompilerOptions& right) const;
		bool operator!=(const RecompilerOptions& right) const;

		u32 GetEEClampMode() const;
		void SetEEClampMode(u32 value);

		u32 GetVUClampMode() const;
	};

	struct CpuOptions
	{
		BITFIELD32()
		bool
			ExtraMemory : 1;
		BITFIELD_END

		RecompilerOptions Recompiler;

		FPControlRegister FPUFPCR;
		FPControlRegister FPUDivFPCR;
		FPControlRegister VU0FPCR;
		FPControlRegister VU1FPCR;

		CpuOptions();
		void LoadSave(SettingsWrapper& wrap);
		void ApplySanityCheck();

		bool CpusChanged(const CpuOptions& right) const;

		bool operator==(const CpuOptions& right) const;
		bool operator!=(const CpuOptions& right) const;
	};

	struct GSOptions
	{
		static const char* AspectRatioNames[];
		static const char* FMVAspectRatioSwitchNames[];
		static const char* BlendingLevelNames[];
		static const char* CaptureContainers[];

		static const char* GetRendererName(GSRendererType type);

		static std::optional<bool> TriStateToOptionalBoolean(int value);

		static constexpr float DEFAULT_FRAME_RATE_NTSC = 59.94f;
		static constexpr float DEFAULT_FRAME_RATE_PAL = 50.00f;

		static constexpr GSRendererType DEFAULT_HW_RENDERER = GSRendererType::Auto;

		static constexpr AspectRatioType DEFAULT_ASPECT_RATIO = AspectRatioType::RAuto4_3_3_2;
		static constexpr GSInterlaceMode DEFAULT_INTERLACE_MODE = GSInterlaceMode::Automatic;
		static constexpr GSPostBilinearMode DEFAULT_BILINEAR_FILTERING_MODE = GSPostBilinearMode::BilinearSmooth;
		static constexpr FMVAspectRatioSwitchType DEFAULT_FMV_ASPECT_RATIO = FMVAspectRatioSwitchType::Off;
		static constexpr GSCASMode DEFAULT_CAS_MODE = GSCASMode::Disabled;

		static constexpr float DEFAULT_UPSCALE_MULTIPLIER = 1.0f;
		static constexpr AccBlendLevel DEFAULT_BLENDING_ACCURACY = AccBlendLevel::Basic;
		static constexpr BiFiltering DEFAULT_TEXTURE_FILTERING_MODE = BiFiltering::PS2;
		static constexpr TriFiltering DEFAULT_TRILINEAR_FILTERING_MODE = TriFiltering::Automatic;

		static constexpr float DEFAULT_OSD_SCALE = 100.0f;
		static constexpr float DEFAULT_OSD_MARGIN = 10.0f;
		static constexpr OsdOverlayPos DEFAULT_OSD_MESSAGE_POS = OsdOverlayPos::TopLeft;
		static constexpr OsdOverlayPos DEFAULT_OSD_PERFORMANCE_POS = OsdOverlayPos::TopRight;

		static constexpr int DEFAULT_VIDEO_CAPTURE_BITRATE = 6000;
		static constexpr int DEFAULT_VIDEO_CAPTURE_WIDTH = 640;
		static constexpr int DEFAULT_VIDEO_CAPTURE_HEIGHT = 480;
		static constexpr int DEFAULT_AUDIO_CAPTURE_BITRATE = 192;
		static const char* DEFAULT_CAPTURE_CONTAINER;

		static constexpr int DEFAULT_SHADEBOOST_BRIGHTNESS = 50;
		static constexpr int DEFAULT_SHADEBOOST_CONTRAST = 50;
		static constexpr int DEFAULT_SHADEBOOST_GAMMA = 50;
		static constexpr int DEFAULT_SHADEBOOST_SATURATION = 50;

		union
		{
			u64 bitsets[2];

			struct
			{
				bool
					SynchronousMTGS : 1,
					VsyncEnable : 1,
					DisableMailboxPresentation : 1,
					ExtendedUpscalingMultipliers : 1,
					PCRTCAntiBlur : 1,
					DisableInterlaceOffset : 1,
					PCRTCOffsets : 1,
					PCRTCOverscan : 1,
					IntegerScaling : 1,
					UseDebugDevice : 1,
					UseDebugBlend : 1,
					UseBlitSwapChain : 1,
					DisableShaderCache : 1,
					DisableFramebufferFetch : 1,
					DisableVertexShaderExpand : 1,
					SkipDuplicateFrames : 1,
					OsdShowSpeed : 1,
					OsdShowFPS : 1,
					OsdShowVPS : 1,
					OsdShowResolution : 1,
					OsdShowGSStats : 1,
					OsdShowCPU : 1,
					OsdShowGPU : 1,
					OsdShowGPUDebug : 1,
					OsdShowGPUStats : 1,
					OsdShowIndicators : 1,
					OsdShowFrameTimes : 1,
					OsdShowHardwareInfo : 1,
					OsdShowVersion : 1,
					OsdShowSettings : 1,
					OsdshowPatches : 1,
					OsdShowInputs : 1,
					OsdShowVideoCapture : 1,
					OsdShowInputRec : 1,
					OsdShowTextureReplacements : 1,
					OsdBoldText : 1,
					HWSpinGPUForReadbacks : 1,
					HWSpinCPUForReadbacks : 1,
					GPUPaletteConversion : 1,
					AutoFlushSW : 1,
					PreloadFrameWithGSData : 1,
					Mipmap : 1,
					HWMipmap : 1,
					HWAccurateAlphaTest : 1,
					HWAA1 : 1,
					HWROV : 1,
					HWROVLogging : 1,
					HWROVBarriersVK : 1,
					ManualUserHacks : 1,
					UserHacks_AlignSpriteX : 1,
					UserHacks_CPUFBConversion : 1,
					UserHacks_ReadTCOnClose : 1,
					UserHacks_DisableDepthSupport : 1,
					UserHacks_DisablePartialInvalidation : 1,
					UserHacks_DisableSafeFeatures : 1,
					UserHacks_DisableRenderFixes : 1,
					UserHacks_MergePPSprite : 1,
					UserHacks_ForceEvenSpritePosition : 1,
					UserHacks_NativePaletteDraw : 1,
					UserHacks_EstimateTextureRegion : 1,
					UserHacks_DrawBuffering : 1,
					FXAA : 1,
					ShadeBoost : 1,
					DumpGSData : 1,
					SaveRT : 1,
					SaveFrame : 1,
					SaveTexture : 1,
					SaveDepth : 1,
					SaveAlpha : 1,
					SaveInfo : 1,
					SaveTransferImages : 1,
					SaveDrawStats : 1,
					SaveFrameStats : 1,
					SaveHWConfig : 1,
					DumpReplaceableTextures : 1,
					DumpReplaceableMipmaps : 1,
					DumpTexturesWithFMVActive : 1,
					DumpDirectTextures : 1,
					DumpPaletteTextures : 1,
					ClassicTextureNames : 1,
					LoadTextureReplacements : 1,
					LoadTextureReplacementsAsync : 1,
					PrecacheTextureReplacements : 1,
					EnableVideoCapture : 1,
					EnableVideoCaptureParameters : 1,
					VideoCaptureAutoResolution : 1,
					EnableAudioCapture : 1,
					EnableAudioCaptureParameters : 1,
					OrganizeSnapshotsByGame : 1,
					OrganizeVideoCaptureByGame : 1;
			};
		};

		int VsyncQueueSize = 2;

		float FramerateNTSC = DEFAULT_FRAME_RATE_NTSC;
		float FrameratePAL = DEFAULT_FRAME_RATE_PAL;

		AspectRatioType AspectRatio = DEFAULT_ASPECT_RATIO;
		FMVAspectRatioSwitchType FMVAspectRatioSwitch = DEFAULT_FMV_ASPECT_RATIO;
		GSInterlaceMode InterlaceMode = DEFAULT_INTERLACE_MODE;
		GSPostBilinearMode LinearPresent = DEFAULT_BILINEAR_FILTERING_MODE;

		float StretchY = 100.0f;
		int Crop[4] = {};

		float OsdScale = DEFAULT_OSD_SCALE;
		float OsdMargin = DEFAULT_OSD_MARGIN;
		std::string OsdFontPath;
		OsdOverlayPos OsdMessagesPos = DEFAULT_OSD_MESSAGE_POS;
		OsdOverlayPos OsdPerformancePos = DEFAULT_OSD_PERFORMANCE_POS;

		GSRendererType Renderer = DEFAULT_HW_RENDERER;
		float UpscaleMultiplier = DEFAULT_UPSCALE_MULTIPLIER;

		AccBlendLevel AccurateBlendingUnit = DEFAULT_BLENDING_ACCURACY;
		BiFiltering TextureFiltering = DEFAULT_TEXTURE_FILTERING_MODE;
		TexturePreloadingLevel TexturePreloading = TexturePreloadingLevel::Full;
		GSDumpCompressionMethod GSDumpCompression = GSDumpCompressionMethod::Zstandard;
		GSHardwareDownloadMode HWDownloadMode = GSHardwareDownloadMode::Enabled;
		GSCASMode CASMode = DEFAULT_CAS_MODE;
		u8 Dithering = 2;
		u8 MaxAnisotropy = 0;
		u8 TVShader = 0;
		s16 GetSkipCountFunctionId = -1;
		s16 BeforeDrawFunctionId = -1;
		s16 MoveHandlerFunctionId = -1;
		int SkipDrawStart = 0;
		int SkipDrawEnd = 0;

		GSHWAutoFlushLevel UserHacks_AutoFlush = GSHWAutoFlushLevel::Disabled;
		GSHalfPixelOffset UserHacks_HalfPixelOffset = GSHalfPixelOffset::Off;
		s8 UserHacks_RoundSprite = 0;
		GSNativeScaling UserHacks_NativeScaling = GSNativeScaling::Off;
		s32 UserHacks_TCOffsetX = 0;
		s32 UserHacks_TCOffsetY = 0;
		u8 UserHacks_CPUSpriteRenderBW = 0;
		u8 UserHacks_CPUSpriteRenderLevel = 0;
		u8 UserHacks_CPUCLUTRender = 0;
		GSGPUTargetCLUTMode UserHacks_GPUTargetCLUTMode = GSGPUTargetCLUTMode::Disabled;
		GSTextureInRtMode UserHacks_TextureInsideRt = GSTextureInRtMode::Disabled;
		GSLimit24BitDepth UserHacks_Limit24BitDepth = GSLimit24BitDepth::Disabled;
		GSBilinearDirtyMode UserHacks_BilinearHack = GSBilinearDirtyMode::Automatic;
		TriFiltering TriFilter = DEFAULT_TRILINEAR_FILTERING_MODE;
		s8 OverrideTextureBarriers = -1;
		GSDepthFeedbackMode DepthFeedbackMode = GSDepthFeedbackMode::Auto;

		u8 CAS_Sharpness = 50;
		u8 ShadeBoost_Brightness = DEFAULT_SHADEBOOST_BRIGHTNESS;
		u8 ShadeBoost_Contrast = DEFAULT_SHADEBOOST_CONTRAST;
		u8 ShadeBoost_Saturation = DEFAULT_SHADEBOOST_SATURATION;
		u8 ShadeBoost_Gamma = DEFAULT_SHADEBOOST_GAMMA;
		u8 PNGCompressionLevel = 1;

		u16 SWExtraThreads = 2;
		u16 SWExtraThreadsHeight = 4;

		int SaveDrawStart = 0;
		int SaveDrawCount = 5000;
		int SaveDrawBy = 1;
		int SaveFrameStart = 0;
		int SaveFrameCount = -1;
		int SaveFrameBy = 1;

		s8 ExclusiveFullscreenControl = -1;
		GSScreenshotSize ScreenshotSize = GSScreenshotSize::WindowResolution;
		GSScreenshotFormat ScreenshotFormat = GSScreenshotFormat::PNG;
		int ScreenshotQuality = 90;

		std::string CaptureContainer = DEFAULT_CAPTURE_CONTAINER;
		std::string VideoCaptureCodec;
		std::string VideoCaptureFormat;
		std::string VideoCaptureParameters;
		std::string AudioCaptureCodec;
		std::string AudioCaptureParameters;
		int VideoCaptureBitrate = DEFAULT_VIDEO_CAPTURE_BITRATE;
		int VideoCaptureWidth = DEFAULT_VIDEO_CAPTURE_WIDTH;
		int VideoCaptureHeight = DEFAULT_VIDEO_CAPTURE_HEIGHT;
		int AudioCaptureBitrate = DEFAULT_AUDIO_CAPTURE_BITRATE;

		std::string Adapter;
		std::string HWDumpDirectory;
		std::string SWDumpDirectory;

		GSOptions();

		void LoadSave(SettingsWrapper& wrap);

		void MaskUserHacks();

		void MaskUpscalingHacks();

		bool UseHardwareRenderer() const;

		bool RestartOptionsAreEqual(const GSOptions& right) const;

		bool OptionsAreEqual(const GSOptions& right) const;

		bool operator==(const GSOptions& right) const;
		bool operator!=(const GSOptions& right) const;

		bool ShouldDump(u64 draw, int frame) const;
	};

	struct SPU2Options
	{
		enum class SPU2SyncMode : u8
		{
			Disabled,
			TimeStretch,
			Count
		};

		static constexpr s32 MAX_VOLUME = 200;
		static constexpr AudioBackend DEFAULT_BACKEND = AudioBackend::Cubeb;
		static constexpr SPU2SyncMode DEFAULT_SYNC_MODE = SPU2SyncMode::TimeStretch;

		static std::optional<SPU2SyncMode> ParseSyncMode(const char* str);
		static const char* GetSyncModeName(SPU2SyncMode backend);
		static const char* GetSyncModeDisplayName(SPU2SyncMode backend);

		BITFIELD32()
		bool
			DebugEnabled : 1,
			MsgToConsole : 1,
			MsgKeyOnOff : 1,
			MsgVoiceOff : 1,
			MsgDMA : 1,
			MsgAutoDMA : 1,
			MsgCache : 1,
			AccessLog : 1,
			DMALog : 1,
			WaveLog : 1,
			CoresDump : 1,
			MemDump : 1,
			RegDump : 1,
			VisualDebugEnabled : 1;
		BITFIELD_END

		u32 StandardVolume = 100;
		u32 FastForwardVolume = 100;
		bool OutputMuted = false;

		AudioBackend Backend = DEFAULT_BACKEND;
		SPU2SyncMode SyncMode = DEFAULT_SYNC_MODE;
		AudioStreamParameters StreamParameters;

		std::string DriverName;
		std::string DeviceName;

		SPU2Options();

		void LoadSave(SettingsWrapper& wrap);

		bool IsTimeStretchEnabled() const { return (SyncMode == SPU2SyncMode::TimeStretch); }

		bool operator==(const SPU2Options& right) const;
		bool operator!=(const SPU2Options& right) const;
	};

	struct DEV9Options
	{
		enum struct NetApi : int
		{
			Unset = 0,
			PCAP_Bridged = 1,
			PCAP_Switched = 2,
			TAP = 3,
			Sockets = 4,
		};
		static const char* NetApiNames[];

		enum struct DnsMode : int
		{
			Manual = 0,
			Auto = 1,
			Internal = 2,
		};
		static const char* DnsModeNames[];

		struct HostEntry
		{
			std::string Url;
			std::string Desc;
			u8 Address[4]{};
			bool Enabled;

			bool operator==(const HostEntry& right) const;
			bool operator!=(const HostEntry& right) const;
		};

		bool EthEnable{false};
		NetApi EthApi{NetApi::Unset};
		std::string EthDevice;
		bool EthLogDHCP{false};
		bool EthLogDNS{false};

		bool InterceptDHCP{false};
		u8 PS2IP[4]{};
		u8 Mask[4]{};
		u8 Gateway[4]{};
		u8 DNS1[4]{};
		u8 DNS2[4]{};
		bool AutoMask{true};
		bool AutoGateway{true};
		DnsMode ModeDNS1{DnsMode::Auto};
		DnsMode ModeDNS2{DnsMode::Auto};

		std::vector<HostEntry> EthHosts;

		bool HddEnable{false};
		std::string HddFile;

		DEV9Options();

		void LoadSave(SettingsWrapper& wrap);

		bool operator==(const DEV9Options& right) const;
		bool operator!=(const DEV9Options& right) const;

	protected:
		static void LoadIPHelper(u8* field, const std::string& setting);
		static std::string SaveIPHelper(u8* field);
	};

	struct GamefixOptions
	{
		BITFIELD32()
		bool
			FpuMulHack : 1,
			GoemonTlbHack : 1,
			SoftwareRendererFMVHack : 1,
			SkipMPEGHack : 1,
			OPHFlagHack : 1,
			EETimingHack : 1,
			InstantDMAHack : 1,
			DMABusyHack : 1,
			GIFFIFOHack : 1,
			VIFFIFOHack : 1,
			VIF1StallHack : 1,
			VuAddSubHack : 1,
			IbitHack : 1,
			VUSyncHack : 1,
			VUOverflowHack : 1,
			XgKickHack : 1,
			BlitInternalFPSHack : 1,
			FullVU0SyncHack : 1;
		BITFIELD_END

		GamefixOptions();
		void LoadSave(SettingsWrapper& wrap);
		GamefixOptions& DisableAll();

		static const char* GetGameFixName(GamefixId id);

		bool Get(GamefixId id) const;
		void Set(GamefixId id, bool enabled = true);
		void Clear(GamefixId id) { Set(id, false); }

		bool operator==(const GamefixOptions& right) const;
		bool operator!=(const GamefixOptions& right) const;
	};

	struct SpeedhackOptions
	{
		static constexpr s8 MIN_EE_CYCLE_RATE = -3;
		static constexpr s8 MAX_EE_CYCLE_RATE = 3;
		static constexpr u8 MAX_EE_CYCLE_SKIP = 3;

		BITFIELD32()
		bool
			fastCDVD : 1,
			IntcStat : 1,
			WaitLoop : 1,
			vuFlagHack : 1,
			vuThread : 1,
			vu1Instant : 1;
		BITFIELD_END

		s8 EECycleRate;
		u8 EECycleSkip;

		SpeedhackOptions();
		void LoadSave(SettingsWrapper& conf);
		SpeedhackOptions& DisableAll();

		void Set(SpeedHack id, int value);

		bool operator==(const SpeedhackOptions& right) const;
		bool operator!=(const SpeedhackOptions& right) const;

		static const char* GetSpeedHackName(SpeedHack id);
		static std::optional<SpeedHack> ParseSpeedHackName(const std::string_view name);
	};

	struct DebugAnalysisOptions
	{

		static const char* RunConditionNames[];
		static const char* FunctionScanModeNames[];

		DebugAnalysisCondition RunCondition = DebugAnalysisCondition::IF_DEBUGGER_IS_OPEN;
		bool GenerateSymbolsForIRXExports = true;

		bool AutomaticallySelectSymbolsToClear = true;
		std::vector<DebugSymbolSource> SymbolSources;

		bool ImportSymbolsFromELF = true;
		bool ImportSymFileFromDefaultLocation = true;
		bool DemangleSymbols = true;
		bool DemangleParameters = true;
		std::vector<DebugExtraSymbolFile> ExtraSymbolFiles;

		DebugFunctionScanMode FunctionScanMode = DebugFunctionScanMode::SCAN_ELF;
		bool CustomFunctionScanRange = false;
		std::string FunctionScanStartAddress;
		std::string FunctionScanEndAddress;

		bool GenerateFunctionHashes = true;

		void LoadSave(SettingsWrapper& wrap);

		friend auto operator<=>(const DebugAnalysisOptions& lhs, const DebugAnalysisOptions& rhs) = default;
	};

	struct EmulationSpeedOptions
	{
		BITFIELD32()
		bool SyncToHostRefreshRate : 1;
		bool UseVSyncForTiming : 1;
		BITFIELD_END

		float NominalScalar{1.0f};
		float TurboScalar{2.0f};
		float SlomoScalar{0.5f};

		EmulationSpeedOptions();

		void LoadSave(SettingsWrapper& wrap);
		void SanityCheck();

		bool operator==(const EmulationSpeedOptions& right) const;
		bool operator!=(const EmulationSpeedOptions& right) const;
	};

	struct FilenameOptions
	{
		std::string Bios;

		FilenameOptions();
		void LoadSave(SettingsWrapper& wrap);

		bool operator==(const FilenameOptions& right) const;
		bool operator!=(const FilenameOptions& right) const;
	};

	struct USBOptions
	{
		static constexpr u32 NUM_PORTS = 2;

		struct Port
		{
			s32 DeviceType;
			u32 DeviceSubtype;

			bool operator==(const USBOptions::Port& right) const;
			bool operator!=(const USBOptions::Port& right) const;
		};

		std::array<Port, NUM_PORTS> Ports;

		USBOptions();
		void LoadSave(SettingsWrapper& wrap);

		bool operator==(const USBOptions& right) const;
		bool operator!=(const USBOptions& right) const;
	};

	struct PadOptions
	{
		static constexpr u32 NUM_PORTS = 8;

		struct Port
		{
			Pad::ControllerType Type;

			bool operator==(const PadOptions::Port& right) const;
			bool operator!=(const PadOptions::Port& right) const;
		};

		std::array<Port, NUM_PORTS> Ports;

		BITFIELD32()
		bool
			MultitapPort0_Enabled : 1,
			MultitapPort1_Enabled;
		BITFIELD_END

		PadOptions();
		void LoadSave(SettingsWrapper& wrap);

		bool IsMultitapPortEnabled(u32 port) const
		{
			return (port == 0) ? MultitapPort0_Enabled : MultitapPort1_Enabled;
		}

		bool operator==(const PadOptions& right) const;
		bool operator!=(const PadOptions& right) const;
	};

	struct McdOptions
	{
		std::string Filename;
		bool Enabled;
		MemoryCardType Type;
	};

	struct AchievementsOptions
	{
		static constexpr u32 MINIMUM_NOTIFICATION_DURATION = 3;
		static constexpr u32 MAXIMUM_NOTIFICATION_DURATION = 30;
		static constexpr u32 DEFAULT_NOTIFICATION_DURATION = 5;
		static constexpr u32 DEFAULT_LEADERBOARD_DURATION = 10;

		static const char* OverlayPositionNames[(size_t)AchievementOverlayPosition::MaxCount + 1];

		BITFIELD32()
		bool
			Enabled : 1,
			HardcoreMode : 1,
			EncoreMode : 1,
			SpectatorMode : 1,
			UnofficialTestMode : 1,
			Notifications : 1,
			LeaderboardNotifications : 1,
			SoundEffects : 1,
			InfoSound : 1,
			UnlockSound : 1,
			LBSubmitSound : 1,
			Overlays : 1,
			LBOverlays : 1;
		BITFIELD_END

		u32 NotificationsDuration = DEFAULT_NOTIFICATION_DURATION;
		u32 LeaderboardsDuration = DEFAULT_LEADERBOARD_DURATION;
		AchievementOverlayPosition OverlayPosition = AchievementOverlayPosition::BottomRight;
		OsdOverlayPos NotificationPosition = OsdOverlayPos::TopLeft;

		std::string InfoSoundName;
		std::string UnlockSoundName;
		std::string LBSubmitSoundName;

		AchievementsOptions();
		void LoadSave(SettingsWrapper& wrap);

		bool operator==(const AchievementsOptions& right) const;
		bool operator!=(const AchievementsOptions& right) const;
	};

	struct SavestateOptions
	{
		SavestateOptions();
		void LoadSave(SettingsWrapper& wrap);

		SavestateCompressionMethod CompressionType = SavestateCompressionMethod::Zstandard;
		SavestateCompressionLevel CompressionRatio = SavestateCompressionLevel::Medium;

		bool operator==(const SavestateOptions& right) const;
		bool operator!=(const SavestateOptions& right) const;
	};

	struct VROptions
	{
		bool Enable = false;

		std::string XrSeatRuntimeDirs;
		std::string XrSeatRuntimeJsons;

		float ScreenDistance = 2.0f;
		float ScreenHeight = 1.4f;

		float ScreenVerticalOffset = 0.0f;

		float ScreenArcDeg = 100.0f;

		bool StereoMode = true;
		bool StereoUseProfile = true;
		float StereoSeparation = 0.02f;
		float StereoConvergence = 20.0f;

		bool HeadCamera = true;
		// First person: turn the view in steps on the turn stick instead of smoothly.
		bool SnapTurn = false;
		// A short fade to black and back over the whole headset view on each snap turn and each change
		// between the first-person and the world screen, hiding the jump. Only what this player sees.
		bool ComfortBlink = true;
		// First person: a thin red beam from the pistol's muzzle along the aim while the weapon is raised.
		// Drawn in the headset only, so only what this player sees.
		bool LaserSight = true;
		// First person: the equipped weapon drawn in the hip holster zone while it is lowered (a long gun slung over the
		// right shoulder), a small dim bag at the belt zone and faint rings on the belt pouches, each brighter while its
		// hand is in reach, so a reach for the hip or a pouch has something to aim at. Drawn in the headset only.
		bool ZoneMarkers = true;
		// First person with a gun raised: reload with the hands. Squeeze the off hand's grip at the left hip (where the
		// belt zone is) to take a magazine, bring it to the gun and let go (or pull that hand's trigger) there; the reload
		// then runs on this player's own pad the way the game's own controls do it (R1 let go so the weapon lowers, the
		// profile's reload button held until the rounds stop). Needs feedback.weapon.reloadButton in the profile.
		bool ReloadGesture = true;
		// First person, while a menu is up: a beam and a dot where the controller points at the menu screen, the hands
		// drawn as empty pointing hands, and (for games whose profile maps the menu) the game's own cursor steered to
		// the cell pointed at with D-pad presses from this player's own pad; the trigger stays the player's confirm,
		// nothing is ever confirmed for them. Online only in private co-op, and only where the profile allows it.
		bool MenuPointer = true;
		// First person, while a menu is up: the menu on a small screen tilted up in front of the chest, like a tablet,
		// instead of the world screen. Shown in the headset only.
		bool MenuTablet = false;
		// First person, File #1 (profiles with camera.lookAt.pauseWhen): a game screen (the item screen, the map, pause, a
		// take-it prompt) no longer hands the camera back. First person keeps driving the camera, so the room keeps
		// rendering around you in stereo, and the screen's own 2D draws are split off the frame onto a flat panel in
		// front of you, where the menu pointer aims. The game reads the pad for its menu as before; nothing is confirmed
		// for you. false = the pull-away to the world screen (or the Menu Tablet) as before. Ini only for now.
		bool MenuPanel = false;
		// With MenuPanel, what the room behind the panel does while a game screen is up
		// (research/2026-10-13-menu-panel.md, "Calm menus"):
		//  Freeze: the camera holds the view it had as the screen came up (the head no longer drives it), so the room stays
		//          put in the world as you look around, like a VR game pausing into its scene; dimmed (below), and black
		//          past its edges (no edge fill while it holds). The head drives the camera again as the screen goes.
		//  Dim:    the camera follows the head as before, the room dimmed (below).
		//  Live:   the camera follows the head, the room as bright as in play (MenuPanel as first played, 2026-10-14).
		enum struct MenuPanelRoomMode : int
		{
			Freeze = 0,
			Dim = 1,
			Live = 2,
		};
		static const char* MenuPanelRoomModeNames[];
		MenuPanelRoomMode MenuPanelRoom = MenuPanelRoomMode::Freeze;
		// Freeze and Dim: how bright the room behind the panel is (1 = as in play), eased in and out with the screen.
		static constexpr float MIN_MENU_PANEL_ROOM_BRIGHTNESS = 0.2f, MAX_MENU_PANEL_ROOM_BRIGHTNESS = 1.0f;
		static constexpr float DEFAULT_MENU_PANEL_ROOM_BRIGHTNESS = 0.55f;
		float MenuPanelRoomBrightness = DEFAULT_MENU_PANEL_ROOM_BRIGHTNESS;
		// First person, File #1 offline only (profiles with camera.silence entries marked whenSetting: SeamlessDoors): a
		// door you open yourself no longer plays the game's door shot or arrival shot, so first person holds while your
		// character opens the door and comes back as soon as the next room is loaded, as when you follow a partner.
		// The door, its locks and keys, the load and partners are unchanged. Never online (not even private co-op).
		// Ini only for now.
		bool SeamlessDoors = false;
		// With SeamlessDoors: the room change around a door you open yourself is shown black (the first-person view darkens
		// as the load starts and clears once the first frame drawn from the head in the next room is on screen), so none of
		// the game's own room-camera frames of the fade out, the load or the fade in show. Display only: the game runs on.
		bool DoorVeil = true;
		// First person, while a zombie holds you (the profile's breakout.grabbed checks: grabbed, not down or dead): the head
		// keeps driving the camera, camera only (no heading, aim or body writes; the stick still turns the view), instead of
		// handing back to the game camera on the flat screen. Shaking to break free and the bite haptics are unchanged.
		bool GrabFirstPerson = true;
		// With GrabFirstPerson: the view eases round toward the zombie holding you (yaw only, at most 20 deg/s, the comfort
		// vignette on while it turns). Off by default: an artificial turn the head did not make.
		bool GrabFacePull = false;
		// First person, File #1 offline only (profiles with a melee block and camera.silence entries marked whenSetting:
		// PhysicalMelee): with a melee weapon equipped, a deliberate swing of the right controller presses the game's own
		// attack (Circle; R1 too if the weapon is down), and while the game's swing is live its hit test is run on the VR
		// weapon, where your hand holds it, instead of on the character's canned swing (a code cave; the game's own damage,
		// stagger and hit reaction follow). A tick when the attack goes live, a firmer buzz when it lands. Never presses
		// anything while a game screen is up or while grabbed. Online only with PhysicalMeleeOnline below. Ini only for now.
		bool PhysicalMelee = false;
		// PhysicalMelee online, in private co-op only (first person armed there, never online-safe; the profile's
		// melee.online; tools/outbreak-vr/research/2026-10-14-melee-coop.md). Off: PhysicalMelee does nothing while the
		// network adapter is on. On: a swing presses the game's own attack (partners see the game's own swing), and the hit
		// follows the VR weapon only while this console runs the room's enemies (it decides their HP and reactions for
		// everyone); otherwise the game's own swing and hit, exactly. Only this player's pad, its block and the caves in free
		// RAM are written, never another character. Ini only, off by default; untested online until a co-op session.
		bool PhysicalMeleeOnline = false;
		// How PhysicalMelee attacks (tools/outbreak-vr/research/2026-10-14-vr-melee.md). Contact (the default): the VR
		// weapon's own hit shape touching an enemy makes the game apply a hit through its own code (no attack action, no
		// button: the character is never locked), stronger the faster the swing; only where the caves may run (offline, or
		// in private co-op while this console runs the room's enemies), else Press. Press: a swing presses the game's own
		// attack as before (the canned swing, its hit on the VR weapon). Ini only.
		enum struct PhysicalMeleeModeType : int
		{
			Contact = 0,
			Press = 1,
		};
		static const char* PhysicalMeleeModeNames[];
		PhysicalMeleeModeType PhysicalMeleeMode = PhysicalMeleeModeType::Contact;
		// A prototype on top of PhysicalMelee's contact mode, File #1 strictly offline (tools/outbreak-vr/research/
		// 2026-10-14-vr-melee.md section 9): a contact hit also pushes the zombie's struck body part the way the swing went
		// (head, belly or legs, and the bones next to them), a turn of 8 to 24 degrees that eases back over 5 to 10 of its
		// updates (0.17-0.33 s), on top of the game's own reaction. Only how this console draws the enemy (its bone
		// matrices, rebuilt every frame); never its position, HP or AI. Ini only, off by default.
		bool MeleeHitJolt = false;
		// File #1 strictly offline (tools/outbreak-vr/research/2026-10-14-vr-melee.md section 11): a shot hits along the VR
		// gun's own ray (the laser: from the gun model's muzzle along its barrel) through the game's own shot code (cave I
		// hands the shot's attack spawn the VR ray instead of the character's gun), for the local player only; the damage,
		// the reactions, ammo, sound and the game's own wider second test are the game's. Ini only.
		bool GunRayAim = true;
		// On top of GunRayAim, File #1 strictly offline (section 13): the local player's shot also takes the game's own muzzle
		// flash, shell and effects from the VR gun (cave O hands FUN_0062CF10 a fake weapon object posed as the VR gun's
		// muzzle); the hands then draw no flash of their own. Ini only.
		bool GunObjectAim = true;
		// With PhysicalMelee in contact mode, strictly offline (section 12): a fast swing of the empty right fist, or of the gun
		// hand as a pistol-whip, strikes through the contact path with the knife's attack at a fist-sized sphere, scaled to
		// a light hit (cave H); no ammo or durability spent. Ini only, off by default.
		bool PhysicalMeleeFists = false;
		// With PhysicalMelee in contact mode (tools/outbreak-vr/research/2026-10-27-melee-power.md): a contact strike's power
		// (the tip's speed, the committed stroke, a second hand on the weapon) sets the game's own damage (0.2-1.35 x a
		// weapon's; fists up to 41 and whips 52 of the knife's 55) and raises the game's own reaction class: a hard hit
		// staggers, the hardest knocks down (cave P on FUN_00578300); the VR hand's picture holds still 2-4 frames at the
		// contact, and the buzz follows the power. Online only where contact strikes apply (the room's master with
		// PhysicalMeleeOnline). Ini only, on by default.
		bool MeleePower = true;
		// First person, File #1 (the native-feel switches, tools/outbreak-vr/research/2026-10-22-native-feel.md). Ini only.
		// ScriptedShotsFirstPerson: the game's scripted camera shots (room camera mode 4, a fixed cut for a while, and 5,
		// the camera shake) keep first person: scripts run as before (their waits see the mode the game sets) and the VR eye
		// stays the camera; the shake moves only the game's eye, not yours (its two stores are skipped, offline).
		// Cutscenes (status 4) keep the world screen.
		bool ScriptedShotsFirstPerson = true;
		// HeadInWall: the VR eye stops short of the game's own walls (a read-only copy of the room's player collision,
		// grid A and the moving faces; nothing is written but the eye first person already writes), and the view
		// darkens past 15 cm into a wall.
		bool HeadInWall = true;
		// SpatialAudio: a code cave puts the game's stereo pan of positioned sounds round the whole head (behind you
		// is centred, beside you fully left or right) instead of across the screen only. Offline only.
		bool SpatialAudio = true;
		// GameRumbleEvents: the game's own rumble player (its patterns for damage, firing, grabs...) drives the VR
		// controllers, read from game memory: firing on the gun hand, the rest on both; our own hurt and shot pulses
		// then sit out (the heartbeat, reload and empty clicks stay). Replaces the pad forwarding (GameRumble) while on.
		bool GameRumbleEvents = true;
		// FadeDim: the game's full-screen fades (the fade quad FUN_001B9DA0 draws from u32 0x308894 on, f32 0x30889C level,
		// u32 0x308898 colour) dim the whole first-person view instead (head-locked, eased; black fades only), and the
		// game's quad is skipped while first person draws (offline). The brightness option's own pass (u8 0x3065C0) is
		// left alone.
		bool FadeDim = true;
		// First person, for profiles with a reach block: a light tick in the right hand and a short toast naming the item
		// (and the button, "Kevin's 45 Auto  (A)") when something on the floor comes within reach of the character's own
		// Circle press. Only reads the game's memory; shown in the headset only. Ini only for now.
		bool PickupCue = true;
		// The belt pouches (chest rig + buckle, BeltPouches): off since Alex's 2026-10-14 headset session ("the little
		// circular quick slots... maybe just stick to the inventory"); the item screen stays the way to grab things.
		bool BeltPouches = false;
		// First person, File #1 offline only (not online, not private co-op): a read-only prototype of reaching for the
		// J's Bar Handgun with the right hand. It maps the hand's position into the game's coordinates, tests it against
		// the Handgun's trigger volume (the one the pickup cue's list walk reads), draws that volume as a wireframe box
		// that brightens as the hand nears, and gives one light tick in the right hand on entering it. It writes nothing
		// to the game and presses nothing; what it measures is in the emulog ("Hand targeting:" lines). Ini only.
		bool HandTargeting = false;
		// First person, File #1 strictly offline (not online, not private co-op): real hand pickups. A hand inside (or a few cm
		// from) a floor item's trigger volume highlights it (the box HandTargeting draws, any floor item, either hand, a
		// light tick on entering); a deliberate grip squeeze there presses Circle once on your own pad with a single-use word
		// that makes the game's own Circle search (the cave at 0x61DB94, the local record only) look at the hand instead of
		// in front of the character, so the item under the hand is the one taken whichever way the character faces. When the
		// game's own "Will you take it?" prompt for that item comes up and the grip is still held, the build confirms it with
		// Circle on a free cell (Alex's approved exception, 2026-10-21: the only confirm the build ever makes). A full
		// inventory (the game's swap), any other screen or a different item: nothing is pressed. Ini only, off by default
		// until checked in the headset (tools/outbreak-vr/research/2026-10-21-hand-pickup.md).
		bool HandPickup = false;
		// First person: the game's own text (speech subtitles, "X Obtained", examine and system lines: File #1's 2D layer,
		// it has no persistent HUD) is drawn at half its size about the view centre, so lines the game puts near the bottom
		// of its screen sit 10-25 deg below the line of sight instead of 30-40: with TrueStereo the flat plane spans half as
		// much of the view; without it the 2D draws move toward the picture's centre. Not fades, not menus. Our pickup cue's
		// toast is taken off once the game's own take-it prompt names the item. Display only, no game state. false = the
		// game's own placement (research/2026-10-22-game-cues.md).
		bool GameText = true;
		// First person: the hands are the CC0 hand model (VR::HandMesh) and the weapon in the right hand, in the hip
		// holster and slung over the shoulder is the game's own model read from the disc (VR::GameModels), at its real
		// size and placed in the hand the way the game attaches it (its weapon table's grip offset and turn), drawn with
		// VR::MeshRaster into the same impostor images. A weapon whose model or grip is not known keeps the placeholder
		// shape (logged once). false = the procedural glove and placeholder shapes as before. Ini only for now.
		bool GameWeaponModels = true;
		// With GameWeaponModels: the hands and weapons are lit from the room on screen (the shown frame's average colour
		// and brightness round where the hands are, its brighter side as the key light, smoothed over about half a
		// second), with no rim glow, so they are never brighter than the room. false = the fixed lit look with the rim.
		// Ini only for now.
		bool WeaponSceneLighting = true;
		// First person: darkens the edges of the headset view while the view turns or walks without the head
		// (smooth stick turns, walking), the more the faster it moves; snap turns keep the blink instead.
		// Drawn in the headset only, so only what this player sees.
		bool ComfortVignette = false;
		// How dark the vignette's edges get at full motion.
		static constexpr float MIN_VIGNETTE_STRENGTH = 0.2f;
		static constexpr float MAX_VIGNETTE_STRENGTH = 1.0f;
		static constexpr float DEFAULT_VIGNETTE_STRENGTH = 0.6f;
		float VignetteStrength = DEFAULT_VIGNETTE_STRENGTH;
		// Turns off the first-person extras that change gameplay (walking while aiming, aim pitch from the
		// controller). Also on whenever the network adapter is enabled, except in private co-op (below),
		// where first person plays in full unless this is set.
		bool OnlineSafe = false;
		// First person online, in co-op on a private server only. Off: first person never arms while the
		// network adapter is on or the game is online. On: it may arm online, but only while the PS2's primary DNS is
		// DEV9's Internal DNS and its hosts entry sends the game's login host to a LAN, Tailscale or loopback address
		// (see CameraDriver::LoginHostIsPrivate and ResolveNetMode).
		bool PrivateCoopFirstPerson = false;
		// VR presence (tools/outbreak-vr/research/2026-10-21-vr-presence.md): in private co-op, two PenguinScreen2 consoles send
		// each other their players' VR head and hand poses (in the game's coordinates, about 30 a second) over a small UDP
		// side channel of their own, never the game's connection, and each draws the partner's VR head and hands beside the
		// partner's game character in the same room. Off by default; nothing is sent or drawn unless first person is armed
		// in private co-op (or the offline loopback test, PCSX2_VR_PRESENCE_LOOPBACK=1). No game memory is written.
		bool VrPresence = false;
		// The other console, host:port (an IPv4 address or a name). Only a private address is accepted (a LAN 10/8,
		// 172.16/12, 192.168/16, Tailscale 100.64/10, or this PC 127/8); anything else, the public internet or obsrv, is
		// refused. Empty: off.
		std::string VrPresencePeer;
		// The UDP port this console listens on (1024-65535); the peer's own VrPresencePort is the port in its address.
		int VrPresencePort = 47800;
		// A shared word both consoles set alike (required: empty keeps VR presence off); packets carrying another word's hash
		// are dropped.
		std::string VrPresenceKey;
		// VR body (tools/outbreak-vr/research/2026-10-21-vr-body.md): a VR player is shown as the game's own character model,
		// its own skeleton posed by the player's real head and hands (the head and neck turned to the VR head, each arm
		// reaching the VR hand with a two-bone IK, the hand turned to the controller; the legs, hips and walking stay the
		// game's animation). The partner's, from VrPresence's poses (so only in private co-op or its offline loopback
		// test). Off by default. Only the bone matrices the game rebuilds every frame for the draw; never a character's
		// game state, never the game's traffic.
		bool VrBody = false;
		// With VrBody: the local character too, offline only, so in first person you see your own character's arms
		// (the rest of the body stays hidden) instead of the hand models.
		bool VrBodySelf = false;
		// First-person near depth, in metres: how near true stereo depth reaches before it flattens onto that
		// plane. 0 = the profile's stereo.firstPerson.nearest, below 0 = off (flatten at the convergence plane
		// as before), above 0 = this value (clamped to 0.45-1.5 m).
		float NearDepthMetres = 0.0f;
		// First person: turn the stereo parallax with head roll so a tilted head sees no vertical disparity.
		bool RollParallax = true;
		// Derive first-person separation and convergence from the headset IPD and the first-person screen, for
		// profiles with stereo.firstPerson.auto; false = the profile's numbers.
		bool FirstPersonAutoStereo = true;
		// How deep the first-person stereo looks: 1.0 = life-size, below flattens toward the screen distance, above
		// deepens near objects while distant ones never pass parallel.
		static constexpr float MIN_FP_STEREO_STRENGTH = 0.5f, MAX_FP_STEREO_STRENGTH = 1.5f;
		float FirstPersonStereoStrength = 1.0f;
		// First person: the game camera's field of view. Auto fits it to the headset on each first-person arm (both eyes'
		// furthest reach plus FirstPersonFovMarginDeg, never below the profile's camera.fov nor above 130 deg, eased in
		// over half a second) and the first-person screen is sized to match, so the game draws the room out to the edge of
		// the view; Profile keeps the profile's camera.fov. Only for profiles whose first-person screen is sized to it.
		// Auto by default since Alex's first headset test (2026-10-12); Profile keeps the old 100 deg view (sharper centre).
		enum struct FirstPersonFovMode : int
		{
			Auto = 0,
			Profile = 1,
		};
		static const char* FirstPersonFovModeNames[];
		FirstPersonFovMode FirstPersonFov = FirstPersonFovMode::Auto;
		// Degrees added to the headset's field of view by FirstPersonFov = Auto (half above and below the view, and as
		// much to each side), so a turn of the head is seen before the edge of the picture.
		static constexpr float MIN_FP_FOV_MARGIN_DEG = 0.0f, MAX_FP_FOV_MARGIN_DEG = 20.0f, DEFAULT_FP_FOV_MARGIN_DEG = 6.0f;
		float FirstPersonFovMarginDeg = DEFAULT_FP_FOV_MARGIN_DEG;
		// First person: around the first-person screen the headset shows a dim, blurred continuation of the picture's edges
		// instead of black (a fast head turn shows more room, not a black border). Needs OpaqueBackground; read when VR
		// starts. On by default since Alex's first headset test (2026-10-12).
		bool EdgeFill = true;

		// Headset brightness: scales the gamma of the presentation lift (1.0 = neutral) on top of the
		// profile's own. Applied only while a VR session runs, so flat play is never touched.
		float Brightness = 1.0f;
		static constexpr float MIN_BRIGHTNESS = 0.8f;
		static constexpr float MAX_BRIGHTNESS = 1.8f;

		// Forward the game's DualShock rumble to the VR controllers, for profiles whose feedback block asks
		// for it, on the pad port of the profile's VR Gamepad.
		bool GameRumble = true;
		// Scales the game-feedback haptics (forwarded rumble, hit, shot and heartbeat cues); 0 turns them off.
		float HapticStrength = 1.0f;
		// Frames go to the headset from the VR pacer thread at the display rate; false = legacy, one frame per GS
		// present.
		bool HeadsetRateSubmit = true;
		// An opaque black projection layer under the screens, so the runtime's environment never shows around them.
		bool OpaqueBackground = true;
		// Hands, laser sight and wrist card are placed every headset display frame on the VR pacer from the controller pose
		// for that frame (their images are still drawn on the GS thread); false = placed once per GS present. Only with
		// HeadsetRateSubmit.
		bool HandsDisplayRate = true;
		// The OpenXR session gets a Vulkan queue of its own in the GS graphics family (when the GPU has one to spare and
		// timeline semaphores), so a runtime that blocks inside xrEndFrame no longer holds up the GS's own submits;
		// false = the session shares the GS queue under the queue lock. Read when the GS device is created.
		bool DedicatedXrQueue = false;
		// The VR pacer copies each frame into the headset's swapchains itself: the GS thread only copies the frame into an
		// image of the compositor's in its own command buffer and draws the hands and HUD cards into staging memory, and
		// never calls into the runtime or takes the queue lock, so a runtime that blocks inside xrEndFrame cannot hold up
		// the emulation at all; false = the GS thread acquires, copies and releases the swapchain images as before. Needs
		// DedicatedXrQueue (in effect, not only asked for) and HeadsetRateSubmit. Read when VR starts.
		bool PacerCopies = false;
		// First person: the shown frame goes to the runtime as a projection layer at the pose it was rendered from, with
		// its depth (XR_KHR_composition_layer_depth: the GS depth target exported beside the colour, in metres through
		// the Z-driven depth fit), so the runtime's own reprojection can use it; false = the flat first-person screen
		// quad as before, with no depth swapchain and no depth copy. Needs the runtime to offer the extension (else it
		// stays off, logged once) and PacerCopies off. Read when VR starts. Render-only: nothing in the game changes.
		bool DepthLayer = false;
		// First person: our own translation reprojection of the shown frame, per eye and per display frame, in the VR pacer
		// (VR/DepthWarp.h): each eye's image is redrawn into its swapchain as seen from where that eye is now, a grid mesh
		// displaced by the frame's exported depth, so a lean moves near things against far ones instead of moving the
		// picture; the quad stays at the render pose (rotation corrected by the runtime as before). The pacer then owns the
		// eye swapchains (the GS thread copies each frame into images of the compositor's own); a frame without depth (not
		// first person, menus, the first frames) goes as it is. Needs PacerCopies off and HeadsetRateSubmit on; takes
		// precedence over DepthLayer. Read when VR starts. Render-only: nothing in the game changes.
		bool DepthWarp = false;
		// First person: true per-eye rendering (tools/outbreak-vr/research/2026-10-21-true-stereo.md). Each layer of the
		// stereo target is drawn with that eye's own view and projection: the GS maps every perspective vertex the game's VU1
		// emitted for its one camera through the eye (VR/EyeProjection.h), recomputing Z and Q, and the compositor sends the
		// two layers as a projection layer with the headset's per-eye poses and fields of view instead of the first-person
		// screen; separation, convergence and the near floor are unused in first person. 2D draws (the HUD, message text)
		// stay flat on a plane 1.5 m ahead. Needs PacerCopies off; DepthWarp is not used with it (DepthLayer adds depth).
		// Read when VR starts. Render-only: nothing in the game changes.
		bool TrueStereo = false;
		// The game's real-time cutscenes (File #1: scenario status 4, the profile's camera.cutscene) in stereo through a
		// world-locked window (VR/CutsceneView.h, tools/outbreak-vr/research/2026-10-22-cutscene-vr.md): the game's own
		// cutscene camera is placed where your head was as the cutscene began, the window is its image plane 1 m ahead at its
		// own field of view (the scene at its real size), and each eye sees the scene through it from where that eye is, per
		// eye as TrueStereo draws first person (leaning looks around a little, softly clamped at 15 cm). The camera moves only
		// inside the window. The game's fades, letterbox and subtitles stay on the window's plane. Needs PacerCopies off.
		// Turned off, it takes effect at once; turned on, only after a VR restart (the compositor takes it on when VR
		// starts). Render-only: the cutscene, its timing, its flags and the speed are untouched.
		bool CutsceneStereo = true;

		VROptions();
		void LoadSave(SettingsWrapper& wrap);

		bool operator==(const VROptions& right) const;
		bool operator!=(const VROptions& right) const;
	};

	BITFIELD32()
	bool
		CdvdVerboseReads : 1,
		CdvdDumpBlocks : 1,
		CdvdPrecache : 1,
		EnablePatches : 1,
		EnableCheats : 1,
		EnablePINE : 1,
		EnableWideScreenPatches : 1,
		EnableNoInterlacingPatches : 1,
		EnableFastBoot : 1,
		EnableFastBootFastForward : 1,
		EnableThreadPinning : 1,
		EnableRecordingTools : 1,
		EnableGameFixes : 1,
		SaveStateOnShutdown : 1,
		EnableDiscordPresence : 1,
		UseSavestateSelector : 1,
		InhibitScreensaver : 1,
		BackupSavestate : 1,
		ManuallySetRealTimeClock : 1,
		UseSystemLocaleFormat : 1,

		HostFs : 1,

		WarnAboutUnsafeSettings : 1;
	BITFIELD_END

	CpuOptions Cpu;
	GSOptions GS;
	SpeedhackOptions Speedhacks;
	GamefixOptions Gamefixes;
	ProfilerOptions Profiler;
	DebugAnalysisOptions DebuggerAnalysis;
	EmulationSpeedOptions EmulationSpeed;
	SavestateOptions Savestate;
	SPU2Options SPU2;
	DEV9Options DEV9;
	USBOptions USB;
	PadOptions Pad;

	TraceLogFilters Trace;

	FilenameOptions BaseFilenames;

	AchievementsOptions Achievements;

	VROptions VR;

	McdOptions Mcd[8];
	std::string GzipIsoIndexTemplate;

	int PINESlot;

	int RtcYear;
	int RtcMonth;
	int RtcDay;
	int RtcHour;
	int RtcMinute;
	int RtcSecond;

	std::string CurrentBlockdump;
	std::string CurrentIRX;
	std::string CurrentGameArgs;
	std::string CustomDataPath;
	AspectRatioType CurrentAspectRatio = AspectRatioType::RAuto4_3_3_2;
	float CurrentCustomAspectRatio = 0.f;
	bool IsPortableMode = false;

	Pcsx2Config();
	void LoadSave(SettingsWrapper& wrap);
	void LoadSaveCore(SettingsWrapper& wrap);
	void LoadSaveMemcards(SettingsWrapper& wrap);

	void ReloadPatchAffectingOptions();

	std::string FullpathToBios() const;
	std::string FullpathToMcd(uint slot) const;

	bool operator==(const Pcsx2Config& right) const = delete;
	bool operator!=(const Pcsx2Config& right) const = delete;

	void CopyRuntimeConfig(Pcsx2Config& cfg);

	static void CopyConfiguration(SettingsInterface* dest_si, SettingsInterface& src_si);

	static void ClearConfiguration(SettingsInterface* dest_si);

	static void ClearInvalidPerGameConfiguration(SettingsInterface* si);
};

extern Pcsx2Config EmuConfig;

namespace EmuFolders
{
	extern std::string AppRoot;
	extern std::string DataRoot;
	extern std::string Settings;
	extern std::string Bios;
	extern std::string Snapshots;
	extern std::string Savestates;
	extern std::string MemoryCards;
	extern std::string Logs;
	extern std::string Cheats;
	extern std::string Patches;
	extern std::string Resources;
	extern std::string UserResources;
	extern std::string Cache;
	extern std::string Covers;
	extern std::string GameSettings;
	extern std::string Textures;
	extern std::string InputProfiles;
	extern std::string Videos;
	extern std::string DebuggerLayouts;
	extern std::string DebuggerSettings;
	extern std::string VRProfiles;

	void SetAppRoot();
	bool SetResourcesDirectory();
	bool SetDataDirectory(Error* error);

	void SetDefaults(SettingsInterface& si);
	void LoadConfig(SettingsInterface& si);
	bool EnsureFoldersExist();

	std::FILE* OpenLogFile(std::string_view name, const char* mode);

	std::string GetOverridableResourcePath(std::string_view name);
}

#ifdef _M_X86
#define REC_VU1 (EmuConfig.Cpu.Recompiler.EnableVU1)
#define THREAD_VU1 (REC_VU1 && EmuConfig.Speedhacks.vuThread)
#else
#define THREAD_VU1 false
#define REC_VU1 false
#endif
#define INSTANT_VU1 (EmuConfig.Speedhacks.vu1Instant)
#define CHECK_EEREC (EmuConfig.Cpu.Recompiler.EnableEE)
#define CHECK_CACHE (EmuConfig.Cpu.Recompiler.EnableEECache)
#define CHECK_IOPREC (EmuConfig.Cpu.Recompiler.EnableIOP)
#define CHECK_FASTMEM (EmuConfig.Cpu.Recompiler.EnableEE && EmuConfig.Cpu.Recompiler.EnableFastmem)
#define CHECK_EXTRAMEM (memGetExtraMemMode())

#define CHECK_VUADDSUBHACK (EmuConfig.Gamefixes.VuAddSubHack)
#define CHECK_FPUMULHACK (EmuConfig.Gamefixes.FpuMulHack)
#define CHECK_XGKICKHACK (EmuConfig.Gamefixes.XgKickHack)
#define CHECK_EETIMINGHACK (EmuConfig.Gamefixes.EETimingHack)
#define CHECK_INSTANTDMAHACK (EmuConfig.Gamefixes.InstantDMAHack)
#define CHECK_SKIPMPEGHACK (EmuConfig.Gamefixes.SkipMPEGHack)
#define CHECK_OPHFLAGHACK (EmuConfig.Gamefixes.OPHFlagHack)
#define CHECK_DMABUSYHACK (EmuConfig.Gamefixes.DMABusyHack)
#define CHECK_VIFFIFOHACK (EmuConfig.Gamefixes.VIFFIFOHack)
#define CHECK_VIF1STALLHACK (EmuConfig.Gamefixes.VIF1StallHack)
#define CHECK_GIFFIFOHACK (EmuConfig.Gamefixes.GIFFIFOHack)
#define CHECK_VUOVERFLOWHACK (EmuConfig.Gamefixes.VUOverflowHack)
#define CHECK_FULLVU0SYNCHACK (EmuConfig.Gamefixes.FullVU0SyncHack)

#define CHECK_VU_OVERFLOW(vunum) (((vunum) == 0) ? EmuConfig.Cpu.Recompiler.vu0Overflow : EmuConfig.Cpu.Recompiler.vu1Overflow)
#define CHECK_VU_EXTRA_OVERFLOW(vunum) (((vunum) == 0) ? EmuConfig.Cpu.Recompiler.vu0ExtraOverflow : EmuConfig.Cpu.Recompiler.vu1ExtraOverflow)
#define CHECK_VU_SIGN_OVERFLOW(vunum) (((vunum) == 0) ? EmuConfig.Cpu.Recompiler.vu0SignOverflow : EmuConfig.Cpu.Recompiler.vu1SignOverflow)
#define CHECK_VU_UNDERFLOW(vunum) (((vunum) == 0) ? EmuConfig.Cpu.Recompiler.vu0Underflow : EmuConfig.Cpu.Recompiler.vu1Underflow)

#define CHECK_FPU_OVERFLOW (EmuConfig.Cpu.Recompiler.fpuOverflow)
#define CHECK_FPU_EXTRA_OVERFLOW (EmuConfig.Cpu.Recompiler.fpuExtraOverflow)
#define CHECK_FPU_EXTRA_FLAGS 1
#define CHECK_FPU_FULL (EmuConfig.Cpu.Recompiler.fpuFullMode)

#define SHIFT_RECOMPILE
#define BRANCH_RECOMPILE

#define ARITHMETICIMM_RECOMPILE
#define ARITHMETIC_RECOMPILE
#define MULTDIV_RECOMPILE
#define JUMP_RECOMPILE
#define LOADSTORE_RECOMPILE
#define MOVE_RECOMPILE
#define MMI_RECOMPILE
#define MMI0_RECOMPILE
#define MMI1_RECOMPILE
#define MMI2_RECOMPILE
#define MMI3_RECOMPILE
#define FPU_RECOMPILE
#define CP0_RECOMPILE
#define CP2_RECOMPILE

#ifndef ARITHMETIC_RECOMPILE
#undef ARITHMETICIMM_RECOMPILE
#endif

#define EE_CONST_PROP 1

#define PSX_EXTRALOGS 0

#undef BITFIELD32
#undef BITFIELD_END
