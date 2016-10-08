#ifndef __VPUX_H__
#define __VPUX_H__

#ifdef BUILD_FOR_ANDROID
#include <vpu_wrapper.h>
#else
#include <imx-mm/vpu/vpu_wrapper.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define VPUX_MAX_NUM_MEM_NUM		16
#define ALIGN_PTR(ptr, align) \
	((((unsigned int)(ptr)) + (align) - 1) / (align) * (align))

typedef struct {
	int nvc;
	unsigned int vmem[VPUX_MAX_NUM_MEM_NUM];

	int npc;
	VpuMemDesc pmem[VPUX_MAX_NUM_MEM_NUM];
} XMemInfo;


typedef struct {
	int w;
	int h;
	int fps;
	int bitrate;
	VpuCodStd std;
	XMemInfo xmem;
	VpuEncHandle handle;
	VpuEncEncParam param;
} XEnc;

typedef struct {
	int w;
	int h;
	VpuCodStd std;
	XMemInfo xmem;
	VpuDeccHandle handle;
} XDec;


// apis for decode
int dec_init(XDec *pXDec);
void dec_exit(XDec *pXDec);
int dec_stream(XDec *pXDec, unsigned char *pbuf, int size, VpuDecInitInfo *pInitInfo);
int dec_get_frame(XDec *pXDec, VpuDecOutFrameInfo *pFrame);
void dec_put_frame(XDec *pXDec, VpuDecOutFrameInfo *pFrame);

// apis for encode
int enc_init(XEnc *pXEnc);
void enc_exit(XEnc *pXEnc);
int enc_stream(XEnc *pXEnc, unsigned int paddr, unsigned char **ppout);



#ifdef __cplusplus
}
#endif

#endif
