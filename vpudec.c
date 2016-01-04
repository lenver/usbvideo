#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <errno.h>

#include <asm/types.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include <signal.h>
#include <pthread.h>

#include <imx-mm/vpu/vpu_wrapper.h>

#include "vpudec.h"

#define MAX_FRAME_NUM	10
#define FRAME_SURPLUS	0
#define	FRAME_ALIGN		16

////////////////// var for VPU dec //////////////////
static VpuDecHandle	gHandle;
static VpuMemInfo	gMemInfo;
static DecMemInfo	gDecMem;
static VpuCodStd	gDecStd;
static int 			gMapType = 0;
static VpuDecOutFrameInfo gFrameInfo;
/////////////////////////////////////////////////////

static void dec_mem_free(DecMemInfo *pDecMem)
{
	int i;
	VpuMemDesc vpuMem;
	VpuDecRetCode res;

	for(i = 0; i < pDecMem->v_count; i++) {
		if(pDecMem->v_vaddr[i])
			free((void*)pDecMem->v_vaddr[i]);

		printf("[%d] Free [v] mem\n", i);
	}

	pDecMem->v_count = 0;

	for(i = 0; i < pDecMem->p_count; i++) {
		vpuMem.nPhyAddr = pDecMem->p_paddr[i];
		vpuMem.nVirtAddr= pDecMem->p_vaddr[i];
		vpuMem.nCpuAddr = pDecMem->p_caddr[i];
		vpuMem.nSize	= pDecMem->p_msize[i];
		res = VPU_DecFreeMem(&vpuMem);
		if(res != VPU_DEC_RET_SUCCESS) {
			printf("Free vpu memory[%d] failed err = %d\n", res);
		}

		printf("[%d] Free [p] mem\n", i);
	}

	pDecMem->p_count = 0;
}

static void dec_mem_free_block(DecMemInfo *pDecMem, int numFB)
{
	int i;
	VpuMemDesc vpuMem;
	VpuDecRetCode res;

	for(i = pDecMem->p_count - numFB; i < pDecMem->p_count; i++) {
		vpuMem.nPhyAddr = pDecMem->p_paddr[i];
		vpuMem.nVirtAddr= pDecMem->p_vaddr[i];
		vpuMem.nCpuAddr = pDecMem->p_caddr[i];
		vpuMem.nSize	= pDecMem->p_msize[i];
		res = VPU_DecFreeMem(&vpuMem);
		if(res != VPU_DEC_RET_SUCCESS) {
			printf("Free vpu memory[%d] failed err = %d\n", res);
		}

		printf("[%d] Free [p] mem\n", i);
	}

	pDecMem->p_count = pDecMem->p_count - numFB;
}

static int dec_mem_alloc(VpuMemInfo *pMemInfo, DecMemInfo *pDecMem)
{
	int i, size;
	unsigned char *ptr = NULL;

	for(i = 0; i < pMemInfo->nSubBlockNum; i++) {
		size = pMemInfo->MemSubBlock[i].nAlignment +
			   pMemInfo->MemSubBlock[i].nSize;

		printf("[%d] Alloc [%c] mem sz=%d, ag=%d, tsz=%d\n", i,
			pMemInfo->MemSubBlock[i].MemType == VPU_MEM_VIRT ? 'v' : 'p',
			pMemInfo->MemSubBlock[i].nSize,
			pMemInfo->MemSubBlock[i].nAlignment, size);

		if(pMemInfo->MemSubBlock[i].MemType == VPU_MEM_VIRT) {
			ptr = (unsigned char *)malloc(size);

			if(!ptr) {
				printf("Get virtual memory[%d] failed, size = %d\n", i, size);
				goto err;
			}

			pMemInfo->MemSubBlock[i].pVirtAddr = (unsigned char *)
				ALIGN_PTR(ptr, pMemInfo->MemSubBlock[i].nAlignment);

			//record virtual base addr
			pDecMem->v_vaddr[pDecMem->v_count] = (unsigned int)ptr;
			pDecMem->v_count++;

		} else { // VPU_MEM_PHY
			VpuDecRetCode res;
			VpuMemDesc vpuMem;
			vpuMem.nSize = size;
			res = VPU_DecGetMem(&vpuMem);
			if(res != VPU_DEC_RET_SUCCESS) {
				printf("Get vpu memory[%d] failure, size = %d, err = %d\n", i, size, res);
				goto err;
			}

			pMemInfo->MemSubBlock[i].pVirtAddr =(unsigned char*)ALIGN_PTR(
				vpuMem.nVirtAddr, pMemInfo->MemSubBlock[i].nAlignment);
			pMemInfo->MemSubBlock[i].pPhyAddr =(unsigned char*)ALIGN_PTR(
				vpuMem.nPhyAddr, pMemInfo->MemSubBlock[i].nAlignment);

			//record physical base addr
			pDecMem->p_paddr[pDecMem->p_count] = (unsigned int)vpuMem.nPhyAddr;
			pDecMem->p_vaddr[pDecMem->p_count] = (unsigned int)vpuMem.nVirtAddr;
			pDecMem->p_caddr[pDecMem->p_count] = (unsigned int)vpuMem.nCpuAddr;
			pDecMem->p_msize[pDecMem->p_count] = size;
			pDecMem->p_count++;

		}
	}

	return 0;
err:
	dec_mem_free(pDecMem);
	return -1;
}

int dec_info_init(VpuDecInitInfo *pInitInfo, int *pOutFrmNum)
{
	VpuDecRetCode res;
	VpuMemDesc vpuMem;
	VpuFrameBuffer frameBuf[MAX_FRAME_NUM];
	int BufNum, i;
	int ySize, uSize, vSize, mvSize;
	int yStride, uStride, vStride;
	unsigned char* paddr;
	unsigned char* vaddr;
	int nAlign;
	int multifactor = 1;

	//get init info
	res = VPU_DecGetInitialInfo(gHandle, pInitInfo);
	if(res != VPU_DEC_RET_SUCCESS) {
		printf("VPU_DecGetInitialInfo failed err = %d\n", res);
		return -1;
	}

	//malloc frame buffs
	BufNum = pInitInfo->nMinFrameBufferCount + FRAME_SURPLUS;
	if(BufNum > MAX_FRAME_NUM) {
		printf("VPU req too many frames num=%d\n", pInitInfo->nMinFrameBufferCount);
		return -1;
	}

	yStride = ALIGN_PTR(pInitInfo->nPicWidth, FRAME_ALIGN);
	if(pInitInfo->nInterlace) {
		ySize = ALIGN_PTR(pInitInfo->nPicWidth, FRAME_ALIGN) *
				ALIGN_PTR(pInitInfo->nPicHeight, (2 * FRAME_ALIGN));
	} else {
		ySize = ALIGN_PTR(pInitInfo->nPicWidth, FRAME_ALIGN) *
				ALIGN_PTR(pInitInfo->nPicHeight, FRAME_ALIGN);
	}

	//for MJPG: we need to check 4:4:4/4:2:2/4:2:0/4:0:0
	if(gDecStd == VPU_V_MJPG) {
		switch(pInitInfo->nMjpgSourceFormat) {
		case VPU_COLOR_420:
			printf("MJPG: 4:2:0\n");
			uStride = vStride = yStride >> 1;
			uSize  	= vSize = ySize >> 2;
			mvSize 	= uSize;
			break;
		case VPU_COLOR_422H:
			printf("MJPG: 4:2:2 H\n");
			uStride = vStride = yStride >> 1;
			uSize	= vSize = ySize >> 1;
			mvSize	= uSize;
			break;
		case VPU_COLOR_422V:
			printf("MJPG: 4:2:2 V\n");
			uStride = vStride = yStride;
			uSize	= vSize = ySize >> 1;
			mvSize	= uSize;
			break;
		case VPU_COLOR_444:
			printf("MJPG: 4:4:4\n");
			uStride	= vStride = yStride;
			uSize	= vSize = ySize;
			mvSize=uSize;
			break;
		case VPU_COLOR_400:
			printf("MJPG: 4:0:0\n");
			uStride	= vStride = 0;
			uSize	= vSize = 0;
			mvSize	= 0;
			break;
		default:	//4:2:0
			printf("unknown color format: %d\n",pInitInfo->nMjpgSourceFormat);
			uStride = vStride = yStride >> 1;
			uSize	= vSize = ySize >> 2;
			mvSize	= uSize;
			break;
		}
	} else {
		//4:2:0 for all video
		uStride = vStride = yStride >> 1;
		uSize	= vSize = ySize >> 2;
		mvSize	= uSize;
	}

	nAlign = pInitInfo->nAddressAlignment;

	if(gMapType == 2) multifactor = 2;

	if(nAlign > 1) {
		ySize = ALIGN_PTR(ySize, nAlign * multifactor);
		uSize = ALIGN_PTR(uSize, nAlign);
		vSize = ALIGN_PTR(vSize, nAlign);
	}

	printf(" FBinfo: Y U V stride = [%d, %d, %d]\n", yStride, uStride, vStride);
	printf(" FBinfo: Y U V size   = [%d, %d, %d]\n", ySize, uSize, vSize);

	// avoid buffer is too big to malloc by vpu
	for(i = 0; i < BufNum; i++) {
#if 0
        gDecMem.p_paddr[gDecMem.p_count] = (unsigned int)obuffers[i].offset;
		gDecMem.p_vaddr[gDecMem.p_count] = (unsigned int)obuffers[i].start;
		gDecMem.p_caddr[gDecMem.p_count] = 0;
		gDecMem.p_msize[gDecMem.p_count] = obuffers[i].length;
		gDecMem.p_count++;

		//fill frameBuf
		paddr = (unsigned char*)obuffers[i].offset;
		vaddr = (unsigned char*)obuffers[i].start;
#else
		VpuMemDesc vpuMem;
		vpuMem.nSize = 1280 * 720 * 2;
		res = VPU_DecGetMem(&vpuMem);
		if(res != VPU_DEC_RET_SUCCESS) {
			printf("FB[%d] get vpu memory failed, ret=%d\n", i, res);
			return -1;
		}
		gDecMem.p_paddr[gDecMem.p_count] = (unsigned int)vpuMem.nPhyAddr;
		gDecMem.p_vaddr[gDecMem.p_count] = (unsigned int)vpuMem.nVirtAddr;
		gDecMem.p_caddr[gDecMem.p_count] = (unsigned int)vpuMem.nCpuAddr;
		gDecMem.p_msize[gDecMem.p_count] = vpuMem.nSize;
		gDecMem.p_count++;

		//fill frameBuf
		paddr = (unsigned char*)vpuMem.nPhyAddr;
		vaddr = (unsigned char*)vpuMem.nVirtAddr;
#endif


		/*align the base address*/
		if(nAlign > 1) {
			paddr = (unsigned char*)ALIGN_PTR(paddr, nAlign);
			vaddr = (unsigned char*)ALIGN_PTR(vaddr, nAlign);
		}

		/* fill stride info */
		frameBuf[i].nStrideY = yStride;
		frameBuf[i].nStrideC = uStride;

		/* fill phy addr*/
		frameBuf[i].pbufY	= paddr;
		frameBuf[i].pbufCb	= paddr + ySize;
		frameBuf[i].pbufCr	= paddr + ySize + uSize;
		// frameBuf[i].pbufMvCol = paddr + ySize + uSize + vSize;
		// paddr += ySize + uSize + vSize + mvSize;

		/* fill virt addr */
		frameBuf[i].pbufVirtY	= vaddr;
		frameBuf[i].pbufVirtCb	= vaddr + ySize;
		frameBuf[i].pbufVirtCr	= vaddr + ySize + uSize;
		// frameBuf[i].pbufVirtMvCol = vaddr + ySize + uSize + vSize;
		// vaddr += ySize + uSize + vSize + mvSize;

		/* fill bottom address for field tile*/
		if(gMapType == 2) {
			frameBuf[i].pbufY_tilebot	= frameBuf[i].pbufY + ySize / 2;
			frameBuf[i].pbufCb_tilebot	= frameBuf[i].pbufCr;
			frameBuf[i].pbufVirtY_tilebot	= frameBuf[i].pbufVirtY + ySize / 2;
			frameBuf[i].pbufVirtCb_tilebot	= frameBuf[i].pbufVirtCr;
		} else {
			frameBuf[i].pbufY_tilebot		= 0;
			frameBuf[i].pbufCb_tilebot		= 0;
			frameBuf[i].pbufVirtY_tilebot	= 0;
			frameBuf[i].pbufVirtCb_tilebot	= 0;
		}
	}

	//register frame buffs
	res = VPU_DecRegisterFrameBuffer(gHandle, frameBuf, BufNum);
	if(res != VPU_DEC_RET_SUCCESS){
		printf("VPU_DecRegisterFrameBuffer failed err = %d\n", res);
		return -1;
	}

	*pOutFrmNum = BufNum;
	return 0;
}

int dec_init(int width, int height, VpuCodStd vstd)
{
	int param;
	VpuDecRetCode res;
	VpuVersionInfo vvi;
	VpuWrapperVersionInfo vwvi;
	VpuDecOpenParam oparam;
	VpuDecConfig config;

	res = VPU_DecLoad();
	if(res != VPU_DEC_RET_SUCCESS) {
		printf("VPU_DecLoad failed err = %d\n", res);
		return -1;
	}
	VPU_DecGetVersionInfo(&vvi);
	VPU_DecGetWrapperVersionInfo(&vwvi);
	printf("========== VPU Info ============\n");
	printf("  VPU lib version : %d.%d.%d\n", vvi.nLibMajor, vvi.nLibMinor, vvi.nLibRelease);
	printf("  VPU FW  version : %d.%d.%d.%d\n", vvi.nFwMajor, vvi.nFwMinor, vvi.nFwRelease, vvi.nFwCode);
	printf("  VPU WP  version : %d.%d.%d\n", vwvi.nMajor, vwvi.nMinor, vwvi.nRelease);
	printf("================================\n");

	bzero(&gMemInfo, sizeof(gMemInfo));
	bzero(&gDecMem, sizeof(gDecMem));

	res = VPU_DecQueryMem(&gMemInfo);
	if (res != VPU_DEC_RET_SUCCESS) {
		printf("VPU_DecQueryMem failed err = %d\n", res);
		goto err0;
	}

	if(dec_mem_alloc(&gMemInfo, &gDecMem) < 0) {
		printf("Dec memory alloc failed\n");
		goto err0;
	}

	gDecStd = vstd;

	bzero(&oparam, sizeof(oparam));
	oparam.CodecFormat = gDecStd;
	oparam.nChromaInterleave = 0;
	oparam.nReorderEnable = 0;
	oparam.nMapType = 0;
	oparam.nTiled2LinearEnable = 0;
	oparam.nEnableFileMode = 0;
	oparam.nPicWidth = width;
	oparam.nPicHeight = height;

	res = VPU_DecOpen(&gHandle, &oparam, &gMemInfo);
	if(res != VPU_ENC_RET_SUCCESS) {
		printf("VPU_DecOpen failed err = %d\n", res);
		goto err1;
	}

	config = VPU_DEC_CONF_SKIPMODE;
	param = VPU_DEC_SKIPNONE;

	res = VPU_DecConfig(gHandle, config, &param);
	if(res != VPU_DEC_RET_SUCCESS) {
		printf("VPU_DecConfig skipmode failure: err = %d\n", res);
	}

	config = VPU_DEC_CONF_INPUTTYPE;
	param = VPU_DEC_IN_NORMAL;
	res = VPU_DecConfig(gHandle, config, &param);
	if(res != VPU_DEC_RET_SUCCESS) {
		printf("VPU_DecConfig inputtype failure: err = %d\n", res);
	}

	return 0;

err1:
	dec_mem_free(&gDecMem);
err0:
	VPU_DecUnLoad();
	return -1;
}

void dec_exit()
{
	VPU_DecClose(gHandle);
	dec_mem_free(&gDecMem);
	VPU_DecUnLoad();
}

int dec_stream(unsigned char *pbuf, int size, VpuDecInitInfo *pInitInfo)
{
	int nFrmNum;
    int bufRetCode = 0;
    VpuDecRetCode res;
    VpuBufferNode InData;

	InData.nSize = size;
	InData.pPhyAddr = NULL;
	InData.pVirAddr = pbuf;
	InData.sCodecData.pData = NULL;
	InData.sCodecData.nSize = 0;

	res = VPU_DecDecodeBuf(gHandle, &InData, &bufRetCode);
	if(VPU_DEC_RET_SUCCESS != res) {
		printf("VPU_DecDecodeBuf failed err = %d\n", res);
		return -1;
	}

	// printf("=== dec buf ret code = 0x%08x\n", bufRetCode);

	//check init info
	if(bufRetCode & VPU_DEC_INIT_OK) {
		//process init info
		if(dec_info_init(pInitInfo, &nFrmNum) < 0) {
			printf("VPU frame init info failure\n");
		}

		printf("VPU frame Init info OK, [%d x %d], Interlaced: %d, MinFrm: %d, nFrmNum=%d\n",
			pInitInfo->nPicWidth, pInitInfo->nPicHeight,
			pInitInfo->nInterlace, pInitInfo->nMinFrameBufferCount, nFrmNum);
		// if (pInitInfo->nPicHeight == 1088)
			// pInitInfo->nPicHeight = 1080;
	}

	if(bufRetCode & VPU_DEC_RESOLUTION_CHANGED) {
		printf("Resolution changed, release prev frames %d\n", nFrmNum);
		dec_mem_free_block(&gDecMem, nFrmNum);

		if(dec_info_init(pInitInfo, &nFrmNum) < 0) {
			printf("VPU frame Re-init info failure\n");
			return -1;
		}

		printf("VPU frame Re-Init OK, [%d x %d], Interlaced: %d, MinFrm: %d\n",
			pInitInfo->nPicWidth, pInitInfo->nPicHeight,
			pInitInfo->nInterlace, pInitInfo->nMinFrameBufferCount);
	}

	if((bufRetCode & VPU_DEC_OUTPUT_DIS) || (bufRetCode & VPU_DEC_OUTPUT_MOSAIC_DIS)) {
		// return 1;
	}

	if(bufRetCode & VPU_DEC_OUTPUT_EOS) {
		printf("VPU_DEC_OUTPUT_EOS\n");
		// return 2;
	}

	if(bufRetCode & VPU_DEC_NO_ENOUGH_INBUF) {
		printf("VPU_DEC_NO_ENOUGH_INBUF\n");
		// return 3;
	}

	if(bufRetCode & VPU_DEC_NO_ENOUGH_BUF) {
		printf("VPU_DEC_NO_ENOUGH_BUF\n");
		// return 3;
	}

	return bufRetCode;
}

VpuDecOutFrameInfo* dec_get_frame()
{
	VpuDecRetCode res;

	res = VPU_DecGetOutputFrame(gHandle, &gFrameInfo);
	if(res != VPU_DEC_RET_SUCCESS) {
		printf("VPU_DecGetOutputFrame failed err = %d\n", res);
		return NULL;
	}

	return &gFrameInfo;
}

void dec_put_frame()
{
	VpuDecRetCode res;
	res = VPU_DecOutFrameDisplayed(gHandle, gFrameInfo.pDisplayFrameBuf);
	if(res != VPU_DEC_RET_SUCCESS) {
		printf("VPU_DecOutFrameDisplayed failed err = %d\n", res);
	}
}

