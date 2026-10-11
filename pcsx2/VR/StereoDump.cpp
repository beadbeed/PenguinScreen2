// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/StereoDump.h"
#include "VR/StereoState.h"

#include "Config.h"
#include "GS/GS.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Common/GSTexture.h"
#include "GS/Renderers/Vulkan/GSDeviceVK.h"
#include "GS/Renderers/Vulkan/GSTextureVK.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Image.h"
#include "common/Path.h"

#include "fmt/format.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace VR::StereoDump
{
	namespace
	{
		struct DumpConfig
		{
			bool enabled = false;
			std::string dir;
			u32 every_n = 30;
		};

		// PCSX2_VR_STEREODUMP=<dir>[,everyN], read once.
		const DumpConfig& GetDumpConfig()
		{
			static const DumpConfig cfg = []() {
				DumpConfig c;
				const char* env = std::getenv("PCSX2_VR_STEREODUMP");
				if (!env || env[0] == '\0')
					return c;
				std::string value(env);
				const size_t comma = value.rfind(',');
				if (comma != std::string::npos)
				{
					const std::string tail = value.substr(comma + 1);
					char* end = nullptr;
					const long n = std::strtol(tail.c_str(), &end, 10);
					if (!tail.empty() && end && *end == '\0' && n > 0)
					{
						c.every_n = static_cast<u32>(n);
						value.resize(comma);
					}
				}
				if (value.empty())
					return c;
				c.dir = std::move(value);
				c.enabled = true;
				Console.WriteLn("(VR) Stereo dump armed: %s, every %u frames (first person, offline only)",
					c.dir.c_str(), c.every_n);
				return c;
			}();
			return cfg;
		}

		// What the GS used for the captured frame, for the JSON sidecar.
		struct DumpInfo
		{
			u64 frame = 0;
			u32 width = 0;
			u32 height = 0;
			float separation = 0.0f;
			float convergence = 0.0f;
			float near_floor = 0.0f;
			float roll_deg = 0.0f;
			bool roll_parallax = false;
			bool roll_valid = false;
			float aspect = 4.0f / 3.0f;
			bool bgra = false;
			// First-person auto stereo (StereoState::Params fp_*), and when the frame was captured.
			bool fp_auto = false;
			float ipd_mm = 0.0f;
			float screen_w_m = 0.0f;
			float screen_h_m = 0.0f;
			float screen_d_m = 0.0f;
			float strength = 1.0f;
			float sep_target = 0.0f;
			float ease = 1.0f;
			float ndc_ratio = 1.0f;
			u64 t_ms = 0;
		};

		// Persistently mapped host-visible readback buffer (GS thread only).
		GSDeviceVK* s_buffer_device = nullptr;
		VmaAllocator s_buffer_allocator = VK_NULL_HANDLE;
		VkBuffer s_buffer = VK_NULL_HANDLE;
		VmaAllocation s_allocation = VK_NULL_HANDLE;
		void* s_mapped = nullptr;
		u32 s_buffer_size = 0;

		bool s_pending = false;
		u64 s_pending_fence = 0;
		DumpInfo s_pending_info;

		u64 s_frame = 0;
		bool s_logged_online = false;
		bool s_logged_failure = false;
		std::atomic<int> s_writes_in_flight{0};

		void ForgetBuffer()
		{
			s_buffer = VK_NULL_HANDLE;
			s_allocation = VK_NULL_HANDLE;
			s_mapped = nullptr;
			s_buffer_size = 0;
			s_buffer_device = nullptr;
			s_buffer_allocator = VK_NULL_HANDLE;
		}

		bool EnsureBuffer(GSDeviceVK* dev, u32 size)
		{
			// A restarted renderer is a new device: the old buffer died with its allocator, so just forget it.
			if (s_buffer != VK_NULL_HANDLE && (s_buffer_device != dev || s_buffer_allocator != dev->GetAllocator()))
			{
				ForgetBuffer();
				s_pending = false;
			}

			if (s_buffer != VK_NULL_HANDLE && s_buffer_size == size)
				return true;

			if (s_buffer != VK_NULL_HANDLE)
			{
				dev->DeferBufferDestruction(s_buffer, s_allocation);
				ForgetBuffer();
			}

			const VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0u, size,
				VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0u, nullptr};

			VmaAllocationCreateInfo aci = {};
			aci.usage = VMA_MEMORY_USAGE_GPU_TO_CPU;
			aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
			aci.preferredFlags = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;

			VmaAllocationInfo ai = {};
			VkBuffer buffer = VK_NULL_HANDLE;
			VmaAllocation allocation = VK_NULL_HANDLE;
			const VkResult res = vmaCreateBuffer(dev->GetAllocator(), &bci, &aci, &buffer, &allocation, &ai);
			if (res != VK_SUCCESS || !ai.pMappedData)
			{
				if (res == VK_SUCCESS)
					dev->DeferBufferDestruction(buffer, allocation);
				if (!s_logged_failure)
				{
					s_logged_failure = true;
					Console.Warning("(VR) Stereo dump: could not create a %u-byte readback buffer (VkResult %d).",
						size, static_cast<int>(res));
				}
				return false;
			}

			s_buffer_device = dev;
			s_buffer_allocator = dev->GetAllocator();
			s_buffer = buffer;
			s_allocation = allocation;
			s_mapped = ai.pMappedData;
			s_buffer_size = size;
			return true;
		}

		void WriteJson(const std::string& path, const DumpInfo& info)
		{
			const std::string json = fmt::format(
				"{{\n  \"frame\": {},\n  \"w\": {},\n  \"h\": {},\n  \"sep\": {:.6f},\n  \"conv\": {:.4f},\n"
				"  \"near_floor\": {:.6f},\n  \"roll_deg\": {:.4f},\n  \"roll_parallax\": {},\n  \"roll_valid\": {},\n"
				"  \"aspect\": {:.6f},\n  \"auto\": {},\n  \"ipd_mm\": {:.3f},\n  \"screen_w_m\": {:.4f},\n  \"screen_h_m\": {:.4f},\n"
				"  \"screen_d_m\": {:.4f},\n  \"strength\": {:.4f},\n  \"sep_target\": {:.8f},\n  \"ease\": {:.6f},\n"
				"  \"ndc_ratio\": {:.4f},\n  \"t_ms\": {}\n}}\n",
				info.frame, info.width, info.height, info.separation, info.convergence, info.near_floor, info.roll_deg,
				info.roll_parallax ? "true" : "false", info.roll_valid ? "true" : "false", info.aspect,
				info.fp_auto ? "true" : "false", info.ipd_mm, info.screen_w_m, info.screen_h_m, info.screen_d_m, info.strength,
				info.sep_target, info.ease, info.ndc_ratio, info.t_ms);
			FileSystem::WriteStringToFile(path.c_str(), json);
		}

		// Worker thread: owns its pixels, so it outlives nothing on the GS thread.
		void WritePair(std::string dir, DumpInfo info, std::vector<u32> left, std::vector<u32> right)
		{
			const auto fix = [&info](std::vector<u32>& px) {
				for (u32& p : px)
				{
					if (info.bgra)
						p = (p & 0xFF00FF00u) | ((p & 0x00FF0000u) >> 16) | ((p & 0x000000FFu) << 16);
					p |= 0xFF000000u; // opaque: PS2 alpha is not coverage
				}
			};
			fix(left);
			fix(right);

			FileSystem::EnsureDirectoryExists(dir.c_str(), true);
			const std::string base = Path::Combine(dir, fmt::format("stereo_{:06d}", info.frame));
			const std::string left_path = base + "_L.png";
			const std::string right_path = base + "_R.png";
			const RGBA8Image left_img(info.width, info.height, std::move(left));
			const RGBA8Image right_img(info.width, info.height, std::move(right));
			const bool ok = left_img.SaveToFile(left_path.c_str()) && right_img.SaveToFile(right_path.c_str());
			WriteJson(base + ".json", info);
			if (ok)
				Console.WriteLn("(VR) Stereo dump: %s", base.c_str());
			else
				Console.Warning("(VR) Stereo dump: failed to write %s", base.c_str());
			s_writes_in_flight.fetch_sub(1, std::memory_order_acq_rel);
		}

		// Collect a finished readback (never waits): once the GPU has passed the copy's fence, the bytes are final.
		void PollPending(GSDeviceVK* dev)
		{
			if (!s_pending)
				return;
			if (s_buffer == VK_NULL_HANDLE || s_buffer_device != dev)
			{
				s_pending = false;
				return;
			}
			if (dev->GetCompletedFenceCounter() < s_pending_fence)
				return;

			s_pending = false;
			const DumpInfo info = s_pending_info;
			const size_t layer_px = static_cast<size_t>(info.width) * info.height;
			const size_t layer_bytes = layer_px * 4;
			if (layer_bytes * 2 > s_buffer_size)
				return;

			vmaInvalidateAllocation(dev->GetAllocator(), s_allocation, 0, static_cast<VkDeviceSize>(layer_bytes * 2));
			std::vector<u32> left(layer_px);
			std::vector<u32> right(layer_px);
			std::memcpy(left.data(), s_mapped, layer_bytes);
			std::memcpy(right.data(), static_cast<const u8*>(s_mapped) + layer_bytes, layer_bytes);

			s_writes_in_flight.fetch_add(1, std::memory_order_acq_rel);
			std::thread(WritePair, GetDumpConfig().dir, info, std::move(left), std::move(right)).detach();
		}
	}

	void MaybeCapture(GSTexture* current)
	{
		const DumpConfig& cfg = GetDumpConfig();
		if (!cfg.enabled)
			return;
		if (!g_gs_device || g_gs_device->GetRenderAPI() != RenderAPI::Vulkan)
			return;

		GSDeviceVK* const dev = GSDeviceVK::GetInstance();
		PollPending(dev);

		const u64 frame = s_frame++;
		if (!current || current->GetArrayLayers() < 2)
			return;

		const StereoState::Params st = StereoState::Get();
		if (!st.enabled || !st.first_person)
			return;

		if (!StereoState::DumpAllowed())
		{
			if (!s_logged_online)
			{
				s_logged_online = true;
				Console.Warning("(VR) Stereo dump: refused, the network adapter is on (offline only).");
			}
			return;
		}

		if ((frame % cfg.every_n) != 0)
			return;
		if (s_pending || s_writes_in_flight.load(std::memory_order_acquire) >= 2)
			return;

		GSTextureVK* const tex = static_cast<GSTextureVK*>(current);
		const VkFormat vk_format = tex->GetVkFormat();
		const bool bgra = (vk_format == VK_FORMAT_B8G8R8A8_UNORM);
		if (vk_format != VK_FORMAT_R8G8B8A8_UNORM && !bgra)
		{
			if (!s_logged_failure)
			{
				s_logged_failure = true;
				Console.Warning("(VR) Stereo dump: frame format %d is not RGBA8; not dumping.", static_cast<int>(vk_format));
			}
			return;
		}

		const int w = tex->GetWidth();
		const int h = tex->GetHeight();
		if (w <= 0 || h <= 0)
			return;
		const u64 layer_bytes = static_cast<u64>(w) * static_cast<u64>(h) * 4u;
		if (layer_bytes * 2 > 0x7FFFFFFFull)
			return;
		if (!EnsureBuffer(dev, static_cast<u32>(layer_bytes * 2)))
			return;

		if (dev->InRenderPass())
			dev->EndRenderPass();
		tex->CommitClear();

		const VkCommandBuffer cmd = dev->GetCurrentCommandBuffer();
		tex->TransitionToLayout(cmd, GSTextureVK::Layout::TransferSrc);

		VkBufferImageCopy regions[2] = {};
		for (u32 layer = 0; layer < 2; layer++)
		{
			regions[layer].bufferOffset = static_cast<VkDeviceSize>(layer_bytes * layer);
			regions[layer].bufferRowLength = 0;
			regions[layer].bufferImageHeight = 0;
			regions[layer].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			regions[layer].imageSubresource.mipLevel = 0;
			regions[layer].imageSubresource.baseArrayLayer = tex->GetBaseArrayLayer() + layer;
			regions[layer].imageSubresource.layerCount = 1;
			regions[layer].imageOffset.x = 0;
			regions[layer].imageOffset.y = 0;
			regions[layer].imageOffset.z = 0;
			regions[layer].imageExtent.width = static_cast<u32>(w);
			regions[layer].imageExtent.height = static_cast<u32>(h);
			regions[layer].imageExtent.depth = 1;
		}
		vkCmdCopyImageToBuffer(cmd, tex->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_buffer, 2, regions);

		VkBufferMemoryBarrier barrier = {};
		barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.buffer = s_buffer;
		barrier.offset = 0;
		barrier.size = VK_WHOLE_SIZE;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
			&barrier, 0, nullptr);

		DumpInfo info;
		info.frame = frame;
		info.width = static_cast<u32>(w);
		info.height = static_cast<u32>(h);
		info.separation = st.separation;
		info.convergence = st.convergence;
		info.near_floor = st.near_floor;
		info.roll_parallax = StereoState::RollParallaxEnabled();
		float roll = 0.0f;
		info.roll_valid = StereoState::HeadRoll(&roll) && std::isfinite(roll);
		info.roll_deg = info.roll_valid ? roll * (180.0f / 3.14159265358979323846f) : 0.0f;
		info.aspect = (GSConfig.AspectRatio == AspectRatioType::R16_9) ? (16.0f / 9.0f) : (4.0f / 3.0f);
		info.bgra = bgra;
		info.fp_auto = st.fp_auto;
		info.ipd_mm = st.fp_ipd_m * 1000.0f;
		info.screen_w_m = st.fp_screen_w_m;
		info.screen_h_m = st.fp_screen_h_m;
		info.screen_d_m = st.fp_screen_d_m;
		info.strength = st.fp_strength;
		info.sep_target = st.fp_sep_target;
		info.ease = st.fp_ease;
		info.ndc_ratio = st.fp_ndc_ratio;
		info.t_ms = static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());

		s_pending_info = info;
		s_pending_fence = dev->GetCurrentFenceCounter();
		s_pending = true;
	}
}
