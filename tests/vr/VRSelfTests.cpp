// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

// vr_selftests: the VR layer's pure self-tests (no OpenXR session, no Vulkan device, no running VM) in one console run,
// for CI. In the emulator they run once with PCSX2_VR_SELFTEST (and its siblings) set and are read from the emulog by
// hand; here each runs once, its own summary line is printed with PASS or FAIL, and the exit code is 0 only when every
// one passed (1 when any failed, 2 when the shipped profiles are missing).
//
// Most of them are public and called directly. Two groups are not, and are reached the way the emulator reaches them,
// with nothing to act on: CameraDriver::Apply with no VM (its first act each vsync is the matrix, hook, predict,
// comfort and FOV self-tests and SpatialControls' Gamepad line, and with no VM it then returns at its "no VM" disarm
// before any game memory or profile is read), and XRCompositor::Initialize with no XR session (the pacer's menu pointer
// and the edge fill's plan self-tests, then its "no XR session" return before any runtime call). Their results are read
// from the lines they log.
//
// Not run here: GSDeviceVK's multiview self-test (PCSX2_VR_MV_SELFTEST, needs a Vulkan device) and the texture cache's
// source guard (PCSX2_VR_SRCGUARD_SELFTEST, frees a draw source in a running GS on purpose).
//
// Usage: vr_selftests [resources directory]   (default: this checkout's bin/resources, for the shipped profiles)

#include "VR/BeltPouches.h"
#include "VR/DepthExport.h"
#include "VR/CameraDriver.h"
#include "VR/GameFeedback.h"
#include "VR/HandModel.h"
#include "VR/HudCards.h"
#include "VR/ItemReach.h"
#include "VR/MenuPanel.h"
#include "VR/MenuPointer.h"
#include "VR/MeshRaster.h"
#include "VR/ReloadGesture.h"
#include "VR/SpatialControls.h"
#include "VR/StereoAuto.h"
#include "VR/VRProfileDB.h"
#include "VR/ZoneMarkers.h"
#include "VR/XRCompositor.h"

#include "Config.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef VR_SELFTESTS_RESOURCES
#define VR_SELFTESTS_RESOURCES ""
#endif

namespace
{
	std::mutex s_log_mutex;
	std::vector<std::string> s_log;

	// Every line the emulator logs (Console, DevCon) is printed and kept, so a self-test that only logs its result can be
	// read back.
	void OnLog(LOGLEVEL, ConsoleColors, std::string_view message)
	{
		std::lock_guard<std::mutex> lock(s_log_mutex);
		s_log.emplace_back(message);
		std::fwrite(message.data(), 1, message.size(), stdout);
		std::fputc('\n', stdout);
		std::fflush(stdout);
	}

	size_t LogMark()
	{
		std::lock_guard<std::mutex> lock(s_log_mutex);
		return s_log.size();
	}

	// The first line logged since mark that starts with prefix, or "" when there is none.
	std::string FindLine(size_t mark, std::string_view prefix)
	{
		std::lock_guard<std::mutex> lock(s_log_mutex);
		for (size_t i = mark; i < s_log.size(); i++)
		{
			if (std::string_view(s_log[i]).starts_with(prefix))
				return s_log[i];
		}
		return std::string();
	}

	// The numbers in text, in order.
	std::vector<int> NumbersIn(std::string_view text)
	{
		std::vector<int> numbers;
		size_t i = 0;
		while (i < text.size())
		{
			if (text[i] < '0' || text[i] > '9')
			{
				i++;
				continue;
			}
			int value = 0;
			while (i < text.size() && text[i] >= '0' && text[i] <= '9')
				value = value * 10 + (text[i++] - '0');
			numbers.push_back(value);
		}
		return numbers;
	}

	struct SuiteResult
	{
		std::string name;
		bool passed = false;
		std::string detail;
	};
	std::vector<SuiteResult> s_results;

	void Begin(const char* what)
	{
		std::printf("\n--- %s\n", what);
		std::fflush(stdout);
	}

	void Record(const char* name, bool passed, std::string detail)
	{
		std::printf("[%s] %s: %s\n", passed ? "PASS" : "FAIL", name, detail.c_str());
		std::fflush(stdout);
		s_results.push_back({name, passed, std::move(detail)});
	}

	// A summary line as the result's detail, without the "(VR) " every VR line starts with.
	std::string Detail(const std::string& line)
	{
		if (line.empty())
			return "its summary line was not logged";
		return line.starts_with("(VR) ") ? line.substr(5) : line;
	}

	// A self-test that returns whether every check passed and logs one summary line starting with summary.
	void RunLogged(const char* name, bool (*test)(), const char* summary)
	{
		Begin(name);
		const size_t mark = LogMark();
		const bool passed = test();
		Record(name, passed, Detail(FindLine(mark, summary)));
	}

	// A self-test that returns whether every check passed and logs nothing.
	void RunSilent(const char* name, bool (*test)())
	{
		Begin(name);
		const bool passed = test();
		Record(name, passed, passed ? "passed (it returns pass or fail only, no check count)" : "FAIL (it names no check)");
	}

	// A self-test that only logs, a line "<summary>N ..." or "<summary>P passed, N failed": it passes when the line was
	// logged and the number at fail_index is 0 (with fail_index 1, after at least one passed check).
	void ReadCountLine(const char* name, size_t mark, const char* summary, size_t fail_index)
	{
		const std::string line = FindLine(mark, summary);
		const std::vector<int> n = line.empty() ? std::vector<int>() : NumbersIn(std::string_view(line).substr(std::string_view(summary).size()));
		const bool passed = n.size() > fail_index && n[fail_index] == 0 && (fail_index == 0 || n[0] > 0);
		Record(name, passed, Detail(line));
	}

	// A self-test that only logs, one line "<summary>: passed ..." or "<summary> FAIL: ...".
	void ReadPassedLine(const char* name, size_t mark, const char* summary)
	{
		const std::string line = FindLine(mark, summary);
		Record(name, line.starts_with(std::string(summary) + ": passed"), Detail(line));
	}

	void RunGamepad()
	{
		Begin("Gamepad (SpatialControls::SelfTestGamepad)");
		const char* failed = nullptr;
		const bool passed = VR::SpatialControls::SelfTestGamepad(&failed);
		Record("Gamepad", passed,
			passed ? std::string("passed (it names the first failed check only, no check count)") :
					 std::string("FAIL: ") + (failed ? failed : "?"));
	}

	void RunAutoStereo()
	{
		Begin("Auto-stereo (StereoAuto::MaybeRunSelfTest, PCSX2_VR_SELFTEST set)");
		const size_t mark = LogMark();
		VR::StereoAuto::MaybeRunSelfTest();
		ReadCountLine("Auto-stereo", mark, "(VR) Auto-stereo self-test: ", 0);
	}

	void RunCameraDriverLogged()
	{
		Begin("CameraDriver's logged self-tests (CameraDriver::Apply with no VM, PCSX2_VR_SELFTEST set)");
		const size_t mark = LogMark();
		VR::CameraDriver::Apply();
		ReadCountLine("CameraDriver matrix", mark, "(VR) CameraDriver matrix self-test: ", 1);
		ReadCountLine("CameraDriver hook", mark, "(VR) CameraDriver hook self-test: ", 0);
		ReadCountLine("CameraDriver predict", mark, "(VR) CameraDriver predict self-test: ", 0);
		ReadCountLine("CameraDriver comfort", mark, "(VR) CameraDriver comfort self-test: ", 0);
		ReadCountLine("CameraDriver FOV", mark, "(VR) CameraDriver FOV self-test: ", 0);
		ReadPassedLine("Gamepad (CameraDriver's run)", mark, "(VR) Gamepad self-test");
	}

#ifdef ENABLE_VULKAN
	void RunCompositorLogged()
	{
		Begin("XRCompositor's self-tests (XRCompositor::Initialize with no XR session, PCSX2_VR_SELFTEST set)");
		const size_t mark = LogMark();
		if (VR::XRCompositor::Initialize())
			VR::XRCompositor::Shutdown(); // not reached: this process has no XR session
		ReadPassedLine("Menu pointer (pacer)", mark, "(VR) Menu pointer (pacer) self-test");
		ReadCountLine("Edge fill plan", mark, "(VR) Edge fill self-test: ", 1);
	}
#endif
} // namespace

int main(int argc, char* argv[])
{
	// The emulator runs the logged self-tests below only with this set; read where they are called, not at startup.
#ifdef _WIN32
	_putenv_s("PCSX2_VR_SELFTEST", "1");
#else
	setenv("PCSX2_VR_SELFTEST", "1", 1);
#endif
	Log::SetHostOutputLevel(LOGLEVEL_DEV, OnLog);

	// The item names and pouch self-tests check the shipped profiles (resources/vr-profiles); no user profile folder.
	EmuFolders::Resources = (argc > 1) ? std::string(argv[1]) : std::string(VR_SELFTESTS_RESOURCES);
	const std::string profiles = Path::Combine(EmuFolders::Resources, "vr-profiles");
	if (!FileSystem::DirectoryExists(profiles.c_str()))
	{
		std::fprintf(stderr, "vr_selftests: no shipped profiles at %s (give the resources directory as the argument).\n",
			profiles.c_str());
		return 2;
	}
	std::printf("vr_selftests: the shipped profiles from %s\n", profiles.c_str());
	std::fflush(stdout);

	RunLogged("MenuPointer", VR::MenuPointer::SelfTest, "(VR) MenuPointer self-test");
	RunLogged("Item names", VR::GameFeedback::ItemSelfTest, "(VR) Item names self-test");
	RunLogged("BeltPouches", VR::BeltPouches::SelfTest, "(VR) BeltPouches self-test");
	RunLogged("HUD", VR::HudCards::SelfTest, "(VR) HUD self-test: ");
	RunLogged("Feedback", VR::GameFeedback::SelfTest, "(VR) Feedback self-test: ");
	RunLogged("Hands", VR::HandModel::SelfTest, "(VR) Hands self-test: ");
	RunLogged("Pickup cue and hand targeting", VR::ItemReach::SelfTest, "(VR) Pickup cue self-test");
	RunLogged("Zone markers", VR::ZoneMarkers::SelfTest, "(VR) Zone markers self-test");
	RunLogged("MeshRaster", +[]() { return VR::MeshRaster::SelfTest() == 0; }, "(VR) MeshRaster self-test");
	{
		Begin("Reload gesture (ReloadGesture::SelfTest)");
		const std::vector<std::string> failed = VR::ReloadGesture::SelfTest();
		Record("Reload gesture", failed.empty(),
			failed.empty() ? std::string("passed (it names the failed checks only, no check count)") :
							 "FAIL: " + std::to_string(failed.size()) + " check(s), first: " + failed.front());
	}
	RunLogged("Depth export", VR::DepthExport::SelfTest, "(VR) Depth export self-test");
	RunLogged("Menu panel", VR::MenuPanel::SelfTest, "(VR) Menu panel self-test");
	// [VR] GameWeaponModels: File #1's profile names the game's weapon-type table and the scenario's item table, with the
	// layout the weapon models are placed by (tools/outbreak-vr/research/2026-10-14-weapon-models-in-hand.md).
	RunSilent("Weapon hold (File #1 profile, feedback.weapon.hold)", +[]() {
		const VR::ProfileDB::Profile* p = VR::ProfileDB::Lookup("SLPM-65428", 0x32088394u);
		if (!p || !p->feedback.has_value())
			return false;
		const VR::ProfileDB::FeedbackParams& f = p->feedback.value();
		return f.has_weapon_hold && f.has_hold_items && f.hold_type_offset == 0xCA8 && f.hold_table == 0x6E5944 &&
		       f.hold_stride == 0x44 && f.hold_grip_offset == 0x0C && f.hold_rotation_offset == 0x24 &&
		       f.hold_muzzle_offset == 0x34 && f.hold_items == 0x716980 && f.hold_item_stride == 0x18 &&
		       f.hold_item_id == 0x12 && f.hold_item_type == 0x0F;
	});
	// [VR] SeamlessDoors: File #1's camera.silence entries gated on the switch (whenSetting), the arrival ones kept
	// through disarm; each NOPs one jal 0x590B40 (tools/outbreak-vr/research/2026-10-14-doors.md).
	RunSilent("Seamless doors (File #1 profile, camera.silence whenSetting)", +[]() {
		const VR::ProfileDB::Profile* p = VR::ProfileDB::Lookup("SLPM-65428", 0x32088394u);
		if (!p || !p->camera.has_value() || !VR::ProfileDB::IsSilenceSettingName("SeamlessDoors") ||
			VR::ProfileDB::IsSilenceSettingName("NoSuchSwitch"))
			return false;
		int gated = 0, kept = 0;
		for (const VR::ProfileDB::CameraSilence& sil : p->camera->silence)
		{
			if (sil.when_setting.empty())
			{
				if (sil.keep_through_disarm)
					return false;
				continue;
			}
			if (sil.when_setting != "SeamlessDoors" || !sil.offline_only || sil.value_off != 0x0C1642D0u ||
				sil.value_on != 0 || sil.when.empty())
				return false;
			gated++;
			if (sil.keep_through_disarm)
			{
				kept++;
				if (sil.ee_address != 0x550494u && sil.ee_address != 0x5504E0u && sil.ee_address != 0x55052Cu)
					return false;
			}
		}
		return gated == 15 && kept == 3;
	});
	RunGamepad();
	RunSilent("CameraDriver::SelfTestAssembler (hook)", VR::CameraDriver::SelfTestAssembler);
	RunSilent("CameraDriver::SelfTestMath (math, predict, comfort, FOV)", VR::CameraDriver::SelfTestMath);
	RunSilent("ProfileDB::SelfTestMultibandResolve", VR::ProfileDB::SelfTestMultibandResolve);
	RunAutoStereo();
	RunCameraDriverLogged();
#ifdef ENABLE_VULKAN
	RunCompositorLogged();
#endif

	size_t failed = 0;
	for (const SuiteResult& r : s_results)
		failed += r.passed ? 0u : 1u;
	std::printf("\n=== VR self-tests: %zu of %zu passed, %zu failed.\n", s_results.size() - failed, s_results.size(), failed);
	for (const SuiteResult& r : s_results)
		std::printf("  %s  %-58s %s\n", r.passed ? "PASS" : "FAIL", r.name.c_str(), r.detail.c_str());

	// Exit without the emulator's static teardown: nothing here started what its frontend would shut down first.
	std::fflush(nullptr);
	std::_Exit(failed == 0 ? 0 : 1);
}
