// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/MenuPanel.h"
#include "VR/StereoState.h"

#include "Config.h"

#ifdef ENABLE_VULKAN
#include "GS/GS.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Common/GSTexture.h"
#include "GS/Renderers/Vulkan/GSDeviceVK.h"
#include "GS/Renderers/Vulkan/GSTextureVK.h"
#include "GS/Renderers/Vulkan/VKBuilders.h"
#include "GS/Renderers/Vulkan/VKShaderCache.h"
#endif

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Image.h"
#include "common/Path.h"

#include "fmt/format.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace VR::MenuPanel
{
	namespace
	{
		std::atomic_bool s_screen_up{false};
		// [VR] MenuPanelRoom = Freeze holding the room (PublishRoom; read by the compositor and the GS).
		std::atomic_bool s_room_frozen{false};

		// What CameraDriver published for the screen, cleared as the GS renderer goes (the VM stopped): with no more vsyncs
		// nothing would clear it, and the compositor would keep a stale "screen up" or "room frozen" (review a5).
		void ClearPublished()
		{
			s_screen_up.store(false, std::memory_order_release);
			s_room_frozen.store(false, std::memory_order_release);
		}

		// The present-time gate (GS thread): a screen up, or one gone fewer than this many vsyncs ago, so frames drawn while
		// it was up and presented after its flag fell still show without it on the first-person screen.
		constexpr u32 kTailVsyncs = 6;
		u32 s_vsyncs_since_up = kTailVsyncs;

		SplitTracker s_tracker;
		// [VR] MenuPanelRoom: this present's Merge read a room snapshot (RoomFor gave one since the vsync began), and the
		// dim's eased amount (GS thread).
		bool s_room_shown = false;
		float s_dim_amount = 0.0f;
		std::chrono::steady_clock::time_point s_dim_last{};
		bool s_dim_logged = false;
		const char* s_no_panel_why = nullptr;
		PanelInfo s_latest;
		u64 s_serial = 0;

		// --- PCSX2_VR_MENUTRACE ---
		u32 TraceVsyncsSetting()
		{
			static const u32 n = []() -> u32 {
				const char* e = std::getenv("PCSX2_VR_MENUTRACE");
				if (!e || !*e)
					return 0;
				const long v = std::strtol(e, nullptr, 10);
				return (v > 0) ? static_cast<u32>(std::min<long>(v, 120)) : 0u;
			}();
			return n;
		}
		u32 s_trace_left = 0;   // vsyncs still to trace
		u64 s_trace_vsync = 0;  // GS vsyncs seen
		bool s_trace_last_up = false;
		u32 s_trace_up_vsyncs = 0;
		// Per vsync while tracing: draws, room (depth-tested) draws, overlay draws.
		u32 s_vs_draws = 0, s_vs_room = 0, s_vs_overlay = 0;

		void StartTrace(const char* why)
		{
			if (TraceVsyncsSetting() == 0 || !StereoState::DumpAllowed())
				return;
			s_trace_left = TraceVsyncsSetting();
			Console.WriteLn("(VR) MTRACE start (%s): %u vsyncs", why, s_trace_left);
		}
	}

	void PublishScreenUp(bool up)
	{
		s_screen_up.store(up, std::memory_order_release);
	}

	bool ScreenUp()
	{
		return s_screen_up.load(std::memory_order_acquire);
	}

	namespace
	{
		std::atomic<const char*> s_refusal{nullptr};
		const char* s_refusal_logged = nullptr;
	}

	void SetCompositorRefusal(const char* why)
	{
		s_refusal.store(why, std::memory_order_release);
	}

	bool Usable()
	{
		if (!EmuConfig.VR.MenuPanel)
			return false;
		const char* const why = s_refusal.load(std::memory_order_acquire);
		if (why != s_refusal_logged)
		{
			s_refusal_logged = why;
			if (why)
				Console.Warning("(VR) Menu panel: game screens pull away as before (%s).", why);
		}
		return why == nullptr;
	}

	bool IsBackdrop(const DrawNote& d)
	{
		return IsOverlay(d) && d.covers && d.blend && !d.z_write;
	}

	bool IsPerspective(const DrawNote& d)
	{
		// Textured: perspective-correct coordinates (STQ) whose Q varies across the draw. Untextured: a depth that varies.
		return d.tme ? (!d.fst && !d.q_const) : !d.z_const;
	}

	bool IsClear(const DrawNote& d)
	{
		return d.z_write && d.covers && !d.z_test;
	}

	bool IsRoom(const DrawNote& d)
	{
		return d.z_test && IsPerspective(d);
	}

	bool IsOverlay(const DrawNote& d)
	{
		// Flat, and not a clear. The depth test does not decide it: the take-it prompt draws its text depth-tested
		// against the screen's own cleared depth.
		return !IsClear(d) && !IsPerspective(d);
	}

	SplitAction SplitTracker::NoteDraw(const DrawNote& d, bool split_on)
	{
		snapshot_fbp = 0;
		snapshot_slot = -1;
		skip = false;
		clock++;
		int idx = -1;
		for (u32 i = 0; i < kSlots; i++)
		{
			if (slot[i].used && slot[i].fbp == d.fbp)
				idx = static_cast<int>(i);
		}
		const bool clear = IsClear(d);
		const bool room = IsRoom(d);
		const bool backdrop = IsBackdrop(d);
		if (idx < 0)
		{
			// Only the room or a clear starts tracking a colour target: overlay-only targets (texture renders, the
			// game's own copies) are never the room.
			if (!room && !clear)
				return SplitAction::None;
			// The free slot, or the one drawn into longest ago.
			u32 pick = 0;
			for (u32 i = 0; i < kSlots; i++)
			{
				if (!slot[i].used)
				{
					pick = i;
					break;
				}
				if (slot[i].age < slot[pick].age)
					pick = i;
			}
			slot[pick] = Slot{};
			slot[pick].used = true;
			slot[pick].fbp = d.fbp;
			idx = static_cast<int>(pick);
		}
		Slot& s = slot[idx];
		s.age = clock;
		// The first draw into a target since it was shown begins its next frame.
		if (s.shown)
		{
			const u32 fbp = s.fbp;
			const u64 age = s.age;
			s = Slot{};
			s.used = true;
			s.fbp = fbp;
			s.age = age;
		}
		if (s.snap)
		{
			// The room again after a snapshot that only a plain overlay asked for: that was 2D in the room (a glow, a
			// subtitle drawn before the rest of the room), not a screen. The split waits for the screen.
			if (room && !s.strong)
			{
				s.snap = false;
				s.overlay_draws = 0;
				s.room_draws = 1;
				return SplitAction::None;
			}
			// Everything else is the screen's, its 3D included (the take-it prompt's turning item).
			s.overlay_draws++;
			s.strong = s.strong || clear || backdrop;
			skip = split_on && backdrop;
			return SplitAction::None;
		}
		if (room)
		{
			s.room_draws++;
			return SplitAction::None;
		}
		// After this frame's room the screen begins at its first draw: best its depth clear or a backdrop (File #1
		// starts every screen with one or the other), else, with a screen known to be up, any flat draw. The clear and the
		// backdrop ask for the snapshot even before the screen's flag reaches the GS (its first frame or two are drawn
		// before CameraDriver publishes it; whether a frame shows split is decided at its present), but only with the
		// screen known to be up is anything left out.
		if (s.room_draws > 0 && (clear || backdrop || (split_on && IsOverlay(d))))
		{
			snapshot_fbp = d.fbp;
			snapshot_slot = idx;
			strong_pending = clear || backdrop;
			skip = split_on && backdrop;
			return SplitAction::Snapshot;
		}
		// A clear with no room before it: the frame's own first draw.
		if (clear)
			s.room_draws = 0;
		return SplitAction::None;
	}

	void SplitTracker::NoteShown(u32 fbp)
	{
		for (u32 i = 0; i < kSlots; i++)
		{
			if (slot[i].used && slot[i].fbp == fbp)
				slot[i].shown = true;
		}
	}

	void SplitTracker::SnapshotTaken(bool ok)
	{
		if (snapshot_slot < 0 || snapshot_slot >= static_cast<int>(kSlots))
			return;
		Slot& s = slot[snapshot_slot];
		if (ok)
		{
			s.snap = true;
			s.strong = strong_pending;
			// The draw that asked for it is the screen's first.
			s.overlay_draws = 1;
		}
		snapshot_slot = -1;
		snapshot_fbp = 0;
		strong_pending = false;
	}

	int SplitTracker::SlotForPresent(u32 fbp) const
	{
		for (u32 i = 0; i < kSlots; i++)
		{
			if (slot[i].used && slot[i].fbp == fbp && slot[i].snap && slot[i].overlay_draws > 0)
				return static_cast<int>(i);
		}
		return -1;
	}

	void SplitTracker::Reset()
	{
		*this = SplitTracker{};
	}

	bool SplitOn()
	{
		return EmuConfig.VR.MenuPanel && (ScreenUp() || s_vsyncs_since_up < kTailVsyncs);
	}

	void NoteShown(u32 fbp)
	{
		s_tracker.NoteShown(fbp);
	}

	bool SkipDraw()
	{
		return s_tracker.skip;
	}

	bool Tracing()
	{
		return s_trace_left > 0;
	}

	void NoPanel(const char* why)
	{
		s_latest.valid = false;
		if (why != s_no_panel_why)
		{
			s_no_panel_why = why;
			if (why)
				Console.WriteLn("(VR) Menu panel: no panel (%s).", why);
		}
	}

	const PanelInfo& Latest()
	{
		return s_latest;
	}

#ifdef ENABLE_VULKAN
	namespace
	{
		GSDeviceVK* VulkanDevice()
		{
			if (!g_gs_device || g_gs_device->GetRenderAPI() != RenderAPI::Vulkan)
				return nullptr;
			return GSDeviceVK::GetInstance();
		}

		// The room snapshots, one per tracker slot, on the device that made them.
		GSTexture* s_room[SplitTracker::kSlots] = {};
		GSDeviceVK* s_room_device = nullptr;
		// a6: a snapshot texture that could not be made is not tried again for kSnapRetryVsyncs (the tracker asks again
		// at the screen's next draw, and a screen has hundreds), so the frames go unsplit meanwhile.
		constexpr u64 kSnapRetryVsyncs = 60;
		u64 s_snap_failed_at = 0;
		bool s_snap_failed = false;

		// The panel images: two, alternating, so a present's pass never writes the image the compositor may still be
		// copying from the present before.
		std::unique_ptr<GSTextureVK> s_panel[2];
		GSDeviceVK* s_panel_device = nullptr;
		u32 s_panel_next = 0;

		// The pass: push descriptors (the GS device requires VK_KHR_push_descriptor), like CAS.
		VkDescriptorSetLayout s_ds_layout = VK_NULL_HANDLE;
		VkPipelineLayout s_pl_layout = VK_NULL_HANDLE;
		VkPipeline s_pipeline = VK_NULL_HANDLE;
		GSDeviceVK* s_pipeline_device = nullptr;
		bool s_pipeline_failed = false;

		struct PushConstants
		{
			s32 src_x, src_y; // the displayed rectangle's origin in the colour target
			s32 width, height; // the panel's size
			float threshold;   // a channel changed by more than this (0..1) is the screen's
			float pad[3];
		};

		// What the screen's draws changed, opaque, the rest clear (premultiplied: rgb 0 where alpha is 0). Layer 0 of the
		// finished frame against layer 0 of its room snapshot: the screen's draws are flat (zero separation), so the left
		// eye's are the screen's.
		constexpr const char* kShader = R"(#version 460 core
#extension GL_EXT_samplerless_texture_functions : require
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(push_constant) uniform PC
{
	ivec2 src_off;
	ivec2 size;
	float threshold;
} pc;
layout(set = 0, binding = 0) uniform texture2D frame_tex;
layout(set = 0, binding = 1) uniform texture2D room_tex;
layout(set = 0, binding = 2, rgba8) uniform writeonly image2D panel_img;
void main()
{
	ivec2 p = ivec2(gl_GlobalInvocationID.xy);
	if (p.x >= pc.size.x || p.y >= pc.size.y)
		return;
	vec4 f = texelFetch(frame_tex, pc.src_off + p, 0);
	vec4 r = texelFetch(room_tex, pc.src_off + p, 0);
	vec3 d = abs(f.rgb - r.rgb);
	bool changed = max(d.r, max(d.g, d.b)) > pc.threshold;
	imageStore(panel_img, p, changed ? vec4(f.rgb, 1.0) : vec4(0.0));
}
)";

		void DestroyPipeline()
		{
			if (s_pipeline_device && s_pipeline_device == VulkanDevice())
			{
				const VkDevice dev = s_pipeline_device->GetDevice();
				if (s_pipeline != VK_NULL_HANDLE)
					vkDestroyPipeline(dev, s_pipeline, nullptr);
				if (s_pl_layout != VK_NULL_HANDLE)
					vkDestroyPipelineLayout(dev, s_pl_layout, nullptr);
				if (s_ds_layout != VK_NULL_HANDLE)
					vkDestroyDescriptorSetLayout(dev, s_ds_layout, nullptr);
			}
			s_pipeline = VK_NULL_HANDLE;
			s_pl_layout = VK_NULL_HANDLE;
			s_ds_layout = VK_NULL_HANDLE;
			s_pipeline_device = nullptr;
		}

		bool EnsurePipeline(GSDeviceVK* dev)
		{
			if (s_pipeline != VK_NULL_HANDLE && s_pipeline_device == dev)
				return true;
			if (s_pipeline_failed)
				return false;
			if (s_pipeline_device && s_pipeline_device != dev)
			{
				// A restarted renderer: the old objects died with their device.
				s_pipeline = VK_NULL_HANDLE;
				s_pl_layout = VK_NULL_HANDLE;
				s_ds_layout = VK_NULL_HANDLE;
				s_pipeline_device = nullptr;
			}
			const VkDevice vkdev = dev->GetDevice();
			Vulkan::DescriptorSetLayoutBuilder dslb;
			dslb.SetPushFlag();
			dslb.AddBinding(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT);
			dslb.AddBinding(1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT);
			dslb.AddBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT);
			s_ds_layout = dslb.Create(vkdev);
			Vulkan::PipelineLayoutBuilder plb;
			if (s_ds_layout != VK_NULL_HANDLE)
			{
				plb.AddPushConstants(VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants));
				plb.AddDescriptorSet(s_ds_layout);
				s_pl_layout = plb.Create(vkdev);
			}
			VkShaderModule mod = (s_pl_layout != VK_NULL_HANDLE) ? g_vulkan_shader_cache->GetComputeShader(kShader) : VK_NULL_HANDLE;
			if (mod != VK_NULL_HANDLE)
			{
				Vulkan::ComputePipelineBuilder cpb;
				cpb.SetPipelineLayout(s_pl_layout);
				cpb.SetShader(mod, "main");
				s_pipeline = cpb.Create(vkdev, g_vulkan_shader_cache->GetPipelineCache(true), false);
				vkDestroyShaderModule(vkdev, mod, nullptr);
			}
			s_pipeline_device = dev;
			if (s_pipeline == VK_NULL_HANDLE)
			{
				s_pipeline_failed = true;
				Console.Error("(VR) Menu panel: the split pass could not be built; game screens show over the room.");
				DestroyPipeline();
				return false;
			}
			Console.WriteLn("(VR) Menu panel: split pass ready.");
			return true;
		}

		void ForgetRooms(bool recycle)
		{
			for (GSTexture*& t : s_room)
			{
				if (t && recycle && s_room_device == VulkanDevice())
					g_gs_device->Recycle(t);
				t = nullptr;
			}
			s_room_device = nullptr;
		}

		GSTextureVK* EnsurePanel(GSDeviceVK* dev, u32 w, u32 h)
		{
			if (s_panel_device != dev)
			{
				// Another device: the old images died with it.
				for (auto& p : s_panel)
				{
					if (p)
						(void)p.release();
				}
				s_panel_device = dev;
			}
			std::unique_ptr<GSTextureVK>& p = s_panel[s_panel_next];
			if (p && (static_cast<u32>(p->GetWidth()) != w || static_cast<u32>(p->GetHeight()) != h))
				p.reset(); // destroys deferred
			if (!p)
			{
				p = GSTextureVK::Create(GSTexture::ShaderWriteTexture, GSTexture::Format::Color, static_cast<int>(w),
					static_cast<int>(h), 1, 1);
				if (!p)
					return nullptr;
			}
			GSTextureVK* const out = p.get();
			s_panel_next = (s_panel_next + 1) % 2;
			return out;
		}

		// --- PCSX2_VR_MENUPANEL_DUMP=<dir>[,everyN]: the panel, the room, the finished frame and the picture as shown (the
		// room after the presentation lift and the [VR] MenuPanelRoom dim) (layer 0) as PNGs ---
		struct DumpConfig
		{
			bool enabled = false;
			std::string dir;
			u32 every_n = 30;
		};
		const DumpConfig& GetDumpConfig()
		{
			static const DumpConfig cfg = []() {
				DumpConfig c;
				const char* env = std::getenv("PCSX2_VR_MENUPANEL_DUMP");
				if (!env || !*env)
					return c;
				std::string value(env);
				const size_t comma = value.rfind(',');
				if (comma != std::string::npos)
				{
					const long n = std::strtol(value.c_str() + comma + 1, nullptr, 10);
					if (n > 0)
					{
						c.every_n = static_cast<u32>(n);
						value.resize(comma);
					}
				}
				c.dir = value;
				c.enabled = !value.empty();
				if (c.enabled)
					Console.WriteLn("(VR) Menu panel dump armed: %s, every %u panels (offline only).", c.dir.c_str(), c.every_n);
				return c;
			}();
			return cfg;
		}
		VkBuffer s_dump_buffer = VK_NULL_HANDLE;
		VmaAllocation s_dump_alloc = VK_NULL_HANDLE;
		void* s_dump_map = nullptr;
		u32 s_dump_size = 0;
		GSDeviceVK* s_dump_device = nullptr;
		bool s_dump_pending = false;
		u64 s_dump_fence = 0;
		u64 s_dump_serial = 0;
		u32 s_dump_w = 0, s_dump_h = 0;
		bool s_dump_has_shown = false;
		u64 s_dump_count = 0;
		std::atomic<int> s_dump_writes{0};

		bool EnsureDumpBuffer(GSDeviceVK* dev, u32 size)
		{
			if (s_dump_buffer != VK_NULL_HANDLE && s_dump_device != dev)
			{
				s_dump_buffer = VK_NULL_HANDLE;
				s_dump_alloc = VK_NULL_HANDLE;
				s_dump_map = nullptr;
				s_dump_size = 0;
				s_dump_pending = false;
			}
			if (s_dump_buffer != VK_NULL_HANDLE && s_dump_size == size)
				return true;
			if (s_dump_buffer != VK_NULL_HANDLE)
			{
				dev->DeferBufferDestruction(s_dump_buffer, s_dump_alloc);
				s_dump_buffer = VK_NULL_HANDLE;
				s_dump_alloc = VK_NULL_HANDLE;
				s_dump_map = nullptr;
				s_dump_size = 0;
			}
			const VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0u, size,
				VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0u, nullptr};
			VmaAllocationCreateInfo aci = {};
			aci.usage = VMA_MEMORY_USAGE_GPU_TO_CPU;
			aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
			aci.preferredFlags = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
			VmaAllocationInfo ai = {};
			if (vmaCreateBuffer(dev->GetAllocator(), &bci, &aci, &s_dump_buffer, &s_dump_alloc, &ai) != VK_SUCCESS ||
				!ai.pMappedData)
			{
				s_dump_buffer = VK_NULL_HANDLE;
				s_dump_alloc = VK_NULL_HANDLE;
				return false;
			}
			s_dump_map = ai.pMappedData;
			s_dump_size = size;
			s_dump_device = dev;
			return true;
		}

		void WriteDump(std::string dir, u64 serial, u32 w, u32 h, std::vector<u32> panel, std::vector<u32> room,
			std::vector<u32> frame, std::vector<u32> shown, bool has_shown)
		{
			// [VR] MenuPanelRoom: the room as shown against its snapshot, over the pixels the panel leaves to the room (and
			// bright enough to measure): the dim as it reached the picture.
			double room_sum = 0.0, shown_sum = 0.0;
			u32 room_px = 0;
			if (has_shown)
			{
				const auto lum = [](u32 c) {
					return 0.2125 * (c & 0xFF) + 0.7154 * ((c >> 8) & 0xFF) + 0.0721 * ((c >> 16) & 0xFF);
				};
				for (size_t i = 0; i < room.size(); i++)
				{
					if ((panel[i] >> 24) != 0)
						continue;
					const double r = lum(room[i]);
					if (r < 24.0)
						continue;
					room_sum += r;
					shown_sum += lum(shown[i]);
					room_px++;
				}
			}
			// The panel's coverage, for the log: how many pixels the screen's draws changed, and where.
			u32 n = 0;
			u32 x0 = w, y0 = h, x1 = 0, y1 = 0;
			for (u32 y = 0; y < h; y++)
			{
				for (u32 x = 0; x < w; x++)
				{
					if ((panel[static_cast<size_t>(y) * w + x] >> 24) != 0)
					{
						n++;
						x0 = std::min(x0, x);
						y0 = std::min(y0, y);
						x1 = std::max(x1, x);
						y1 = std::max(y1, y);
					}
				}
			}
			for (u32& p : room)
				p |= 0xFF000000u;
			for (u32& p : frame)
				p |= 0xFF000000u;
			for (u32& p : shown)
				p |= 0xFF000000u;
			FileSystem::EnsureDirectoryExists(dir.c_str(), true);
			const std::string base = Path::Combine(dir, fmt::format("menupanel_{:06d}", serial));
			const RGBA8Image pi(w, h, std::move(panel));
			const RGBA8Image ri(w, h, std::move(room));
			const RGBA8Image fi(w, h, std::move(frame));
			bool ok = pi.SaveToFile((base + "_panel.png").c_str()) && ri.SaveToFile((base + "_room.png").c_str()) &&
			          fi.SaveToFile((base + "_frame.png").c_str());
			if (has_shown)
			{
				const RGBA8Image si(w, h, std::move(shown));
				ok = si.SaveToFile((base + "_shown.png").c_str()) && ok;
				if (room_px > 0)
					Console.WriteLn("(VR) Menu panel dump: %s: the room as shown is %.1f%% of its snapshot's brightness (%u px of "
									"the room off the panel).",
						base.c_str(), 100.0 * shown_sum / room_sum, room_px);
			}
			if (n > 0)
				Console.WriteLn("(VR) Menu panel dump: %s (%ux%u): %u px are the screen's (%.1f%%), within %u,%u-%u,%u%s",
					base.c_str(), w, h, n, 100.0 * n / (static_cast<double>(w) * h), x0, y0, x1 + 1, y1 + 1, ok ? "" : " (write failed)");
			else
				Console.WriteLn("(VR) Menu panel dump: %s (%ux%u): no pixel is the screen's%s", base.c_str(), w, h,
					ok ? "" : " (write failed)");
			s_dump_writes.fetch_sub(1, std::memory_order_acq_rel);
		}

		void PollDump(GSDeviceVK* dev)
		{
			if (!s_dump_pending || !dev || s_dump_device != dev || dev->GetCompletedFenceCounter() < s_dump_fence)
				return;
			s_dump_pending = false;
			const size_t px = static_cast<size_t>(s_dump_w) * s_dump_h;
			if (px * 16 > s_dump_size)
				return;
			vmaInvalidateAllocation(dev->GetAllocator(), s_dump_alloc, 0, static_cast<VkDeviceSize>(px * 16));
			std::vector<u32> panel(px), room(px), frame(px), shown(px);
			std::memcpy(panel.data(), s_dump_map, px * 4);
			std::memcpy(room.data(), static_cast<const u8*>(s_dump_map) + px * 4, px * 4);
			std::memcpy(frame.data(), static_cast<const u8*>(s_dump_map) + px * 8, px * 4);
			if (s_dump_has_shown)
				std::memcpy(shown.data(), static_cast<const u8*>(s_dump_map) + px * 12, px * 4);
			s_dump_writes.fetch_add(1, std::memory_order_acq_rel);
			std::thread(WriteDump, GetDumpConfig().dir, s_dump_serial, s_dump_w, s_dump_h, std::move(panel), std::move(room),
				std::move(frame), std::move(shown), s_dump_has_shown)
				.detach();
		}

		void MaybeDump(GSDeviceVK* dev, VkCommandBuffer cmd, GSTextureVK* panel, GSTextureVK* room, GSTextureVK* frame,
			u32 sx, u32 sy, u32 w, u32 h, GSTextureVK* shown, u32 shown_x, u32 shown_y)
		{
			const DumpConfig& cfg = GetDumpConfig();
			if (!cfg.enabled || !StereoState::DumpAllowed())
				return;
			const u64 n = s_dump_count++;
			if ((n % cfg.every_n) != 0 || s_dump_pending || s_dump_writes.load(std::memory_order_acquire) >= 2)
				return;
			const u64 plane = static_cast<u64>(w) * h * 4u;
			if (plane * 4 > 0x7FFFFFFFull || !EnsureDumpBuffer(dev, static_cast<u32>(plane * 4)))
				return;
			if (room->GetVkFormat() != VK_FORMAT_R8G8B8A8_UNORM || frame->GetVkFormat() != VK_FORMAT_R8G8B8A8_UNORM)
				return;
			panel->TransitionToLayout(cmd, GSTextureVK::Layout::TransferSrc);
			room->TransitionToLayout(cmd, GSTextureVK::Layout::TransferSrc);
			frame->TransitionToLayout(cmd, GSTextureVK::Layout::TransferSrc);
			VkBufferImageCopy r = {};
			r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			r.imageExtent = {w, h, 1};
			vkCmdCopyImageToBuffer(cmd, panel->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_dump_buffer, 1, &r);
			r.bufferOffset = plane;
			r.imageOffset = {static_cast<s32>(sx), static_cast<s32>(sy), 0};
			r.imageSubresource.baseArrayLayer = room->GetBaseArrayLayer();
			vkCmdCopyImageToBuffer(cmd, room->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_dump_buffer, 1, &r);
			r.bufferOffset = plane * 2;
			r.imageSubresource.baseArrayLayer = frame->GetBaseArrayLayer();
			vkCmdCopyImageToBuffer(cmd, frame->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_dump_buffer, 1, &r);
			s_dump_has_shown = shown && shown->GetVkFormat() == VK_FORMAT_R8G8B8A8_UNORM &&
			                   shown_x + w <= static_cast<u32>(shown->GetWidth()) &&
			                   shown_y + h <= static_cast<u32>(shown->GetHeight());
			if (s_dump_has_shown)
			{
				if (shown->GetState() == GSTexture::State::Cleared)
					shown->CommitClear();
				shown->TransitionToLayout(cmd, GSTextureVK::Layout::TransferSrc);
				shown->SetUseFenceCounter(dev->GetCurrentFenceCounter());
				r.bufferOffset = plane * 3;
				r.imageOffset = {static_cast<s32>(shown_x), static_cast<s32>(shown_y), 0};
				r.imageSubresource.baseArrayLayer = shown->GetBaseArrayLayer();
				vkCmdCopyImageToBuffer(cmd, shown->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_dump_buffer, 1, &r);
			}
			VkBufferMemoryBarrier bb = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
			bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			bb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
			bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			bb.buffer = s_dump_buffer;
			bb.size = VK_WHOLE_SIZE;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &bb, 0, nullptr);
			s_dump_pending = true;
			s_dump_fence = dev->GetCurrentFenceCounter();
			s_dump_serial = s_serial;
			s_dump_w = w;
			s_dump_h = h;
		}
	}

	SplitAction NoteDraw(const DrawNote& d, u64 draw_n, const char* trace_extra)
	{
		const SplitAction a = VulkanDevice() ? s_tracker.NoteDraw(d, SplitOn()) : SplitAction::None;
		if (s_trace_left > 0)
		{
			s_vs_draws++;
			if (IsRoom(d))
				s_vs_room++;
			if (IsOverlay(d))
				s_vs_overlay++;
			Console.WriteLn("(VR) MTRACE v=%llu n=%llu %s fbp=0x%x zbp=0x%x zt=%d zw=%d tme=%d fst=%d qc=%d zc=%d spr=%d cov=%d ov=%d%s %s",
				static_cast<unsigned long long>(s_trace_vsync), static_cast<unsigned long long>(draw_n),
				(a == SplitAction::Snapshot) ? (s_tracker.skip ? "SNAP+SKIP" : "SNAP") : (s_tracker.skip ? "SKIP" : "-"), d.fbp, d.zbp, d.z_test, d.z_write, d.tme, d.fst, d.q_const, d.z_const,
				d.sprite, d.covers, IsOverlay(d) ? 1 : 0, (d.fbp == d.zbp) ? " zself" : "", trace_extra ? trace_extra : "");
		}
		return a;
	}

	int TakeSnapshot(GSTexture* colour)
	{
		GSDeviceVK* const dev = VulkanDevice();
		const int slot = s_tracker.snapshot_slot;
		if (!dev || !colour || slot < 0 || slot >= static_cast<int>(SplitTracker::kSlots) || colour->IsDepthStencil())
		{
			s_tracker.SnapshotTaken(false);
			return -1;
		}
		if (s_room_device != dev)
			ForgetRooms(false);
		s_room_device = dev;
		GSTexture*& snap = s_room[slot];
		if (snap && (snap->GetSize() != colour->GetSize() || snap->GetArrayLayers() != colour->GetArrayLayers() ||
						snap->GetFormat() != colour->GetFormat()))
		{
			g_gs_device->Recycle(snap);
			snap = nullptr;
		}
		if (!snap)
		{
			if (s_snap_failed && s_trace_vsync - s_snap_failed_at < kSnapRetryVsyncs)
			{
				s_tracker.SnapshotTaken(false);
				return -1;
			}
			snap = g_gs_device->CreateRenderTarget(colour->GetWidth(), colour->GetHeight(), colour->GetFormat(), false, false,
				colour->GetArrayLayers());
			if (!snap)
			{
				if (!s_snap_failed)
					Console.Warning("(VR) Menu panel: the room snapshot (%dx%d) could not be made; tried again in 1 s.",
						colour->GetWidth(), colour->GetHeight());
				s_snap_failed = true;
				s_snap_failed_at = s_trace_vsync;
				s_tracker.SnapshotTaken(false);
				return -1;
			}
			s_snap_failed = false;
		}
		// The GS's own copy, recorded in draw order into the GS command buffer: the room as drawn, before the screen's
		// first draw.
		g_gs_device->CopyRect(colour, snap, GSVector4i(0, 0, colour->GetWidth(), colour->GetHeight()), 0, 0);
		s_tracker.SnapshotTaken(true);
		return slot;
	}

	int ShownRoomSlot(u32 fbp)
	{
		if (!EmuConfig.VR.MenuPanel || !s_room_shown || !SplitOn() || s_room_device != VulkanDevice())
			return -1;
		const int slot = s_tracker.SlotForPresent(fbp);
		return (slot >= 0 && s_room[slot]) ? slot : -1;
	}

	GSTexture* RoomFor(u32 fbp, GSTexture* colour)
	{
		if (!EmuConfig.VR.MenuPanel || !colour || !SplitOn() || s_room_device != VulkanDevice())
			return nullptr;
		const int slot = s_tracker.SlotForPresent(fbp);
		if (slot < 0)
			return nullptr;
		GSTexture* const snap = s_room[slot];
		if (!snap || snap->GetSize() != colour->GetSize() || snap->GetArrayLayers() != colour->GetArrayLayers() ||
			snap->GetFormat() != colour->GetFormat())
			return nullptr;
		s_room_shown = true;
		return snap;
	}

	bool ExportPanel(u32 fbp, GSTexture* colour, u32 src_x, u32 src_y, u32 w, u32 h, float u0, float v0, float u1, float v1,
		GSTexture* shown, u32 shown_x, u32 shown_y)
	{
		s_latest.valid = false;
		GSDeviceVK* const dev = VulkanDevice();
		if (!dev)
		{
			NoPanel("the menu panel is Vulkan only");
			return false;
		}
		GSTexture* const room_tex = RoomFor(fbp, colour);
		if (!room_tex)
		{
			NoPanel("no split of the shown frame (no screen draws after its room)");
			return false;
		}
		if (w == 0 || h == 0 || src_x + w > static_cast<u32>(colour->GetWidth()) || src_y + h > static_cast<u32>(colour->GetHeight()))
		{
			NoPanel("the shown frame does not map onto its colour target");
			return false;
		}
		if (!EnsurePipeline(dev))
		{
			NoPanel("no split pass");
			return false;
		}
		GSTextureVK* const panel = EnsurePanel(dev, w, h);
		if (!panel)
		{
			NoPanel("no panel image");
			return false;
		}
		GSTextureVK* const frame = static_cast<GSTextureVK*>(colour);
		GSTextureVK* const room = static_cast<GSTextureVK*>(room_tex);
		if (dev->InRenderPass())
			dev->EndRenderPass();
		if (frame->GetState() == GSTexture::State::Cleared)
			frame->CommitClear();
		if (room->GetState() == GSTexture::State::Cleared)
			room->CommitClear();
		const VkCommandBuffer cmd = dev->GetCurrentCommandBuffer();
		frame->TransitionToLayout(cmd, GSTextureVK::Layout::ShaderReadOnly);
		room->TransitionToLayout(cmd, GSTextureVK::Layout::ShaderReadOnly);
		panel->TransitionToLayout(cmd, GSTextureVK::Layout::ComputeReadWriteImage);
		frame->SetUseFenceCounter(dev->GetCurrentFenceCounter());
		room->SetUseFenceCounter(dev->GetCurrentFenceCounter());
		panel->SetUseFenceCounter(dev->GetCurrentFenceCounter());

		Vulkan::DescriptorSetUpdateBuilder dsub;
		dsub.AddImageDescriptorWrite(VK_NULL_HANDLE, 0, frame->GetLayerView(0), frame->GetVkLayout());
		dsub.AddImageDescriptorWrite(VK_NULL_HANDLE, 1, room->GetLayerView(0), room->GetVkLayout());
		dsub.AddStorageImageDescriptorWrite(VK_NULL_HANDLE, 2, panel->GetView(), panel->GetVkLayout());
		dsub.PushUpdate(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_pl_layout, 0, false);
		PushConstants pc = {};
		pc.src_x = static_cast<s32>(src_x);
		pc.src_y = static_cast<s32>(src_y);
		pc.width = static_cast<s32>(w);
		pc.height = static_cast<s32>(h);
		pc.threshold = 2.5f / 255.0f;
		vkCmdPushConstants(cmd, s_pl_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_pipeline);
		vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);

		s_serial++;
		MaybeDump(dev, cmd, panel, room, frame, src_x, src_y, w, h, static_cast<GSTextureVK*>(shown), shown_x, shown_y);
		// The compositor copies it out in its own submit after the present's: left as a transfer source.
		panel->TransitionToLayout(cmd, GSTextureVK::Layout::TransferSrc);

		s_latest.valid = true;
		s_latest.image = reinterpret_cast<void*>(panel->GetImage());
		s_latest.width = w;
		s_latest.height = h;
		s_latest.serial = s_serial;
		s_latest.u0 = u0;
		s_latest.v0 = v0;
		s_latest.u1 = u1;
		s_latest.v1 = v1;
		if (s_no_panel_why != nullptr || s_serial == 1)
		{
			s_no_panel_why = nullptr;
			Console.WriteLn("(VR) Menu panel: the screen's draws split off the shown frame (%ux%u, target 0x%x).", w, h, fbp);
		}
		return true;
	}

	void OnVSync()
	{
		GSDeviceVK* const dev = VulkanDevice();
		PollDump(dev);
		s_trace_vsync++;
		s_room_shown = false;
		const bool up = ScreenUp();
		s_vsyncs_since_up = up ? 0 : std::min(s_vsyncs_since_up + 1, kTailVsyncs);
		if (TraceVsyncsSetting() > 0)
		{
			if (s_trace_left > 0)
			{
				Console.WriteLn("(VR) MTRACE vsync %llu: %u draws, %u depth-tested, %u overlay; screen %s",
					static_cast<unsigned long long>(s_trace_vsync), s_vs_draws, s_vs_room, s_vs_overlay, up ? "up" : "not up");
				s_trace_left--;
			}
			s_vs_draws = s_vs_room = s_vs_overlay = 0;
			if (up != s_trace_last_up)
			{
				s_trace_last_up = up;
				s_trace_up_vsyncs = 0;
				StartTrace(up ? "screen up" : "screen gone");
			}
			else if (up && ++s_trace_up_vsyncs == 90)
			{
				StartTrace("screen up 1.5 s");
			}
		}
		if (!EmuConfig.VR.MenuPanel)
		{
			s_latest.valid = false;
			if (s_tracker.slot[0].used || s_tracker.slot[1].used)
				s_tracker.Reset();
		}
	}

	void Destroy()
	{
		ClearPublished();
		ForgetRooms(true);
		for (auto& p : s_panel)
			p.reset();
		s_panel_device = nullptr;
		DestroyPipeline();
		s_pipeline_failed = false;
		s_tracker.Reset();
		s_latest = PanelInfo{};
		if (s_dump_buffer != VK_NULL_HANDLE && s_dump_device == VulkanDevice() && s_dump_device)
			s_dump_device->DeferBufferDestruction(s_dump_buffer, s_dump_alloc);
		s_dump_buffer = VK_NULL_HANDLE;
		s_dump_alloc = VK_NULL_HANDLE;
		s_dump_map = nullptr;
		s_dump_size = 0;
		s_dump_device = nullptr;
		s_dump_pending = false;
	}
#else
	SplitAction NoteDraw(const DrawNote& d, u64, const char*)
	{
		(void)d;
		return SplitAction::None;
	}
	int TakeSnapshot(GSTexture*)
	{
		s_tracker.SnapshotTaken(false);
		return -1;
	}
	GSTexture* RoomFor(u32, GSTexture*) { return nullptr; }
	int ShownRoomSlot(u32) { return -1; }
	bool ExportPanel(u32, GSTexture*, u32, u32, u32, u32, float, float, float, float, GSTexture*, u32, u32)
	{
		NoPanel("the menu panel is Vulkan only");
		return false;
	}
	void OnVSync() { s_room_shown = false; }
	void Destroy() { ClearPublished(); }
#endif

	// --- Calm menus ------------------------------------------------------------------------------------------------------
	namespace
	{
		std::atomic<u8> s_room_mode{static_cast<u8>(RoomMode::Live)};
		std::atomic<float> s_room_brightness{1.0f};

		constexpr float kPi = 3.14159265358979f;

		float WrapAngle(float a)
		{
			if (!std::isfinite(a))
				return 0.0f;
			a = std::fmod(a + kPi, 2.0f * kPi);
			if (a < 0.0f)
				a += 2.0f * kPi;
			return a - kPi;
		}
	}

	void PublishRoom(RoomMode mode, float brightness, bool frozen)
	{
		s_room_mode.store(static_cast<u8>(mode), std::memory_order_relaxed);
		s_room_brightness.store(brightness, std::memory_order_relaxed);
		s_room_frozen.store(frozen, std::memory_order_release);
	}

	bool RoomFrozen()
	{
		return s_room_frozen.load(std::memory_order_acquire);
	}

	RoomLook RoomLookFor(RoomMode mode, float brightness)
	{
		RoomLook l;
		if (mode == RoomMode::Live)
			return l;
		l.brightness = std::isfinite(brightness) ? std::clamp(brightness, 0.0f, 1.0f) : 1.0f;
		// No dim asked for (brightness 1): no greying either.
		l.saturation = (l.brightness < 1.0f) ? kRoomSaturation : 1.0f;
		return l;
	}

	float EaseRoomDim(float amount, float target, float dt)
	{
		if (!std::isfinite(amount))
			amount = 0.0f;
		amount = std::clamp(amount, 0.0f, 1.0f);
		if (!std::isfinite(target))
			target = 0.0f;
		target = std::clamp(target, 0.0f, 1.0f);
		if (!std::isfinite(dt) || dt <= 0.0f)
			return amount;
		if (target > amount)
			return std::min(target, amount + dt / kRoomDimInS);
		return std::max(target, amount - dt / kRoomDimOutS);
	}

	RoomLook RoomLookAt(const RoomLook& full, float amount)
	{
		const float t = std::isfinite(amount) ? std::clamp(amount, 0.0f, 1.0f) : 0.0f;
		RoomLook l;
		l.brightness = 1.0f + (full.brightness - 1.0f) * t;
		l.saturation = 1.0f + (full.saturation - 1.0f) * t;
		return l;
	}

	float DimBeforeGamma(float dim, float gamma)
	{
		if (!std::isfinite(dim) || dim >= 1.0f)
			return 1.0f;
		if (!std::isfinite(gamma) || gamma <= 0.0f)
			gamma = 1.0f;
		return std::pow(std::max(dim, 0.0f), gamma);
	}

	bool RoomLookNow(RoomLook* out)
	{
		const auto now = std::chrono::steady_clock::now();
		float dt = 0.0f;
		if (s_dim_last.time_since_epoch().count() != 0)
			dt = std::chrono::duration<float>(now - s_dim_last).count();
		s_dim_last = now;
		// A long gap (the GS idle, presents paused) counts as at most a few frames, so the dim never pops.
		dt = std::clamp(dt, 0.0f, 0.05f);
		const RoomMode mode = static_cast<RoomMode>(s_room_mode.load(std::memory_order_relaxed));
		const RoomLook full = RoomLookFor(mode, s_room_brightness.load(std::memory_order_relaxed));
		const bool want = EmuConfig.VR.MenuPanel && s_room_shown && mode != RoomMode::Live && full.brightness < 1.0f;
		s_dim_amount = EaseRoomDim(s_dim_amount, want ? 1.0f : 0.0f, dt);
		if (want && s_dim_amount >= 1.0f && !s_dim_logged)
		{
			s_dim_logged = true;
			Console.WriteLn("(VR) Menu panel: the room behind it at %.0f%% brightness, %.0f%% saturation ([VR] MenuPanelRoom = %s).",
				100.0f * full.brightness, 100.0f * full.saturation, (mode == RoomMode::Freeze) ? "Freeze" : "Dim");
		}
		else if (!want && s_dim_amount <= 0.0f)
			s_dim_logged = false;
		if (!(s_dim_amount > 0.0f))
			return false;
		*out = RoomLookAt(full, s_dim_amount);
		return true;
	}

	const HeadPose::Snapshot* RoomFreeze::Step(bool screen, bool freeze, const HeadPose::Snapshot* last_written,
		const HeadPose::Snapshot& live, bool* began, bool* ended)
	{
		*began = false;
		*ended = false;
		if (!screen || !freeze)
		{
			if (frozen)
			{
				frozen = false;
				*ended = true;
			}
			return nullptr;
		}
		if (!frozen)
		{
			// The pose the room on screen was last rendered from (near enough: the newest write), so the picture does not
			// move as the hold begins.
			pose = (last_written && last_written->valid) ? *last_written : live;
			frozen = true;
			*began = true;
		}
		held = pose;
		// Stamped as the head now, so everything that ages poses (the prediction horizons, the pose ring) sees a head
		// held perfectly still rather than an old one.
		held.frame = live.frame;
		held.publish_ms = live.publish_ms;
		held.valid = pose.valid && live.valid;
		held.angular_velocity[0] = held.angular_velocity[1] = held.angular_velocity[2] = 0.0f;
		held.angular_valid = true;
		return &held;
	}

	float PanelYawOf(const float q[4], float fallback)
	{
		const float x = q[0], y = q[1], z = q[2], w = q[3];
		const float fx = -2.0f * (w * y + z * x);
		const float fz = -(1.0f - 2.0f * (x * x + y * y));
		if (!std::isfinite(fx) || !std::isfinite(fz) || (fx * fx + fz * fz) <= 1.0e-4f)
			return fallback;
		return std::atan2(-fx, -fz);
	}

	void PlacePanel(const float eye[3], float yaw, float pos[3], float quat[4])
	{
		// anchor + R(yaw) * (0, -drop, -distance), turned Ry(yaw) * Rx(-tilt) so its face (+Z) looks back up at the eyes.
		const float tilt = std::atan2(kPanelDropM, kPanelDistanceM);
		const float th = -0.5f * tilt, ts = std::sin(th), tc = std::cos(th);
		const float ys = std::sin(yaw * 0.5f), yc = std::cos(yaw * 0.5f);
		quat[0] = yc * ts;
		quat[1] = ys * tc;
		quat[2] = -ys * ts;
		quat[3] = yc * tc;
		pos[0] = eye[0] - kPanelDistanceM * std::sin(yaw);
		pos[1] = eye[1] - kPanelDropM;
		pos[2] = eye[2] - kPanelDistanceM * std::cos(yaw);
	}

	void PanelPlacer::Show(const float eye_now[3], float head_yaw)
	{
		placed = true;
		gliding = false;
		yaw = target_yaw = WrapAngle(head_yaw);
		for (int i = 0; i < 3; i++)
			eye[i] = target_eye[i] = eye_now[i];
	}

	bool PanelPlacer::Update(const float eye_now[3], float head_yaw, float dt)
	{
		if (!placed)
		{
			Show(eye_now, head_yaw);
			return false;
		}
		bool set_off = false;
		const float away = WrapAngle(head_yaw - yaw);
		if (!gliding && std::abs(away) > kPanelReplaceDeg * (kPi / 180.0f))
		{
			gliding = true;
			set_off = true;
			target_yaw = WrapAngle(head_yaw);
			for (int i = 0; i < 3; i++)
				target_eye[i] = eye_now[i];
		}
		if (gliding)
		{
			const float k = (std::isfinite(dt) && dt > 0.0f) ? (1.0f - std::exp(-dt / kPanelGlideS)) : 0.0f;
			const float left = WrapAngle(target_yaw - yaw);
			if (std::abs(left) < 0.25f * (kPi / 180.0f))
			{
				yaw = target_yaw;
				for (int i = 0; i < 3; i++)
					eye[i] = target_eye[i];
				gliding = false;
			}
			else
			{
				yaw = WrapAngle(yaw + left * k);
				for (int i = 0; i < 3; i++)
					eye[i] += (target_eye[i] - eye[i]) * k;
			}
		}
		return set_off;
	}

	bool FeelSelfTest()
	{
		int checks = 0;
		int failed = 0;
		const auto check = [&](bool ok, const char* what) {
			checks++;
			if (!ok)
			{
				failed++;
				Console.Error("(VR) Menu panel feel self-test FAIL: %s", what);
			}
		};
		const auto approx = [](float a, float b, float eps) { return std::abs(a - b) <= eps; };
		constexpr float kDeg = kPi / 180.0f;
		const float nan = std::numeric_limits<float>::quiet_NaN();

		// The look.
		const RoomLook live = RoomLookFor(RoomMode::Live, 0.55f);
		check(live.brightness == 1.0f && live.saturation == 1.0f, "Live: the room as in play");
		const RoomLook dim = RoomLookFor(RoomMode::Dim, 0.55f);
		check(approx(dim.brightness, 0.55f, 1e-6f) && approx(dim.saturation, kRoomSaturation, 1e-6f), "Dim: 55%, a little grey");
		const RoomLook frz = RoomLookFor(RoomMode::Freeze, 0.55f);
		check(frz.brightness == dim.brightness && frz.saturation == dim.saturation, "Freeze dims the same way");
		const RoomLook none = RoomLookFor(RoomMode::Dim, 1.0f);
		check(none.brightness == 1.0f && none.saturation == 1.0f, "brightness 1: no dim, no greying");
		check(RoomLookFor(RoomMode::Dim, 3.0f).brightness == 1.0f && RoomLookFor(RoomMode::Dim, -1.0f).brightness == 0.0f &&
				  RoomLookFor(RoomMode::Dim, nan).brightness == 1.0f,
			"brightness clamped to 0..1, NaN as none");
		const RoomLook half = RoomLookAt(dim, 0.5f);
		check(approx(half.brightness, 0.775f, 1e-6f) && approx(half.saturation, 0.9f, 1e-6f), "half way in: half the dim");
		check(RoomLookAt(dim, 0.0f).brightness == 1.0f && RoomLookAt(dim, 1.0f).brightness == dim.brightness &&
				  RoomLookAt(dim, 7.0f).brightness == dim.brightness,
			"no dim at 0, all of it at 1 and beyond");

		// The ease.
		check(approx(EaseRoomDim(0.0f, 1.0f, 0.125f), 0.5f, 1e-5f) && EaseRoomDim(0.0f, 1.0f, kRoomDimInS) == 1.0f,
			"the dim comes in over 0.25 s");
		check(approx(EaseRoomDim(1.0f, 0.0f, 0.1f), 0.5f, 1e-5f) && EaseRoomDim(1.0f, 0.0f, kRoomDimOutS) == 0.0f,
			"and goes over 0.2 s");
		check(EaseRoomDim(0.4f, 0.4f, 1.0f) == 0.4f && EaseRoomDim(0.3f, 1.0f, 0.0f) == 0.3f &&
				  EaseRoomDim(0.3f, 1.0f, -1.0f) == 0.3f,
			"no time, no change");
		check(approx(EaseRoomDim(nan, 1.0f, 0.05f), 0.2f, 1e-6f) && EaseRoomDim(0.5f, nan, 1.0f) == 0.0f,
			"NaN amount starts from none; NaN target eases out");
		{
			float a = 0.0f;
			int frames = 0;
			while (a < 1.0f && frames < 100)
			{
				a = EaseRoomDim(a, 1.0f, 1.0f / 60.0f);
				frames++;
			}
			check(frames >= 15 && frames <= 16, "at 60 presents a second the dim is in after a quarter second");
		}

		// The gamma: (c * b * dim^g) ^ (1/g) = dim * (c * b) ^ (1/g).
		for (const float g : {1.0f, 1.2f, 2.2f})
		{
			const float c = 0.4f;
			const float plain = std::pow(c, 1.0f / g);
			const float dimmed = std::pow(c * DimBeforeGamma(0.55f, g), 1.0f / g);
			check(approx(dimmed, 0.55f * plain, 1e-5f), "the dim survives the lift's gamma exactly");
		}
		check(DimBeforeGamma(1.0f, 1.2f) == 1.0f && DimBeforeGamma(nan, 1.2f) == 1.0f &&
				  approx(DimBeforeGamma(0.5f, 0.0f), 0.5f, 1e-6f),
			"no dim stays 1; a bad gamma counts as 1");

		// The hold.
		{
			HeadPose::Snapshot written;
			written.valid = true;
			written.orientation_y = std::sin(0.25f);
			written.orientation_w = std::cos(0.25f);
			written.position_x = 0.1f;
			written.position_valid = true;
			written.frame = 10;
			written.publish_ms = 1000;
			HeadPose::Snapshot head = written;
			head.orientation_y = std::sin(-0.4f);
			head.orientation_w = std::cos(-0.4f);
			head.position_x = 0.3f;
			head.frame = 12;
			head.publish_ms = 1033;
			head.angular_velocity[1] = 2.0f;
			head.angular_valid = true;
			RoomFreeze f;
			bool began = false, ended = false;
			check(f.Step(false, true, &written, head, &began, &ended) == nullptr && !began && !ended && !f.frozen,
				"no screen: the head as usual");
			check(f.Step(true, false, &written, head, &began, &ended) == nullptr && !f.frozen, "Dim or Live: the head as usual");
			const HeadPose::Snapshot* p = f.Step(true, true, &written, head, &began, &ended);
			check(p && began && !ended && f.frozen, "a screen with Freeze: the hold begins");
			check(p && p->orientation_y == written.orientation_y && p->position_x == written.position_x,
				"held at the pose last written, not the head now");
			check(p && p->frame == head.frame && p->publish_ms == head.publish_ms && p->valid, "stamped as the head now");
			check(p && p->angular_valid && p->angular_velocity[1] == 0.0f, "and not moving");
			head.orientation_y = std::sin(0.6f);
			head.orientation_w = std::cos(0.6f);
			head.frame = 14;
			head.publish_ms = 1066;
			HeadPose::Snapshot later = written;
			later.orientation_y = 0.9f;
			p = f.Step(true, true, &later, head, &began, &ended);
			check(p && !began && p->orientation_y == written.orientation_y && p->publish_ms == 1066,
				"still held as the head turns on (the newer writes are its own)");
			head.valid = false;
			p = f.Step(true, true, &later, head, &began, &ended);
			check(p && !p->valid, "a head lost while held is lost");
			head.valid = true;
			p = f.Step(false, true, &later, head, &began, &ended);
			check(p == nullptr && ended && !f.frozen, "the screen gone: the head drives the camera again");
			p = f.Step(true, true, nullptr, head, &began, &ended);
			check(p && began && p->orientation_y == head.orientation_y, "with nothing written yet it holds the head as it is");
			p = f.Step(true, false, &written, head, &began, &ended);
			check(p == nullptr && ended, "switched to Dim while held: the head again");
			f.Step(true, true, &written, head, &began, &ended);
			f.Reset();
			check(!f.frozen, "reset");
		}

		// The placement.
		{
			const float eye[3] = {0.2f, 1.2f, -0.1f};
			const float id[4] = {0.0f, 0.0f, 0.0f, 1.0f};
			check(approx(PanelYawOf(id, 9.0f), 0.0f, 1e-6f), "looking down -Z is yaw 0");
			const float left[4] = {0.0f, std::sin(0.25f * kPi), 0.0f, std::cos(0.25f * kPi)};
			check(approx(PanelYawOf(left, 9.0f), 0.5f * kPi, 1e-5f), "turned 90 deg left is yaw +90");
			// Turned 30 deg left and pitched 40 deg down (Ry * Rx): the yaw is still 30 (the pitch is left out).
			const float hy = 15.0f * kDeg, hp = -20.0f * kDeg;
			const float down_left[4] = {std::cos(hy) * std::sin(hp), std::sin(hy) * std::cos(hp), -std::sin(hy) * std::sin(hp),
				std::cos(hy) * std::cos(hp)};
			check(approx(PanelYawOf(down_left, 9.0f), 30.0f * kDeg, 1e-4f), "a look down at the belt keeps its yaw");
			const float straight_down[4] = {std::sin(-0.25f * kPi), 0.0f, 0.0f, std::cos(-0.25f * kPi)};
			check(PanelYawOf(straight_down, 9.0f) == 9.0f, "straight down: the fallback");
			float pos[3], q[4];
			PlacePanel(eye, 0.0f, pos, q);
			check(approx(pos[0], 0.2f, 1e-6f) && approx(pos[1], 1.2f - kPanelDropM, 1e-6f) && approx(pos[2], -1.1f, 1e-6f),
				"yaw 0: 1 m ahead (-Z), 0.12 m below the eyes");
			PlacePanel(eye, 0.5f * kPi, pos, q);
			check(approx(pos[0], 0.2f - kPanelDistanceM, 1e-5f) && approx(pos[2], -0.1f, 1e-5f), "yaw +90: 1 m to the left (-X)");
			for (const float yaw : {0.0f, 1.0f, -2.5f})
			{
				// Its face (+Z turned by q) points at the eyes.
				PlacePanel(eye, yaw, pos, q);
				const float x = q[0], y = q[1], z = q[2], w = q[3];
				const float n[3] = {2.0f * (x * z + w * y), 2.0f * (y * z - w * x), 1.0f - 2.0f * (x * x + y * y)};
				const float d[3] = {eye[0] - pos[0], eye[1] - pos[1], eye[2] - pos[2]};
				const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
				check(approx(n[0], d[0] / len, 1e-5f) && approx(n[1], d[1] / len, 1e-5f) && approx(n[2], d[2] / len, 1e-5f) &&
						  approx(x * x + y * y + z * z + w * w, 1.0f, 1e-5f),
					"the panel faces the eyes");
			}
		}

		// The re-place.
		{
			const float eye[3] = {0.0f, 1.2f, 0.0f};
			constexpr float dt = 1.0f / 90.0f;
			PanelPlacer pl;
			check(!pl.Update(eye, 0.3f, dt) && pl.placed && approx(pl.yaw, 0.3f, 1e-6f),
				"the first update places it where the head looks");
			pl.Show(eye, 0.0f);
			bool moved = false;
			for (int i = 0; i < 90 && !moved; i++)
				moved = pl.Update(eye, 59.0f * kDeg, dt);
			check(!moved && pl.yaw == 0.0f, "a look 59 deg away leaves it where it is");
			check(pl.Update(eye, 61.0f * kDeg, dt) && pl.gliding, "61 deg away sets it off");
			check(approx(pl.yaw, 61.0f * kDeg * (1.0f - std::exp(-dt / kPanelGlideS)), 1e-4f), "gliding, not jumping");
			int steps = 0;
			bool again = false;
			while (pl.gliding && steps < 400)
			{
				again = again || pl.Update(eye, 61.0f * kDeg, dt);
				steps++;
			}
			check(!again, "no second set-off while gliding");
			check(!pl.gliding && pl.yaw == WrapAngle(61.0f * kDeg), "there, exactly");
			check(steps > 40 && steps < 120, "in under a second and a half (0.15 s time constant, 0.25 deg snap)");
			// Across the back (+170 to -170 is 20 deg the short way): no re-place; a far turn glides the short way.
			pl.Show(eye, 170.0f * kDeg);
			check(!pl.Update(eye, -170.0f * kDeg, dt), "20 deg across the back is near");
			pl.Show(eye, 170.0f * kDeg);
			check(pl.Update(eye, -100.0f * kDeg, dt), "90 deg across the back is far");
			check(pl.yaw > 170.0f * kDeg || pl.yaw < -170.0f * kDeg, "and it goes round the short way");
			// It re-places in front of the eyes as they are then.
			const float eye2[3] = {0.5f, 1.0f, 0.2f};
			pl.Show(eye, 0.0f);
			pl.Update(eye2, kPi, dt);
			for (int i = 0; i < 400 && pl.gliding; i++)
				pl.Update(eye2, kPi, dt);
			check(pl.eye[0] == eye2[0] && pl.eye[1] == eye2[1] && pl.eye[2] == eye2[2], "re-placed in front of the eyes now");
			pl.Hide();
			check(!pl.placed, "hidden");
		}

		const bool ok = (failed == 0);
		Console.WriteLn("(VR) Menu panel feel self-test: %d checks, %d failed%s", checks, failed, ok ? "" : " (see above)");
		return ok;
	}

	bool SelfTest()
	{
		int checks = 0;
		int failed = 0;
		const auto check = [&](bool ok, const char* what) {
			checks++;
			if (!ok)
			{
				failed++;
				Console.Error("(VR) Menu panel self-test FAIL: %s", what);
			}
		};

		// The overlay test.
		DrawNote room;
		room.fbp = 0xE00;
		room.zbp = 0x1C00;
		room.z_test = true;
		room.z_write = true;
		room.tme = true;
		DrawNote translucent = room;
		translucent.z_write = false;
		DrawNote clear;
		clear.fbp = 0xE00;
		clear.zbp = 0x1C00;
		clear.z_const = true;
		clear.z_write = true;
		clear.sprite = true;
		clear.covers = true;
		DrawNote menu;
		menu.fbp = 0xE00;
		menu.zbp = 0x1C00;
		menu.tme = true;
		menu.fst = true;
		menu.sprite = true;
		menu.z_const = true;
		DrawNote menu_untextured = menu;
		menu_untextured.tme = false;
		menu_untextured.fst = false;
		DrawNote persp_no_z = menu;
		persp_no_z.fst = false;
		persp_no_z.q_const = false;
		check(!IsOverlay(room), "a depth-tested draw is the room");
		check(!IsOverlay(translucent), "a depth-tested translucent draw is the room");
		check(!IsOverlay(clear), "the frame clear is not an overlay");
		check(IsOverlay(menu), "a UV sprite without the depth test is an overlay");
		check(IsOverlay(menu_untextured), "an untextured draw without the depth test is an overlay");
		check(!IsOverlay(persp_no_z), "a perspective draw without the depth test is not an overlay");
		DrawNote const_q = persp_no_z;
		const_q.q_const = true;
		check(IsOverlay(const_q), "a constant-Q draw without the depth test is an overlay");
		DrawNote prompt_text = menu;
		prompt_text.z_test = true;
		check(IsOverlay(prompt_text) && !IsRoom(prompt_text), "depth-tested UV text (the take-it prompt's) is an overlay, not the room");
		DrawNote flat_fill = menu_untextured;
		flat_fill.z_test = true;
		check(IsOverlay(flat_fill) && !IsRoom(flat_fill), "a depth-tested untextured draw at one depth is an overlay");
		DrawNote untextured_3d = flat_fill;
		untextured_3d.z_const = false;
		check(IsRoom(untextured_3d) && !IsOverlay(untextured_3d), "a depth-tested untextured draw with varying depth is the room");
		DrawNote tint = menu;
		tint.covers = true;
		tint.blend = true;
		DrawNote opaque_full = tint;
		opaque_full.blend = false;
		check(IsBackdrop(tint), "a blended overlay over the whole frame is the screen's backdrop");
		check(!IsBackdrop(opaque_full), "an opaque full-frame picture is the screen's content");
		check(!IsBackdrop(menu), "a window is content");
		check(!IsBackdrop(clear), "the clear is no backdrop");

		// The tracker: File #1's frame (clear, room, translucent room), then a screen's draws (the item screen and the
		// map: its own depth clear, the backdrop, the windows), into 0xE00 while 0x0 is shown, then the next frame into 0x0.
		SplitTracker t;
		check(t.NoteDraw(menu, true) == SplitAction::None, "an overlay into an untracked target asks for nothing");
		check(t.SlotForPresent(0xE00) < 0, "nothing to present before any room");
		check(t.NoteDraw(clear, true) == SplitAction::None, "the frame's first clear asks for nothing");
		for (int i = 0; i < 100; i++)
			t.NoteDraw(room, true);
		for (int i = 0; i < 60; i++)
			t.NoteDraw(translucent, true);
		check(t.SlotForPresent(0xE00) < 0, "no split before the screen's first draw");
		const SplitAction a = t.NoteDraw(clear, true);
		check(a == SplitAction::Snapshot && t.snapshot_fbp == 0xE00 && !t.skip,
			"the screen's depth clear after the room asks for the snapshot, and is drawn");
		t.SnapshotTaken(true);
		check(t.NoteDraw(tint, true) == SplitAction::None && t.skip, "the screen's backdrop tint is left out");
		check(t.NoteDraw(menu, true) == SplitAction::None && !t.skip, "a window asks for nothing and is drawn");
		check(t.NoteDraw(menu_untextured, true) == SplitAction::None, "nor an untextured one");
		check(t.NoteDraw(prompt_text, true) == SplitAction::None, "nor depth-tested text");
		check(t.NoteDraw(room, true) == SplitAction::None, "nor the screen's own 3D (an item turning)");
		t.NoteDraw(clear, true);
		const int s0 = t.SlotForPresent(0xE00);
		check(s0 >= 0 && t.slot[s0].overlay_draws == 7, "the frame presents split, with all seven of the screen's draws");
		// The next frame, into the other buffer, does not touch this one.
		DrawNote clear0 = clear, room0 = room, menu0 = menu;
		clear0.fbp = room0.fbp = menu0.fbp = 0x0;
		t.NoteShown(0x0);
		t.NoteDraw(clear0, true);
		for (int i = 0; i < 10; i++)
			t.NoteDraw(room0, true);
		check(t.SlotForPresent(0xE00) == s0, "drawing the other buffer keeps this one's split");
		check(t.NoteDraw(menu0, true) == SplitAction::Snapshot && t.snapshot_fbp == 0x0, "the other buffer splits on its own");
		t.SnapshotTaken(true);
		const int s1 = t.SlotForPresent(0x0);
		check(s1 >= 0 && s1 != s0, "the two buffers keep a slot each");
		// 0xE00 is shown (twice, at 30 fps), then drawn again: its next frame.
		t.NoteShown(0xE00);
		check(t.SlotForPresent(0xE00) == s0, "a target being shown keeps its split for the next showing");
		t.NoteShown(0xE00);
		t.NoteDraw(clear, true);
		check(t.SlotForPresent(0xE00) < 0, "the first draw after it was shown begins a new frame");
		check(t.NoteDraw(clear, true) == SplitAction::None, "a second clear with no room asks for nothing");
		t.NoteDraw(room, true);
		check(t.NoteDraw(tint, true) == SplitAction::Snapshot && t.skip,
			"a backdrop straight after the room asks for the snapshot, and is left out (the take-it prompt)");
		t.SnapshotTaken(false);
		check(t.SlotForPresent(0xE00) < 0, "a snapshot that could not be taken leaves the frame unsplit");
		check(t.NoteDraw(menu, true) == SplitAction::Snapshot, "the next screen draw asks again");
		t.SnapshotTaken(true);
		// A plain overlay inside the room (a glow), then more room: not the screen; the snapshot is dropped.
		check(t.NoteDraw(room, true) == SplitAction::None && t.SlotForPresent(0xE00) < 0,
			"room after a snapshot a plain overlay asked for drops it");
		check(t.NoteDraw(clear, true) == SplitAction::Snapshot, "and the screen's clear asks again");
		t.SnapshotTaken(true);
		check(t.NoteDraw(room, true) == SplitAction::None && t.SlotForPresent(0xE00) >= 0,
			"after a clear the screen's own 3D keeps the split");
		// No screen known to be up (its first frame, before the flag reaches the GS): a plain overlay starts nothing, a
		// clear or a backdrop after the room still asks for the snapshot, and nothing is left out.
		SplitTracker off;
		off.NoteDraw(clear, false);
		off.NoteDraw(room, false);
		check(off.NoteDraw(menu, false) == SplitAction::None && off.SlotForPresent(0xE00) < 0,
			"with no screen known to be up a plain overlay starts nothing");
		check(off.NoteDraw(tint, false) == SplitAction::Snapshot && !off.skip,
			"a backdrop after the room asks for the snapshot, and is drawn");
		off.SnapshotTaken(true);
		check(off.NoteDraw(tint, false) == SplitAction::None && !off.skip, "nothing is left out");
		SplitTracker off2;
		off2.NoteDraw(clear, false);
		off2.NoteDraw(room, false);
		check(off2.NoteDraw(clear, false) == SplitAction::Snapshot && !off2.skip, "so does the screen's clear");
		// A 2D-only frame (no room) is never split.
		SplitTracker flat;
		flat.NoteDraw(clear, true);
		check(flat.NoteDraw(menu, true) == SplitAction::None && flat.NoteDraw(tint, true) == SplitAction::None && !flat.skip,
			"a frame with no room is not split (nor its backdrop left out)");
		// A third target (a texture render with its own depth) takes the oldest slot, not a live one.
		SplitTracker three;
		three.NoteDraw(clear, true);
		three.NoteDraw(room, true);
		three.NoteDraw(menu, true);
		three.SnapshotTaken(true);
		three.NoteDraw(clear0, true);
		three.NoteDraw(room0, true);
		DrawNote other = room;
		other.fbp = 0x2A00;
		three.NoteDraw(other, true);
		check(three.SlotForPresent(0xE00) < 0, "the oldest slot is the one reused");

		const bool ok = (failed == 0);
		Console.WriteLn("(VR) Menu panel self-test: %d checks, %d failed%s", checks, failed, ok ? "" : " (see above)");
		return ok;
	}
}
