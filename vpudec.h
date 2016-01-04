#ifndef __VPU_DEC_H__
#define __VPU_DEC_H__

#include <imx-mm/vpu/vpu_wrapper.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VPU_DEC_MAX_NUM_MEM_NUM		10
#define ALIGN_PTR(ptr, align) \
	((((unsigned int)(ptr)) + (align) - 1) / (align) * (align))

typedef struct
{
	int v_count;
	unsigned int v_vaddr[VPU_DEC_MAX_NUM_MEM_NUM];

	int p_count;
	unsigned int p_vaddr[VPU_DEC_MAX_NUM_MEM_NUM];
	unsigned int p_paddr[VPU_DEC_MAX_NUM_MEM_NUM];
	unsigned int p_caddr[VPU_DEC_MAX_NUM_MEM_NUM];
	unsigned int p_msize[VPU_DEC_MAX_NUM_MEM_NUM];
} DecMemInfo;


int dec_init(int width, int height, VpuCodStd vstd);
void dec_exit();
int dec_stream(unsigned char *pbuf, int size, VpuDecInitInfo *pInitInfo);
VpuDecOutFrameInfo* dec_get_frame();
void dec_put_frame();

#ifdef __cplusplus
}
#endif

#endif
