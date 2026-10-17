cmake_minimum_required(VERSION 3.16)
project(YtLegacyPSP C ASM)

set(PSP_TITLE "YT Legacy PSP")
set(PSP_VERSION "1.0.0")

# The target name and the output directory must not differ only by case:
# "YtLegacyPSP" vs "YTLegacyPSP" collided and made CMake delete the freshly
# linked ELF. The package directory is named explicitly below.
set(PSP_PKG_DIR "${CMAKE_BINARY_DIR}/ytpsp-eboot")

add_executable(YtLegacyPSP
	src/main.c
	src/util/log.c
	src/util/config.c
	src/util/input.c
	src/compat/av_imports.S
	src/net/http.c
	src/net/json.c
	src/net/invidious.c
	src/ui/gui.c
	src/ui/keyboard.c
	src/media/mp4demux.c
	src/media/video.c
	src/media/audio.c
	src/media/download.c
	src/media/player.c
)

target_include_directories(YtLegacyPSP PRIVATE src)

# Keep the linked executable outside the package directory so the two can
# never be confused for one another by the build tools.
set_target_properties(YtLegacyPSP PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# prxspecs supplies the crt0_prx startfile that defines module_start, which
# prxexports.o references.
#
# The SDK's linkfile.prx starts the image at address 0, which leaves the ELF
# marked ET_EXEC with its segments at vaddr 0. Loaders then try to place it at
# address 0, outside user memory (0x08800000-0x0A000000), and the load fails
# with SCE_KERNEL_ERROR_MEMBLOCK_ALLOC_FAILED. Starting the image at the
# conventional module base puts the segments inside user memory, where the
# loader's AllocAt succeeds.
set(PSP_LINKFILE "${CMAKE_BINARY_DIR}/psplink.ld")
file(READ "${PSPDEV}/psp/sdk/lib/linkfile.prx" _psp_linkfile)
string(REGEX REPLACE
	"PROVIDE \\(__executable_start = 0x0\\); \\. = 0x0;"
	"PROVIDE (__executable_start = 0x08804000); . = 0x08804000;"
	_psp_linkfile "${_psp_linkfile}")
file(WRITE "${PSP_LINKFILE}" "${_psp_linkfile}")

target_link_options(YtLegacyPSP PRIVATE
	-specs=${PSPDEV}/psp/sdk/lib/prxspecs
	-Wl,-q
	-T${PSP_LINKFILE}
	${PSPDEV}/psp/sdk/lib/prxexports.o
	-Wl,-zmax-page-size=128
)

# Library order mirrors the SDK's own build.mak: only the stub archives the
# gcc driver's SPECS does NOT already append, in the same relative order.
# The driver appends, after our list:
#
#     -lm --start-group -lpthreadglue -lpthread -lcglue -lc --end-group
#     -lpsputility -lpsprtc -lpspnet_inet -lpspnet_resolver -lpspsdk
#     -lpspmodinfo -lpspuser -lpspkernel
#
# and that placement matters: the newlib/libcglue objects pulled by the group
# add ThreadMan/IoFileMgr/Stdio/Kernel_Library stub references, which must be
# resolved by the single later -lpspuser scan. If pspuser (or libc) is listed
# here as well, the archive gets scanned twice and its stubs end up in two
# non-contiguous runs -- psp-fixup-imports then reports "stubs out of order",
# leaves the second run's NID pointers unset, and every one of those imports
# resolves to module 255 at boot, killing malloc/stdio/threads immediately.
#
# libpspkernel and libpspiofilemgr stay absent: the kernel lib's sysclib stubs
# redefine strtol, colliding with libc, and there is no libpspiofilemgr --
# sceIoOpen/sceIoWrite live in libpspuser.
#
# libpspgu is replaced by src/compat/gu/libpspgu_fixed.a, which is the upstream
# PSPSDK source rebuilt. The copy shipped in the SDK includes pspintrman.h, so
# sceGuStart/sceGuFinishId resolve sceKernelCpuSuspendIntr/ResumeIntr against
# usersystemlib.prx. The XMB does not load that kernel-mode module for an
# EBOOT.PBP, so a real console rejects the module with
# SCE_KERNEL_ERROR_LIBRARY_NOTFOUND (0x8002013C) before main() runs. The
# upstream sources include pspuser.h instead, so those two functions come from
# a module the firmware does load. An emulator does not care either way,
# because it resolves imports by NID alone.
target_link_libraries(YtLegacyPSP
	${CMAKE_SOURCE_DIR}/src/compat/gu/libpspgu_fixed.a
	pspdisplay
	pspge
	pspctrl
	pspnet
	pspnet_apctl
	pspaudio
	pspaudiolib
	pspaudiocodec
)

# cooleyesBridge.prx: the small kernel module that boots the Media Engine in
# video mode (see src/media/video.h). Built from third_party/ source in the
# build tree and shipped next to the EBOOT; the app loads it at run time.
set(BRIDGE_SRC "${CMAKE_SOURCE_DIR}/third_party/cooleyesBridge")
set(BRIDGE_BUILD "${CMAKE_BINARY_DIR}/cooleyesBridge")
add_custom_command(
	OUTPUT "${BRIDGE_BUILD}/src/cooleyesBridge.prx"
	COMMAND ${CMAKE_COMMAND} -E copy_directory "${BRIDGE_SRC}" "${BRIDGE_BUILD}"
	COMMAND make -C "${BRIDGE_BUILD}/src" "PSPSDK=${PSPDEV}/psp/sdk"
	        "PATH=${PSPDEV}/bin:$ENV{PATH}"
	DEPENDS "${BRIDGE_SRC}/src/main.c" "${BRIDGE_SRC}/src/cooleyesBridge.exp"
	        "${BRIDGE_SRC}/src/sceMeCore_driver3xx.S"
	        "${BRIDGE_SRC}/src/sceAudio_driver3xx.S"
	COMMENT "Building cooleyesBridge.prx"
)
add_custom_target(cooleyesBridge ALL
	DEPENDS "${BRIDGE_BUILD}/src/cooleyesBridge.prx")
add_dependencies(YtLegacyPSP cooleyesBridge)

# PARAM.SFO and the EBOOT.PBP are produced here rather than by
# create_pbp_file:
#
#  - The EBOOT.PBP header is written by tools/mkeboot.py, because the SDK's
#    pack-pbp points every slot from 0x0C to 0x24 at the same offset. The
#    XMB therefore reads a zero-length PARAM.SFO and lists the EBOOT as
#    corrupt data, and PPSSPP's kernel HLE reads a zero-length executable
#    ("Truncated ELF header, 0 bytes"). See that script for the layout.
#  - PARAM.SFO keeps the SDK's own values (BOOTABLE=1, REGION=0x8000) -- the
#    XMB needs those to list the game at all. MEMSIZE stays at the SDK's
#    default of 1; the real memory ceiling is PSP_HEAP_SIZE_KB in main.c.
#
# The linked ELF also needs psp-fixup-imports run over it before packaging:
# that step used to come from create_pbp_file and writes the per-library
# NID/func pointers that the kernel loader resolves at boot.
add_custom_command(
	TARGET YtLegacyPSP
	POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E make_directory "${PSP_PKG_DIR}"
	COMMENT "Creating ${PSP_PKG_DIR}"
)

add_custom_command(
	TARGET YtLegacyPSP
	POST_BUILD
	COMMAND ${PSPDEV}/bin/psp-fixup-imports "$<TARGET_FILE:YtLegacyPSP>"
	COMMAND ${PSPDEV}/bin/psp-strip "$<TARGET_FILE:YtLegacyPSP>"
	COMMENT "Fixing up imports and stripping"
)

add_custom_command(
	TARGET YtLegacyPSP
	POST_BUILD
	COMMAND ${PSPDEV}/bin/mksfoex
		"-d" "MEMSIZE=1"
		"-s" "APP_VER=${PSP_VERSION}"
		"${PSP_TITLE}" "${PSP_PKG_DIR}/PARAM.SFO"
	COMMENT "Writing PARAM.SFO (SDK defaults)"
)

add_custom_command(
	TARGET YtLegacyPSP
	POST_BUILD
	COMMAND python3
		"${CMAKE_SOURCE_DIR}/tools/mkeboot.py"
		"${PSP_PKG_DIR}/EBOOT.PBP"
		"${PSP_PKG_DIR}/PARAM.SFO"
		"$<TARGET_FILE:YtLegacyPSP>"
	COMMAND ${CMAKE_COMMAND} -E copy "${BRIDGE_BUILD}/src/cooleyesBridge.prx"
		"${PSP_PKG_DIR}/cooleyesBridge.prx"
	COMMENT "Packaging EBOOT.PBP and cooleyesBridge.prx"
)
