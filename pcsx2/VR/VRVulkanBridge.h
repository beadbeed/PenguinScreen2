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

	// Who takes the queue lock, for the Frames line: GSDevice is GSDeviceVK's submits, presents and vkDeviceWaitIdle,
	// VR everything else (the compositor's XR and Vulkan calls on the GS thread and the VR pacer's frame calls).
	enum class XrQueueUser : u8
	{
		VR,
		GSDevice,
	};

	// The queue the OpenXR session uses: by default the GS graphics queue itself (index 0), shared with the GS. With
	// [VR] DedicatedXrQueue a queue of its own in the same family (XrQueueDedicated below). XR_KHR_vulkan_enable2 makes
	// the app synchronise the session's queue against the runtime's own use of it inside xrBeginFrame, xrEndFrame,
	// xrAcquireSwapchainImage and xrReleaseSwapchainImage, which run on the VR pacer thread as well as the GS thread.
	// Each of those XR calls and every vkQueueSubmit the compositor makes on it holds this lock (never xrWaitFrame or
	// xrWaitSwapchainImage), and so, while the queue is shared, does every vkQueueSubmit and vkQueuePresentKHR on the GS
	// side; vkDeviceWaitIdle always does (it uses every queue of the device). Lock order: XRCompositor's submit mutex,
	// then this lock; never the reverse. Bounded: a lock held over 500 ms (the runtime not answering) abandons VR
	// instead of blocking emulation. A scope opened while this thread already holds the lock (the spin submit inside
	// SubmitCommandBuffer) takes nothing.
	class ScopedXrQueueLock
	{
	public:
		explicit ScopedXrQueueLock(XrQueueUser user = XrQueueUser::VR);
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
	// The part of that total waited by threads other than the VR pacer (in practice the GS thread, so the emulation),
	// and the part of it waited by GSDeviceVK (XrQueueUser::GSDevice), in microseconds since the last call.
	u64 TakeXrQueueLockGsThreadWaitUs();
	u64 TakeXrQueueLockGsSubmitWaitUs();
	// Called once by the VR pacer thread as it starts: its waits are not the GS thread's.
	void MarkXrQueueLockPacerThread();

	// [VR] DedicatedXrQueue: true while the session has a queue of its own (from the session's creation until VR is torn
	// down with the GS device). CreateVulkanDeviceThroughXR asks for one queue more in the GS graphics family, with
	// timeline semaphores; when the family has no queue to spare, the device has no timeline semaphores, the device or
	// the session cannot be created that way, the session shares the GS queue as before (the log says why). The
	// runtime's own submits and the compositor's then go to the session's queue, and the GS's submits and presents never
	// touch it, so they take no queue lock: a runtime that blocks inside xrEndFrame holds up the VR threads only.
	//
	// The two queues are ordered by two timeline semaphores, gs and xr, whose values only grow:
	// - every GS submit (GSDeviceVK::SubmitCommandBuffer) signals the next gs value (AddGsSubmitTimeline);
	// - every compositor submit that reads a GS texture (the screen copies and the edge fill, which also move the
	//   texture's layout to TransferSrc) waits for the gs value of the GS's latest submit, and so for all GS work
	//   submitted before it, and signals the next xr value (AddXrReadTimeline);
	// - the first GS submit after such a read waits for its xr value at ALL_COMMANDS (once: a vkQueueSubmit wait also
	//   holds back every later command on its queue), so nothing the GS does to the texture afterwards (drawing into
	//   it, moving its layout, freeing it, which waits for a later GS fence) can overtake the read.
	// Each side waits only for a value the other side has already submitted the signal of (its latest), never for one
	// still to come, so no submit can end up waiting, through the other queue, for itself: no deadlock. A read therefore
	// never waits for GS work that is recorded but not yet submitted: the compositor has the GS submit such work first
	// (XRCompositor, GSDeviceVK::VRMayHaveUnsubmittedWork). Both queues are of one family and GS textures are
	// VK_SHARING_MODE_EXCLUSIVE: within a family no queue family ownership transfer is needed, the semaphores carry the
	// execution and memory dependency, and a layout transition recorded on one queue holds for the other. The
	// compositor's own uploads (hands, HUD cards, lever cards, the background) touch no GS texture: no wait, no signal.
	bool XrQueueDedicated();

	// GSDeviceVK::SubmitCommandBuffer, GS thread, with a dedicated XR queue: copies si's waits and signals into t (which
	// must outlive the vkQueueSubmit), adds the wait for the compositor's latest read of GS textures not yet waited for
	// and the signal of the next gs value, and points si at them. False (si untouched) while the queue is shared.
	struct GsSubmitTimeline
	{
		static constexpr u32 kMaxSemaphores = 4;
		VkSemaphore waits[kMaxSemaphores];
		VkPipelineStageFlags wait_stages[kMaxSemaphores];
		u64 wait_values[kMaxSemaphores];
		VkSemaphore signals[kMaxSemaphores];
		u64 signal_values[kMaxSemaphores];
		VkTimelineSemaphoreSubmitInfo info;
		u64 gs_value;
		u64 xr_value; // the xr value this submit waits for, 0 = none
	};
	bool AddGsSubmitTimeline(VkSubmitInfo* si, GsSubmitTimeline* t);
	// Once that vkQueueSubmit returned VK_SUCCESS: the compositor's next read waits for its gs value.
	void GsSubmitTimelineDone(const GsSubmitTimeline& t);

	// [VR] PacerCopies (XRCompositor): the gs value the GS's next submit will signal, so work recorded into the GS's
	// current command buffer now is known done on the GPU once the gs timeline reaches it (GS thread only); and the gs
	// value of the GS's latest submit that went in (any thread), which a read of that work must not get ahead of.
	u64 NextGsSubmitValue();
	u64 GsSubmittedValue();

	// XRCompositor, under the queue lock, for a submit on the session's queue that reads GS textures, or with
	// [VR] PacerCopies images the GS wrote in its own command buffers (si has no waits or signals of its own): with a
	// dedicated XR queue, makes it wait for the GS's latest submit and signal the next xr value. The GS thread makes these
	// submits, or with PacerCopies the VR pacer, never both in one session; the queue lock orders the values either way.
	// False (si untouched) while the queue is shared.
	struct XrReadTimeline
	{
		VkSemaphore wait;
		VkPipelineStageFlags wait_stage;
		u64 wait_value;
		VkSemaphore signal;
		u64 signal_value;
		VkTimelineSemaphoreSubmitInfo info;
	};
	bool AddXrReadTimeline(VkSubmitInfo* si, XrReadTimeline* t);
	// Once that vkQueueSubmit returned VK_SUCCESS: the GS's next submit waits for its xr value.
	void XrReadTimelineDone(const XrReadTimeline& t);
}

#endif
