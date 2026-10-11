// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#ifdef ENABLE_VULKAN

#include "GS/Renderers/Vulkan/VKLoader.h"

#include "common/Pcsx2Defs.h"

namespace VR
{
	bool BeginVulkanBootstrap();
	bool VulkanBootstrapActive();

	bool CreateVulkanInstanceThroughXR(const VkInstanceCreateInfo* ci, VkInstance* out);

	VkPhysicalDevice GetXrVulkanPhysicalDevice(VkInstance instance);

	bool CreateVulkanDeviceThroughXR(VkPhysicalDevice physical_device, const VkDeviceCreateInfo* ci, VkDevice* out);

	bool OnVulkanDeviceCreated(VkInstance instance, VkPhysicalDevice physical_device, VkDevice device,
		u32 queue_family, VkQueue queue);

	void OnGSDeviceDestroyed();

	// The OpenXR session is bound to the GS graphics queue (index 0). XR_KHR_vulkan_enable2 makes the app synchronise
	// that queue against the runtime's own use of it inside xrBeginFrame, xrEndFrame, xrAcquireSwapchainImage and
	// xrReleaseSwapchainImage, which run on the VR pacer thread as well as the GS thread. Every vkQueueSubmit,
	// vkQueuePresentKHR and vkDeviceWaitIdle on the GS side and each of those XR calls holds this lock (never
	// xrWaitFrame or xrWaitSwapchainImage). Lock order: XRCompositor's submit mutex, then this lock; never the reverse.
	// Bounded: a lock held over 500 ms (the runtime not answering) abandons VR instead of blocking emulation. A scope
	// opened while this thread already holds the lock (the spin submit inside SubmitCommandBuffer) takes nothing.
	class ScopedXrQueueLock
	{
	public:
		ScopedXrQueueLock();
		~ScopedXrQueueLock();

		ScopedXrQueueLock(const ScopedXrQueueLock&) = delete;
		ScopedXrQueueLock& operator=(const ScopedXrQueueLock&) = delete;

	private:
		bool m_locked = false;
	};

	// On once the session is bound to the queue, off when VR is torn down (or a VR thread is stuck in the runtime).
	void SetXrQueueLockEnabled(bool enabled);

	// True once a wait for the lock timed out: the runtime stopped answering and VR is being abandoned.
	bool XrQueueLockAbandoned();

	// The longest wait for the lock since the last call, in microseconds (resets it).
	u32 TakeXrQueueLockMaxWaitUs();
	// Total time any thread waited for the lock since the last call, in microseconds (frame stats).
	u64 TakeXrQueueLockTotalWaitUs();
}

#endif
