// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#ifdef ENABLE_VULKAN

#include "VR/VRVulkanBridge.h"
#include "VR/GameModels.h"
#include "VR/SeatSession.h"
#include "VR/VRInternal.h"
#include "VR/VRManager.h"
#include "VR/XRCompositor.h"
#include "VR/XRSession.h"

#include "Config.h"

#include "common/Assertions.h"
#include "common/Console.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace VR
{
	namespace
	{
		bool s_bootstrap_active = false;

		// The queue the OpenXR session uses, shared with the GS unless it has one of its own (ScopedXrQueueLock).
		std::timed_mutex s_xr_queue_mutex;
		std::atomic<bool> s_xr_queue_lock_enabled{false};
		std::atomic<bool> s_xr_queue_lock_abandoned{false};
		std::atomic<u32> s_xr_queue_lock_max_wait_us{0};
		std::atomic<u64> s_xr_queue_lock_total_wait_us{0};
		std::atomic<u64> s_xr_queue_lock_gs_thread_wait_us{0};
		std::atomic<u64> s_xr_queue_lock_gs_submit_wait_us{0};
		// This thread holds the lock already, so a nested scope takes nothing (std::timed_mutex is not recursive).
		thread_local bool t_xr_queue_lock_held = false;
		// The VR pacer (MarkXrQueueLockPacerThread): its waits are not the emulation's.
		thread_local bool t_xr_queue_lock_pacer = false;
		// Long enough to ride out a runtime hiccup (a dashboard or environment load can hold xrEndFrame for a few
		// frames), short enough that the GS (and so the emulation) never waits long on a runtime that stopped answering.
		constexpr auto kXrQueueLockTimeout = std::chrono::milliseconds(500);

		// [VR] DedicatedXrQueue (XrQueueDedicated in the header). What CreateVulkanDeviceThroughXR got for the session's
		// queue, for OnVulkanDeviceCreated: whether the device was created with one more queue in the GS graphics
		// family (and timeline semaphores), which, and otherwise why not. GS thread only.
		struct XrQueuePlan
		{
			bool device_has_queue = false;
			u32 family = 0;
			u32 index = 0;
			char shared_reason[160] = {};
		};
		XrQueuePlan s_xr_queue_plan;

		// True while the session is on its own queue and the GS's submits signal and wait for the timelines. Only the GS
		// thread changes it (OnVulkanDeviceCreated, AbortBootstrap), so a GS submit sees one value throughout.
		std::atomic<bool> s_xr_queue_dedicated{false};
		VkSemaphore s_gs_timeline = VK_NULL_HANDLE;
		VkSemaphore s_xr_timeline = VK_NULL_HANDLE;
		// The gs value the next GS submit signals is s_gs_next + 1 (GS thread only); s_gs_submitted is the value of the
		// latest GS submit that went in, which a read waits for.
		u64 s_gs_next = 0;
		std::atomic<u64> s_gs_submitted{0};
		// The same for xr (the reads are submitted on the GS thread, under the queue lock), and the latest xr value a GS
		// submit has waited for (GS thread only).
		u64 s_xr_next = 0;
		std::atomic<u64> s_xr_read_submitted{0};
		u64 s_gs_waited_xr = 0;

		void NoteXrQueueLockWait(u32 us, XrQueueUser user)
		{
			s_xr_queue_lock_total_wait_us.fetch_add(us, std::memory_order_relaxed);
			if (!t_xr_queue_lock_pacer)
				s_xr_queue_lock_gs_thread_wait_us.fetch_add(us, std::memory_order_relaxed);
			if (user == XrQueueUser::GSDevice)
				s_xr_queue_lock_gs_submit_wait_us.fetch_add(us, std::memory_order_relaxed);
			u32 prev = s_xr_queue_lock_max_wait_us.load(std::memory_order_relaxed);
			while (us > prev &&
				   !s_xr_queue_lock_max_wait_us.compare_exchange_weak(prev, us, std::memory_order_relaxed))
			{
			}
		}

		void SetSharedReason(const char* reason)
		{
			std::snprintf(s_xr_queue_plan.shared_reason, sizeof(s_xr_queue_plan.shared_reason), "%s", reason);
		}

		// The GS's device request with one queue more in its graphics family and timeline semaphores on. Holds
		// everything the copied VkDeviceCreateInfo points at, so it is built in place and never copied.
		struct DedicatedQueueRequest
		{
			VkDeviceCreateInfo ci = {};
			std::vector<VkDeviceQueueCreateInfo> queues;
			std::vector<float> priorities;
			std::vector<const char*> extensions;
			VkPhysicalDeviceTimelineSemaphoreFeatures timeline_features = {};
			u32 family = 0;
			u32 index = 0;
		};

		// Fills req from the GS's own request ci; false (with why) when the device cannot give the session a queue of
		// its own, which leaves the session on the GS queue.
		bool PlanDedicatedXrQueue(VkPhysicalDevice physical_device, const VkDeviceCreateInfo& ci, DedicatedQueueRequest* req)
		{
			// GSDeviceVK puts its graphics family first and takes m_graphics_queue as its queue 0 (CreateDevice).
			if (ci.queueCreateInfoCount == 0 || !ci.pQueueCreateInfos)
			{
				SetSharedReason("the GS asked for no queue");
				return false;
			}
			const VkDeviceQueueCreateInfo& gs_queue = ci.pQueueCreateInfos[0];
			const u32 family = gs_queue.queueFamilyIndex;
			u32 family_count = 0;
			vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, nullptr);
			std::vector<VkQueueFamilyProperties> families(family_count);
			vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, families.data());
			if (family >= family_count || !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
			{
				std::snprintf(s_xr_queue_plan.shared_reason, sizeof(s_xr_queue_plan.shared_reason),
					"the GS's first queue family (%u) is not a graphics family", family);
				return false;
			}
			if (families[family].queueCount <= gs_queue.queueCount)
			{
				std::snprintf(s_xr_queue_plan.shared_reason, sizeof(s_xr_queue_plan.shared_reason),
					"graphics family %u has %u queue(s) and the GS uses %u", family, families[family].queueCount,
					gs_queue.queueCount);
				return false;
			}

			// Timeline semaphores through VK_KHR_timeline_semaphore: core only from Vulkan 1.2, and the GS creates its
			// instance for 1.1, which caps the device's API version; drivers with 1.2 still offer the extension.
			u32 ext_count = 0;
			std::vector<VkExtensionProperties> exts;
			if (vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &ext_count, nullptr) == VK_SUCCESS &&
				ext_count > 0)
			{
				exts.resize(ext_count);
				if (vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &ext_count, exts.data()) != VK_SUCCESS)
					exts.clear();
				else
					exts.resize(ext_count);
			}
			if (std::none_of(exts.begin(), exts.end(), [](const VkExtensionProperties& e) {
					return std::strcmp(e.extensionName, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME) == 0;
				}))
			{
				SetSharedReason("the driver does not offer " VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
				return false;
			}
			VkPhysicalDeviceTimelineSemaphoreFeatures query = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
			VkPhysicalDeviceFeatures2 features2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &query};
			vkGetPhysicalDeviceFeatures2(physical_device, &features2);
			if (query.timelineSemaphore != VK_TRUE)
			{
				SetSharedReason("the device has no timeline semaphores");
				return false;
			}

			// A feature struct that already carries timeline semaphores must not be added twice (and the 1.2 features
			// struct must not be joined by the timeline one at all): use it as it is, or give up if it turns them off.
			bool chain_has_timeline = false;
			for (const VkBaseInStructure* p = static_cast<const VkBaseInStructure*>(ci.pNext); p; p = p->pNext)
			{
				VkBool32 on = VK_TRUE;
				if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES)
					on = reinterpret_cast<const VkPhysicalDeviceTimelineSemaphoreFeatures*>(p)->timelineSemaphore;
				else if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)
					on = reinterpret_cast<const VkPhysicalDeviceVulkan12Features*>(p)->timelineSemaphore;
				else
					continue;
				if (on != VK_TRUE)
				{
					SetSharedReason("the GS's device request turns timeline semaphores off");
					return false;
				}
				chain_has_timeline = true;
			}

			req->queues.assign(ci.pQueueCreateInfos, ci.pQueueCreateInfos + ci.queueCreateInfoCount);
			req->priorities.assign(gs_queue.pQueuePriorities, gs_queue.pQueuePriorities + gs_queue.queueCount);
			// As high as the GS's own: the runtime's frame submits and the screen copies are what the headset waits for.
			req->priorities.push_back(1.0f);
			req->queues[0].queueCount = gs_queue.queueCount + 1;
			req->queues[0].pQueuePriorities = req->priorities.data();

			if (ci.enabledExtensionCount > 0 && ci.ppEnabledExtensionNames)
				req->extensions.assign(ci.ppEnabledExtensionNames, ci.ppEnabledExtensionNames + ci.enabledExtensionCount);
			if (std::none_of(req->extensions.begin(), req->extensions.end(), [](const char* name) {
					return std::strcmp(name, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME) == 0;
				}))
			{
				req->extensions.push_back(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
			}

			req->ci = ci;
			req->ci.queueCreateInfoCount = static_cast<u32>(req->queues.size());
			req->ci.pQueueCreateInfos = req->queues.data();
			req->ci.enabledExtensionCount = static_cast<u32>(req->extensions.size());
			req->ci.ppEnabledExtensionNames = req->extensions.data();
			if (!chain_has_timeline)
			{
				// In front of the GS's own chain, which stays as it is.
				req->timeline_features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
				req->timeline_features.pNext = const_cast<void*>(ci.pNext);
				req->timeline_features.timelineSemaphore = VK_TRUE;
				req->ci.pNext = &req->timeline_features;
			}
			req->family = family;
			req->index = gs_queue.queueCount;
			return true;
		}

		void DestroyXrQueueTimelines(VkDevice device)
		{
			if (device != VK_NULL_HANDLE)
			{
				if (s_gs_timeline != VK_NULL_HANDLE)
					vkDestroySemaphore(device, s_gs_timeline, nullptr);
				if (s_xr_timeline != VK_NULL_HANDLE)
					vkDestroySemaphore(device, s_xr_timeline, nullptr);
			}
			s_gs_timeline = VK_NULL_HANDLE;
			s_xr_timeline = VK_NULL_HANDLE;
		}

		bool CreateXrQueueTimelines(VkDevice device)
		{
			VkSemaphoreTypeCreateInfo type_ci = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
			type_ci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
			type_ci.initialValue = 0;
			const VkSemaphoreCreateInfo ci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type_ci, 0};
			VkSemaphore gs = VK_NULL_HANDLE;
			VkSemaphore xr = VK_NULL_HANDLE;
			if (vkCreateSemaphore(device, &ci, nullptr, &gs) != VK_SUCCESS)
				return false;
			if (vkCreateSemaphore(device, &ci, nullptr, &xr) != VK_SUCCESS)
			{
				vkDestroySemaphore(device, gs, nullptr);
				return false;
			}
			s_gs_timeline = gs;
			s_xr_timeline = xr;
			s_gs_next = 0;
			s_gs_submitted.store(0, std::memory_order_relaxed);
			s_xr_next = 0;
			s_xr_read_submitted.store(0, std::memory_order_relaxed);
			s_gs_waited_xr = 0;
			return true;
		}

		void AbortBootstrap()
		{
			SeatSession::Stop();
			XRCompositor::Shutdown();
			// A VR thread stuck inside the runtime: calling into it again could hang this thread too.
			const bool abandoned = XRCompositor::Abandoned() || XrQueueLockAbandoned();
			if (abandoned)
			{
				XRSession::ForgetWithoutDestroying();
			}
			else
			{
				XRSession::DestroySession();
				XRSession::DestroyInstance();
			}
			SetXrQueueLockEnabled(false);
			// The GS's next submits neither signal nor wait for the timelines. Nothing still uses them when VR fails to
			// start (no GS submit since they were made) or goes with the GS device (after its WaitForGPUIdle); with a
			// VR thread stuck in the runtime the session's queue may still hold work, so they are left alive.
			s_xr_queue_dedicated.store(false, std::memory_order_release);
			if (abandoned)
			{
				s_gs_timeline = VK_NULL_HANDLE;
				s_xr_timeline = VK_NULL_HANDLE;
			}
			else
			{
				DestroyXrQueueTimelines(Internal::GetVulkanHandles().device);
			}
			s_xr_queue_plan = XrQueuePlan();
			Internal::GetVulkanHandles().Clear();
			s_bootstrap_active = false;
		}
	}

	ScopedXrQueueLock::ScopedXrQueueLock(XrQueueUser user)
	{
		if (t_xr_queue_lock_held)
			return;
		if (!s_xr_queue_lock_enabled.load(std::memory_order_acquire) ||
			s_xr_queue_lock_abandoned.load(std::memory_order_acquire))
		{
			return;
		}

		const auto start = std::chrono::steady_clock::now();
		const bool locked = s_xr_queue_mutex.try_lock_for(kXrQueueLockTimeout);
		const long long waited_us =
			std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
		NoteXrQueueLockWait((waited_us <= 0) ? 0u : (waited_us >= 0xFFFFFFFFll) ? 0xFFFFFFFFu : static_cast<u32>(waited_us),
			user);
		if (locked)
		{
			m_locked = true;
			t_xr_queue_lock_held = true;
			return;
		}

		// Whoever holds it is stuck inside the runtime. Go on unlocked: emulation must not wait for it.
		if (!s_xr_queue_lock_abandoned.exchange(true, std::memory_order_acq_rel))
		{
			Console.Error("(VR) Vulkan queue lock held over 500 ms (runtime not answering): abandoning VR, emulation continues.");
			XRSession::MarkLost();
		}
	}

	ScopedXrQueueLock::~ScopedXrQueueLock()
	{
		if (!m_locked)
			return;
		t_xr_queue_lock_held = false;
		s_xr_queue_mutex.unlock();
	}

	void SetXrQueueLockEnabled(bool enabled)
	{
		if (enabled)
		{
			s_xr_queue_lock_abandoned.store(false, std::memory_order_release);
			s_xr_queue_lock_max_wait_us.store(0, std::memory_order_relaxed);
		}
		s_xr_queue_lock_enabled.store(enabled, std::memory_order_release);
	}

	bool XrQueueLockAbandoned()
	{
		return s_xr_queue_lock_abandoned.load(std::memory_order_acquire);
	}

	u32 TakeXrQueueLockMaxWaitUs()
	{
		return s_xr_queue_lock_max_wait_us.exchange(0, std::memory_order_relaxed);
	}

	u64 TakeXrQueueLockTotalWaitUs()
	{
		return s_xr_queue_lock_total_wait_us.exchange(0, std::memory_order_relaxed);
	}

	u64 TakeXrQueueLockGsThreadWaitUs()
	{
		return s_xr_queue_lock_gs_thread_wait_us.exchange(0, std::memory_order_relaxed);
	}

	u64 TakeXrQueueLockGsSubmitWaitUs()
	{
		return s_xr_queue_lock_gs_submit_wait_us.exchange(0, std::memory_order_relaxed);
	}

	void MarkXrQueueLockPacerThread()
	{
		t_xr_queue_lock_pacer = true;
	}

	bool XrQueueDedicated()
	{
		return s_xr_queue_dedicated.load(std::memory_order_acquire);
	}

	bool AddGsSubmitTimeline(VkSubmitInfo* si, GsSubmitTimeline* t)
	{
		if (!XrQueueDedicated())
			return false;

		// The GS's own semaphores (the swap chain's binary ones, the spin semaphore) keep their places; their values in
		// the timeline struct are ignored.
		constexpr u32 kMax = GsSubmitTimeline::kMaxSemaphores;
		pxAssertRel(si->waitSemaphoreCount < kMax && si->signalSemaphoreCount < kMax, "GS submit has too many semaphores");
		u32 waits = 0;
		for (u32 i = 0; i < si->waitSemaphoreCount; i++, waits++)
		{
			t->waits[waits] = si->pWaitSemaphores[i];
			t->wait_stages[waits] = si->pWaitDstStageMask[i];
			t->wait_values[waits] = 0;
		}
		// The compositor's latest read of GS textures, if no GS submit has waited for it yet. Everything in this submit
		// waits (its init buffer's uploads too): any of it may draw into, transition or free a texture that read uses.
		t->xr_value = 0;
		const u64 xr = s_xr_read_submitted.load(std::memory_order_acquire);
		if (xr > s_gs_waited_xr)
		{
			t->waits[waits] = s_xr_timeline;
			t->wait_stages[waits] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
			t->wait_values[waits] = xr;
			t->xr_value = xr;
			waits++;
		}
		u32 signals = 0;
		for (u32 i = 0; i < si->signalSemaphoreCount; i++, signals++)
		{
			t->signals[signals] = si->pSignalSemaphores[i];
			t->signal_values[signals] = 0;
		}
		// Every submit signals, so a read can always wait for exactly the work submitted before it. A value is never
		// used twice even when its submit fails (a timeline signal must exceed the semaphore's current value).
		t->gs_value = ++s_gs_next;
		t->signals[signals] = s_gs_timeline;
		t->signal_values[signals] = t->gs_value;
		signals++;

		t->info = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
		t->info.pNext = si->pNext;
		t->info.waitSemaphoreValueCount = waits;
		t->info.pWaitSemaphoreValues = t->wait_values;
		t->info.signalSemaphoreValueCount = signals;
		t->info.pSignalSemaphoreValues = t->signal_values;
		si->pNext = &t->info;
		si->waitSemaphoreCount = waits;
		si->pWaitSemaphores = t->waits;
		si->pWaitDstStageMask = t->wait_stages;
		si->signalSemaphoreCount = signals;
		si->pSignalSemaphores = t->signals;
		return true;
	}

	void GsSubmitTimelineDone(const GsSubmitTimeline& t)
	{
		s_gs_submitted.store(t.gs_value, std::memory_order_release);
		if (t.xr_value > s_gs_waited_xr)
			s_gs_waited_xr = t.xr_value;
	}

	u64 NextGsSubmitValue()
	{
		return s_gs_next + 1;
	}

	u64 GsSubmittedValue()
	{
		return s_gs_submitted.load(std::memory_order_acquire);
	}

	bool AddXrReadTimeline(VkSubmitInfo* si, XrReadTimeline* t)
	{
		if (!XrQueueDedicated())
			return false;

		pxAssertRel(si->waitSemaphoreCount == 0 && si->signalSemaphoreCount == 0, "XR read submit has its own semaphores");
		// The GS's latest submit, which also covers every GS submit before it (a semaphore signal follows all earlier
		// work on its queue). Value 0 (no GS submit yet) is already reached. The barrier the read records first (the
		// texture to TransferSrc) waits too, so ALL_COMMANDS.
		t->wait = s_gs_timeline;
		t->wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		t->wait_value = s_gs_submitted.load(std::memory_order_acquire);
		t->signal = s_xr_timeline;
		t->signal_value = ++s_xr_next;

		t->info = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
		t->info.pNext = si->pNext;
		t->info.waitSemaphoreValueCount = 1;
		t->info.pWaitSemaphoreValues = &t->wait_value;
		t->info.signalSemaphoreValueCount = 1;
		t->info.pSignalSemaphoreValues = &t->signal_value;
		si->pNext = &t->info;
		si->waitSemaphoreCount = 1;
		si->pWaitSemaphores = &t->wait;
		si->pWaitDstStageMask = &t->wait_stage;
		si->signalSemaphoreCount = 1;
		si->pSignalSemaphores = &t->signal;
		return true;
	}

	void XrReadTimelineDone(const XrReadTimeline& t)
	{
		s_xr_read_submitted.store(t.signal_value, std::memory_order_release);
	}

	namespace Internal
	{
		VulkanHandles& GetVulkanHandles()
		{
			static VulkanHandles s_handles;
			return s_handles;
		}
	}

	bool BeginVulkanBootstrap()
	{
		pxAssertRel(!s_bootstrap_active, "VR Vulkan bootstrap started twice");

		if (!WantsVR())
			return false;

		Console.WriteLn("(VR) VR is enabled — attempting OpenXR bootstrap for Vulkan device creation.");

		if (!XRSession::CreateInstanceAndSystem())
			return false;

		if (!XRSession::QueryVulkanGraphicsRequirements())
		{
			XRSession::DestroyInstance();
			return false;
		}

		s_bootstrap_active = true;
		return true;
	}

	bool VulkanBootstrapActive()
	{
		return s_bootstrap_active && XRSession::HasInstance();
	}

	bool CreateVulkanInstanceThroughXR(const VkInstanceCreateInfo* ci, VkInstance* out)
	{
		if (!XRSession::CreateVulkanInstanceThroughXR(ci, out))
		{
			Console.Error("(VR) Vulkan instance creation through OpenXR failed — running flat.");
			AbortBootstrap();
			return false;
		}

		return true;
	}

	VkPhysicalDevice GetXrVulkanPhysicalDevice(VkInstance instance)
	{
		const VkPhysicalDevice pd = XRSession::GetVulkanGraphicsDevice(instance);
		if (pd == VK_NULL_HANDLE)
		{
			Console.Error("(VR) Runtime did not provide a Vulkan physical device — running flat.");
			AbortBootstrap();
		}

		return pd;
	}

	bool CreateVulkanDeviceThroughXR(VkPhysicalDevice physical_device, const VkDeviceCreateInfo* ci, VkDevice* out)
	{
		// [VR] DedicatedXrQueue: the GS's request with one queue more for the session; anything in the way, or a device
		// that cannot be created like that, and the GS's own request goes through untouched (the session then shares
		// the GS queue).
		s_xr_queue_plan = XrQueuePlan();
		if (!EmuConfig.VR.DedicatedXrQueue)
		{
			SetSharedReason("[VR] DedicatedXrQueue off");
		}
		else
		{
			DedicatedQueueRequest req;
			if (PlanDedicatedXrQueue(physical_device, *ci, &req))
			{
				if (XRSession::CreateVulkanDeviceThroughXR(physical_device, &req.ci, out))
				{
					s_xr_queue_plan.device_has_queue = true;
					s_xr_queue_plan.family = req.family;
					s_xr_queue_plan.index = req.index;
					return true;
				}
				Console.Warning("(VR) XR queue: device creation with a queue of its own failed; trying once more with the "
								"GS's own request.");
				SetSharedReason("the device could not be created with a queue of its own");
			}
		}

		if (!XRSession::CreateVulkanDeviceThroughXR(physical_device, ci, out))
		{
			Console.Error("(VR) Vulkan device creation through OpenXR failed — running flat.");
			AbortBootstrap();
			return false;
		}

		return true;
	}

	bool OnVulkanDeviceCreated(VkInstance instance, VkPhysicalDevice physical_device, VkDevice device,
		u32 queue_family, VkQueue queue)
	{
		if (!VulkanBootstrapActive())
			return false;

		Internal::VulkanHandles& handles = Internal::GetVulkanHandles();
		handles.instance = instance;
		handles.physical_device = physical_device;
		handles.device = device;
		handles.queue = queue;
		handles.queue_family = queue_family;

		// [VR] DedicatedXrQueue: the queue the device was created with for the session, and the timeline semaphores that
		// order it against the GS queue. Anything missing and the session goes on the GS queue (index 0).
		u32 xr_queue_index = 0;
		VkQueue xr_queue = queue;
		if (s_xr_queue_plan.device_has_queue)
		{
			if (s_xr_queue_plan.family != queue_family)
			{
				SetSharedReason("the GS took its graphics queue from another family");
			}
			else if (!CreateXrQueueTimelines(device))
			{
				SetSharedReason("timeline semaphore creation failed");
			}
			else
			{
				VkQueue own = VK_NULL_HANDLE;
				vkGetDeviceQueue(device, queue_family, s_xr_queue_plan.index, &own);
				if (own != VK_NULL_HANDLE)
				{
					xr_queue = own;
					xr_queue_index = s_xr_queue_plan.index;
				}
				else
				{
					DestroyXrQueueTimelines(device);
					SetSharedReason("vkGetDeviceQueue gave no queue");
				}
			}
		}

		bool session = XRSession::CreateSessionVK(instance, physical_device, device, queue_family, xr_queue_index);
		if (!session && xr_queue_index != 0)
		{
			Console.Warning("(VR) XR queue: the session could not be created on a queue of its own; trying once more on the "
							"GS queue.");
			DestroyXrQueueTimelines(device);
			SetSharedReason("the runtime refused a session on a queue of its own");
			xr_queue_index = 0;
			xr_queue = queue;
			session = XRSession::CreateSessionVK(instance, physical_device, device, queue_family, 0);
		}
		if (!session)
		{
			AbortBootstrap();
			return false;
		}

		handles.queue = xr_queue;
		if (xr_queue_index != 0)
		{
			Console.WriteLn("(VR) XR queue: dedicated (family %u, index %u); the GS queue no longer waits on the runtime.",
				queue_family, xr_queue_index);
		}
		else
		{
			Console.WriteLn("(VR) XR queue: shared with the GS (%s).", s_xr_queue_plan.shared_reason);
		}

		// Shared, the session now uses the GS graphics queue: from here on both sides take the queue lock to use it.
		// Dedicated, only the session's users take it (the compositor on the GS thread and the VR pacer, and the GS's
		// vkDeviceWaitIdle), and from here on every GS submit signals the gs timeline: before the compositor starts, so
		// its first read finds every GS submit covered.
		SetXrQueueLockEnabled(true);
		s_xr_queue_dedicated.store(xr_queue_index != 0, std::memory_order_release);

		if (!XRCompositor::Initialize())
		{
			Console.Error("(VR) Compositor initialization failed — running flat.");
			AbortBootstrap();
			return false;
		}

		Console.WriteLn("(VR) VR session active. Put on the headset.");
		GameModels::StartLoad();
		return true;
	}

	void OnGSDeviceDestroyed()
	{
		if (!s_bootstrap_active && !XRSession::HasInstance())
			return;

		Console.WriteLn("(VR) GS device destroyed — tearing down VR.");
		AbortBootstrap();
	}
}

#endif
