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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace VR::MenuPanel
{
	namespace
	{
		std::atomic_bool s_screen_up{false};

		// The present-time gate (GS thread): a screen up, or one gone fewer than this many vsyncs ago, so frames drawn while
		// it was up and presented after its flag fell still show without it on the first-person screen.
		constexpr u32 kTailVsyncs = 6;
		u32 s_vsyncs_since_up = kTailVsyncs;

		SplitTracker s_tracker;
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

		// --- PCSX2_VR_MENUPANEL_DUMP=<dir>[,everyN]: the panel, the room and the finished frame (layer 0) as PNGs ---
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
			std::vector<u32> frame)
		{
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
			FileSystem::EnsureDirectoryExists(dir.c_str(), true);
			const std::string base = Path::Combine(dir, fmt::format("menupanel_{:06d}", serial));
			const RGBA8Image pi(w, h, std::move(panel));
			const RGBA8Image ri(w, h, std::move(room));
			const RGBA8Image fi(w, h, std::move(frame));
			const bool ok = pi.SaveToFile((base + "_panel.png").c_str()) && ri.SaveToFile((base + "_room.png").c_str()) &&
			                fi.SaveToFile((base + "_frame.png").c_str());
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
			if (px * 12 > s_dump_size)
				return;
			vmaInvalidateAllocation(dev->GetAllocator(), s_dump_alloc, 0, static_cast<VkDeviceSize>(px * 12));
			std::vector<u32> panel(px), room(px), frame(px);
			std::memcpy(panel.data(), s_dump_map, px * 4);
			std::memcpy(room.data(), static_cast<const u8*>(s_dump_map) + px * 4, px * 4);
			std::memcpy(frame.data(), static_cast<const u8*>(s_dump_map) + px * 8, px * 4);
			s_dump_writes.fetch_add(1, std::memory_order_acq_rel);
			std::thread(WriteDump, GetDumpConfig().dir, s_dump_serial, s_dump_w, s_dump_h, std::move(panel), std::move(room),
				std::move(frame))
				.detach();
		}

		void MaybeDump(GSDeviceVK* dev, VkCommandBuffer cmd, GSTextureVK* panel, GSTextureVK* room, GSTextureVK* frame,
			u32 sx, u32 sy, u32 w, u32 h)
		{
			const DumpConfig& cfg = GetDumpConfig();
			if (!cfg.enabled || !StereoState::DumpAllowed())
				return;
			const u64 n = s_dump_count++;
			if ((n % cfg.every_n) != 0 || s_dump_pending || s_dump_writes.load(std::memory_order_acquire) >= 2)
				return;
			const u64 plane = static_cast<u64>(w) * h * 4u;
			if (plane * 3 > 0x7FFFFFFFull || !EnsureDumpBuffer(dev, static_cast<u32>(plane * 3)))
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

	void TakeSnapshot(GSTexture* colour)
	{
		GSDeviceVK* const dev = VulkanDevice();
		const int slot = s_tracker.snapshot_slot;
		if (!dev || !colour || slot < 0 || slot >= static_cast<int>(SplitTracker::kSlots) || colour->IsDepthStencil())
		{
			s_tracker.SnapshotTaken(false);
			return;
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
			snap = g_gs_device->CreateRenderTarget(colour->GetWidth(), colour->GetHeight(), colour->GetFormat(), false, false,
				colour->GetArrayLayers());
			if (!snap)
			{
				s_tracker.SnapshotTaken(false);
				return;
			}
		}
		// The GS's own copy, recorded in draw order into the GS command buffer: the room as drawn, before the screen's
		// first draw.
		g_gs_device->CopyRect(colour, snap, GSVector4i(0, 0, colour->GetWidth(), colour->GetHeight()), 0, 0);
		s_tracker.SnapshotTaken(true);
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
		return snap;
	}

	bool ExportPanel(u32 fbp, GSTexture* colour, u32 src_x, u32 src_y, u32 w, u32 h, float u0, float v0, float u1, float v1)
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
		MaybeDump(dev, cmd, panel, room, frame, src_x, src_y, w, h);
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
	void TakeSnapshot(GSTexture*) { s_tracker.SnapshotTaken(false); }
	GSTexture* RoomFor(u32, GSTexture*) { return nullptr; }
	bool ExportPanel(u32, GSTexture*, u32, u32, u32, u32, float, float, float, float)
	{
		NoPanel("the menu panel is Vulkan only");
		return false;
	}
	void OnVSync() {}
	void Destroy() {}
#endif

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
