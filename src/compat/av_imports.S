/*
 * Imports for hardware H.264 playback.
 *
 * None of these are in this PSPSDK, so they are declared here by NID with the
 * SDK's own pspimport.s macros, one block per library so no library's stubs
 * ever split into two runs. Sources: PSPLibDoc kd/mpeg.csv and kd/avcodec.csv,
 * ARK-4 libs/KUBridge, and the export table of third_party/cooleyesBridge.
 *
 * libpspmpeg is deliberately not linked: it would add a second "sceMpeg"
 * block, and it lacks GetAvcNalAu and AvcDecodeDetail2 anyway.
 *
 * sceMpeg is resolved against flash0:/kd/mpeg_vsh.prx and sceMpegbase
 * against avcodec.prx, both loaded at run time by video_system_init. A
 * library that is not loaded at boot is fine to import: the loader links the
 * stubs when the module providing it is started.
 */
	.set noreorder
#include "pspimport.s"

	IMPORT_START "sceMpeg",0x00090000
	IMPORT_FUNC  "sceMpeg",0x682A619B,sceMpegInit
	IMPORT_FUNC  "sceMpeg",0x874624D6,sceMpegFinish
	IMPORT_FUNC  "sceMpeg",0xC132E22F,sceMpegQueryMemSize
	IMPORT_FUNC  "sceMpeg",0xD8C5F121,sceMpegCreate
	IMPORT_FUNC  "sceMpeg",0x606A4649,sceMpegDelete
	IMPORT_FUNC  "sceMpeg",0x167AFD9E,sceMpegInitAu
	IMPORT_FUNC  "sceMpeg",0x11F95CF1,sceMpegGetAvcNalAu
	IMPORT_FUNC  "sceMpeg",0x0E3C2E9D,sceMpegAvcDecode
	IMPORT_FUNC  "sceMpeg",0xCF3547A2,sceMpegAvcDecodeDetail2

	IMPORT_START "sceMpegbase",0x00090000
	IMPORT_FUNC  "sceMpegbase",0x91929A21,sceMpegBaseCscAvc

	IMPORT_START "KUBridge",0x40090000
	IMPORT_FUNC  "KUBridge",0x4C25EA72,kuKernelLoadModule

	IMPORT_START "cooleyesBridge",0x40090000
	IMPORT_FUNC  "cooleyesBridge",0xA2361B3C,cooleyesBridgeGetVersion
	IMPORT_FUNC  "cooleyesBridge",0x90CC7DD7,cooleyesMeBootStart
