/* sceMpeg imports by NID (PSPLibDoc kd/mpeg.csv). One stub block for the
 * whole library, so it never splits into two runs. */
	.set noreorder
#include "pspimport.s"
	IMPORT_START "sceMpeg",0x00090000
	IMPORT_FUNC  "sceMpeg",0x682A619B,sceMpegInit
	IMPORT_FUNC  "sceMpeg",0x874624D6,sceMpegFinish
	IMPORT_FUNC  "sceMpeg",0xC132E22F,sceMpegQueryMemSize
	IMPORT_FUNC  "sceMpeg",0xD8C5F121,sceMpegCreate
	IMPORT_FUNC  "sceMpeg",0x606A4649,sceMpegDelete
	IMPORT_FUNC  "sceMpeg",0x37295ED8,sceMpegRingbufferConstruct
	IMPORT_FUNC  "sceMpeg",0xA780CF7E,sceMpegMallocAvcEsBuf
	IMPORT_FUNC  "sceMpeg",0x167AFD9E,sceMpegInitAu
	IMPORT_FUNC  "sceMpeg",0x11F95CF1,sceMpegGetAvcNalAu
	IMPORT_FUNC  "sceMpeg",0x0E3C2E9D,sceMpegAvcDecode
	IMPORT_FUNC  "sceMpeg",0xA11C7026,sceMpegAvcDecodeMode
