#ifndef __VPU_ENC_H__
#define __VPU_ENC_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <imx-mm/vpu/vpu_wrapper.h>

#define VPU_ENC_MAX_NUM_MEM_REQS	16

#define Align(ptr,align)	(((unsigned int)ptr + (align) - 1) / (align) * (align))

typedef struct
{
	int nvc;
	unsigned int vmem[VPU_ENC_MAX_NUM_MEM_REQS];

	int npc;
	VpuMemDesc pmem[VPU_ENC_MAX_NUM_MEM_REQS];
} EncMemInfo;


int enc_init(int w, int h, int fps, VpuCodStd std);
void enc_exit();

int enc_stream(unsigned int paddr, unsigned char **ppout);


#ifdef __cplusplus
}
#endif

#endif