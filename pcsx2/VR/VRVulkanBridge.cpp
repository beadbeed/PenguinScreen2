// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#ifdef ENABLE_VULKAN

#include "VR/VRVulkanBridge.h"
#include "VR/SeatSession.h"
#include "VR/VRInternal.h"
#include "VR/VRManager.h"
#include "VR/XRCompositor.h"
#include "VR/XRSession.h"

#include "common/Assertions.h"
#include "common/Console.h"

#include <atomic>
#include <chrono>
#include <mutex>

namespace VR
{
	namespace
	{
		bool s_bootstrap_active = false;

		// The queue the OpenXR session shares with the GS (ScopedXrQueueLock).
		std::timed_mutex s_xr_queue_mutex;
		std::atomic<bool> s_xr_queue_lock_enabled{false};
		std::atomic<bool> s_xr_queue_lock_abandoned{false};
		std::atomic<u32> s_xr_queue_lock_max_wait_us{0};
		std::atomic<u64> s_xr_queue_lock_total_wait_us{0};
		// This thread holds the lock already, so a nested scope takes nothing (std::timed_mutex is not recursive).
		thread_local bool t_xr_queue_lock_held = false;
		// Long enough to ride out a runtime hiccup (a dashboard or environment load can hold xrEndFrame for a few
		// frames), short enough that the GS (and so the emulation) never waits long on a runtime that stopped answering.
		constexpr auto kXrQueueLockTimeout = std::chrono::milliseconds(500);

		void NoteXrQueueLockWait(u32 us)
		{
			s_xr_queue_lock_total_wait_us.fetch_add(us, std::memory_order_relaxed);
			u32 prev = s_xr_queue_lock_max_wait_us.load(std::memory_order_relaxed);
			while (us > prev &&
				   !s_xr_queue_lock_max_wait_us.compare_exchange_weak(prev, us, std::memory_order_relaxed))
			{
			}
		}

		void AbortBootstrap()
		{
			SeatSession::Stop();
			XRCompositor::Shutdown();
			// A VR thread stuck inside the runtime: calling into it again could hang this thread too.
			if (XRCompositor::Abandoned() || XrQueueLockAbandoned())
			{
				XRSession::ForgetWithoutDestroying();
			}
			else
			{
				XRSession::DestroySession();
				XRSession::DestroyInstance();
			}
			SetXrQueueLockEnabled(false);
			Internal::GetVulkanHandles().Clear();
			s_bootstrap_active = false;
		}
	}

	ScopedXrQueueLock::ScopedXrQueueLock()
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
		NoteXrQueueLockWait((waited_us <= 0) ? 0u : (waited_us >= 0xFFFFFFFFll) ? 0xFFFFFFFFu : static_cast<u32>(waited_us));
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

		if (!XRSession::CreateSessionVK(instance, physical_device, device, queue_family, 0))
		{
			AbortBootstrap();
			return false;
		}

		// The session now shares the GS graphics queue: from here on both sides take the queue lock to use it.
		SetXrQueueLockEnabled(true);

		if (!XRCompositor::Initialize())
		{
			Console.Error("(VR) Compositor initialization failed — running flat.");
			AbortBootstrap();
			return false;
		}

		Console.WriteLn("(VR) VR session active. Put on the headset.");
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
