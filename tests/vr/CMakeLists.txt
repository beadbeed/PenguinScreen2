# vr_selftests: the VR layer's pure self-tests (no OpenXR session, no Vulkan device, no running VM) in one console
# program. It links the emulator core with a do-nothing Host (StubHost.cpp, upstream PCSX2's unit-test host), runs each
# self-test once, prints its summary line and exits non-zero when any fails. The Windows CI build runs it after the
# build (.github/workflows/windows_build_qt.yml, "Run VR Self-Tests"). Not part of the default build:
#   cmake --build build --target vr_selftests
#   build/tests/vr/vr_selftests [bin/resources]
add_executable(vr_selftests EXCLUDE_FROM_ALL
	VRSelfTests.cpp
	StubHost.cpp
)

target_include_directories(vr_selftests PRIVATE
	"${CMAKE_BINARY_DIR}/common/include"
	"${CMAKE_SOURCE_DIR}/pcsx2"
)

target_link_libraries(vr_selftests PRIVATE
	PCSX2_FLAGS
	PCSX2
)

# The shipped profiles the item and pouch self-tests read, when no resources directory is given.
target_compile_definitions(vr_selftests PRIVATE "VR_SELFTESTS_RESOURCES=\"${CMAKE_SOURCE_DIR}/bin/resources\"")

if(MSVC)
	# In the emulator these run on its CPU and GS threads; here all of them run on the main thread.
	target_link_options(vr_selftests PRIVATE /STACK:8388608)
endif()
