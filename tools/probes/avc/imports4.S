/* Imports for probe v4, NIDs from PSPLibDoc and the PMPlayer Advance / ARK-4
 * sources. One stub block per library. */
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
