#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "vpuenc.h"

#define MAX_FRAME_NUM	10

static EncMemInfo gMemInfo;
static VpuEncHandle gHandle;
static VpuEncEncParam gEncParam;

static void enc_mem_free(EncMemInfo* pEncMem)
{
	int i;
	VpuMemDesc vpuMem;
	VpuEncRetCode res;

	for(i = 0; i < pEncMem->nvc; i++) {
		free((void*)pEncMem->vmem[i]);
	}

	for(i = 0; i < pEncMem->npc; i++) {
		res = VPU_EncFreeMem(&pEncMem->pmem[i]);
		if(res != VPU_ENC_RET_SUCCESS)
			printf("Free pmem[%d] error : %d\n", i, res);
	}
}

static int enc_mem_alloc(VpuMemInfo* pMemBlock, EncMemInfo* pEncMem)
{
	int i;
	unsigned char *ptr = NULL;
	int size;

	for(i = 0; i < pMemBlock->nSubBlockNum; i++) {

		size = pMemBlock->MemSubBlock[i].nAlignment + pMemBlock->MemSubBlock[i].nSize;

		if(pMemBlock->MemSubBlock[i].MemType == VPU_MEM_VIRT) {
			ptr = (unsigned char *)malloc(size);
			if(ptr==NULL) {
				printf("Alloc vmem[%d] error : %s\n", i, strerror(errno));
				goto err;
			}

			pMemBlock->MemSubBlock[i].pVirtAddr = (unsigned char*)
				Align(ptr, pMemBlock->MemSubBlock[i].nAlignment);
			pEncMem->vmem[pEncMem->nvc] = (unsigned int)ptr;
			pEncMem->nvc++;
		} else {
			VpuMemDesc vpuMem;
			VpuEncRetCode res;
			vpuMem.nSize=size;
			res = VPU_EncGetMem(&vpuMem);
			if(res != VPU_ENC_RET_SUCCESS) {
				printf("Alloc pmem[%d] error : %d\n", i, res);
				goto err;
			}

			pMemBlock->MemSubBlock[i].pVirtAddr = (unsigned char*)
				Align(vpuMem.nVirtAddr, pMemBlock->MemSubBlock[i].nAlignment);
			pMemBlock->MemSubBlock[i].pPhyAddr = (unsigned char*)
				Align(vpuMem.nPhyAddr, pMemBlock->MemSubBlock[i].nAlignment);

			memcpy(&pEncMem->pmem[pEncMem->npc], &vpuMem, sizeof vpuMem);
			pEncMem->npc++;
		}
	}

	return 0;

err:
	enc_mem_free(pEncMem);
	return 0;

}

int enc_mem_frame(VpuCodStd std, VpuFrameBuffer* pRegFrame, int cnt, int w,
	int h, EncMemInfo* pEncMem, int align)
{
	int i;
	int yStride;
	int cStride;
	int ySize;
	int cSize;
	int mvSize;

	unsigned char* paddr;
	unsigned char* vaddr;
	VpuEncRetCode res;
	VpuMemDesc vpuMem;

	yStride = w;
	cStride = yStride >> 1;
	ySize = yStride * h;
	cSize = ySize >> 2;
	// mvSize = 0;
	mvSize = cSize;

	for(i = 0; i < cnt; i++) {
		vpuMem.nSize = ySize + cSize * 2 + mvSize + align;
		res = VPU_EncGetMem(&vpuMem);
		if(VPU_ENC_RET_SUCCESS != res) {
			printf("VPU_ENC Alloc frame buf[%d] error : %d\n", i, res);
			return -1;
		}

		paddr = (unsigned char*)Align(vpuMem.nPhyAddr, align);
		vaddr = (unsigned char*)Align(vpuMem.nVirtAddr, align);

		pRegFrame[i].nStrideY = yStride;
		pRegFrame[i].nStrideC = cStride;

		/* fill phy addr*/
		pRegFrame[i].pbufY = paddr;
		pRegFrame[i].pbufCb = paddr + ySize;
		pRegFrame[i].pbufCr = paddr + ySize + cSize;
		pRegFrame[i].pbufMvCol = paddr + ySize + cSize * 2;

		/* fill virt addr */
		pRegFrame[i].pbufVirtY = vaddr;
		pRegFrame[i].pbufVirtCb = vaddr + ySize;
		pRegFrame[i].pbufVirtCr = vaddr + ySize + cSize;
		pRegFrame[i].pbufVirtMvCol = vaddr + ySize + cSize * 2;

		/* fill bottom address for field tile*/
		pRegFrame[i].pbufY_tilebot = 0;
		pRegFrame[i].pbufCb_tilebot = 0;
		pRegFrame[i].pbufVirtY_tilebot = 0;
		pRegFrame[i].pbufVirtCb_tilebot = 0;

		//record memory info for release
		memcpy(&pEncMem->pmem[pEncMem->npc], &vpuMem, sizeof vpuMem);
		pEncMem->npc++;
	}

	return 0;
}

int enc_init(int w, int h, int fps, VpuCodStd std)
{
	int tmp, nBufNum, nAlign, nSize;
	VpuEncRetCode res;
	VpuVersionInfo ver;
	VpuWrapperVersionInfo vver;

	VpuMemInfo sMemInfo;
	VpuEncOpenParam sEncOpenParam;
	VpuEncInitInfo sEncInitInfo;
	VpuFrameBuffer sFrameBuf[MAX_FRAME_NUM];

	res = VPU_EncLoad();
	if(res != VPU_ENC_RET_SUCCESS) {
		printf("VPU_ENC load error : %d\n", res);
		return -1;
	}

	res = VPU_EncGetVersionInfo(&ver);
	if (res != VPU_ENC_RET_SUCCESS) {
		printf("VPU_ENC get version error : %d\n", res);
		goto err;
	}

	res = VPU_EncGetWrapperVersionInfo(&vver);
	if (res != VPU_ENC_RET_SUCCESS) {
		printf("VPU_ENC get wrapper version error : %d\n", res);
		goto err;
	}

	printf("======= VPU ENC =======\n");
	printf(" LIB : %d.%d.%d\n", ver.nLibMajor, ver.nLibMinor, ver.nLibRelease);
	printf(" FW  : %d.%d.%d.%d\n", ver.nFwMajor, ver.nFwMinor, ver.nFwRelease, ver.nFwCode);
	printf(" WLIB: %d.%d.%d\n", vver.nMajor, vver.nMinor, vver.nRelease);
	printf("=======================\n");

	res = VPU_EncQueryMem(&sMemInfo);
	if (res != VPU_ENC_RET_SUCCESS) {
		printf("VPU_ENC query memory error : %d\n", res);
		goto err;
	}

	tmp = enc_mem_alloc(&sMemInfo, &gMemInfo);
	if(tmp) {
		printf("enc_mem_alloc error\n");
		goto err;
	}

	bzero(&sEncOpenParam, sizeof(VpuEncOpenParam));
	sEncOpenParam.eFormat = std;
	sEncOpenParam.sMirror = VPU_ENC_MIRDIR_NONE;
	sEncOpenParam.nPicWidth = w;
	sEncOpenParam.nPicHeight = h;
	sEncOpenParam.nRotAngle = 0;
	sEncOpenParam.nFrameRate = fps;
	sEncOpenParam.nBitRate = 5000;
	sEncOpenParam.nGOPSize = 100;
	sEncOpenParam.nChromaInterleave = 0;

	sEncOpenParam.nMapType = 0;
	sEncOpenParam.nLinear2TiledEnable = 0;
	sEncOpenParam.eColorFormat = VPU_COLOR_420;
    sEncOpenParam.nInitialDelay = 0;
	sEncOpenParam.nVbvBufferSize = 0;

	sEncOpenParam.sliceMode.sliceMode = 0;	/* 0: 1 slice per picture; 1: Multiple slices per picture */
	sEncOpenParam.sliceMode.sliceSizeMode = 0; /* 0: silceSize defined by bits; 1: sliceSize defined by MB number*/
	sEncOpenParam.sliceMode.sliceSize = 4000;//4000;  /* Size of a slice in bits or MB numbers */

	//sEncOpenParam.enableAutoSkip = 1;
	sEncOpenParam.nUserGamma = 0.75*32768;         /*  (0*32768 <= gamma <= 1*32768) */
	sEncOpenParam.nRcIntervalMode = 0; //1;        /* 0:normal, 1:frame_level, 2:slice_level, 3: user defined Mb_level */
	sEncOpenParam.nMbInterval = 0;

	// sEncOpenParam.nAvcIntra16x16OnlyModeEnable = 0;
	// sEncOpenParam.VpuEncStdParam.avcParam.avc_constrainedIntraPredFlag = 0;
	// sEncOpenParam.VpuEncStdParam.avcParam.avc_disableDeblk = 0;
	// sEncOpenParam.VpuEncStdParam.avcParam.avc_deblkFilterOffsetAlpha = 0;
	// sEncOpenParam.VpuEncStdParam.avcParam.avc_deblkFilterOffsetBeta = 0;
	// sEncOpenParam.VpuEncStdParam.avcParam.avc_chromaQpOffset = 0;
	// sEncOpenParam.VpuEncStdParam.avcParam.avc_audEnable = 0;
	// sEncOpenParam.VpuEncStdParam.avcParam.avc_fmoEnable = 0;
	// sEncOpenParam.VpuEncStdParam.avcParam.avc_fmoType = 0;
	// sEncOpenParam.VpuEncStdParam.avcParam.avc_fmoSliceNum = 0;
	// sEncOpenParam.VpuEncStdParam.avcParam.avc_fmoSliceSaveBufSize = 32; /* FMO_SLICE_SAVE_BUF_SIZE */

	res = VPU_EncOpen(&gHandle, &sMemInfo, &sEncOpenParam);
	if (res != VPU_ENC_RET_SUCCESS) {
		printf("VPU_ENC open error : %d\n", res);
		goto err1;
	}

	res = VPU_EncConfig(gHandle, VPU_ENC_CONF_NONE, NULL);
	if(VPU_ENC_RET_SUCCESS != res) {
		printf("VPU_ENC config error : %d\n", res);
		goto err2;
	}

	//get initinfo
	res = VPU_EncGetInitialInfo(gHandle, &sEncInitInfo);
	if(VPU_ENC_RET_SUCCESS != res) {
		printf("VPU_ENC get Init Info error : %d\n", res);
		goto err2;
	}

	nBufNum = sEncInitInfo.nMinFrameBufferCount;
	nAlign  = sEncInitInfo.nAddressAlignment;
	printf("Init OK: min buffer cnt: %d, alignment: %d\n", nBufNum, nAlign);

	tmp = enc_mem_frame(sEncOpenParam.eFormat, sFrameBuf,
			nBufNum, w, h, &gMemInfo, nAlign);
	if(tmp) {
		printf("enc_mem_frame error\n");
		goto err2;
	}

	res = VPU_EncRegisterFrameBuffer(gHandle, sFrameBuf, nBufNum, w);
	if(VPU_ENC_RET_SUCCESS != res) {
		printf("VPU_ENC register frame buffer error : %d\n", res);
		goto err2;
	}

	nSize = w * h * 3 / 2;
	nSize += nAlign * 2;

	bzero(&sMemInfo, sizeof(VpuMemInfo));
	sMemInfo.nSubBlockNum = 2;
	sMemInfo.MemSubBlock[0].MemType = VPU_MEM_PHY;
	sMemInfo.MemSubBlock[0].nAlignment = nAlign;
	sMemInfo.MemSubBlock[0].nSize = nSize;
	sMemInfo.MemSubBlock[1].MemType = VPU_MEM_PHY;
	sMemInfo.MemSubBlock[1].nAlignment = nAlign;
	sMemInfo.MemSubBlock[1].nSize = nSize;

	nSize = enc_mem_alloc(&sMemInfo, &gMemInfo);
    if(nSize) {
		printf("enc_mem_alloc error\n");
		goto err2;
	}

	bzero(&gEncParam, sizeof(VpuEncEncParam));
	gEncParam.eFormat = VPU_V_AVC;
	gEncParam.nPicWidth = w;
	gEncParam.nPicHeight = h;
	gEncParam.nFrameRate = sEncOpenParam.nFrameRate;
	gEncParam.nQuantParam = 0;
	gEncParam.nInPhyInput = (unsigned int)sMemInfo.MemSubBlock[0].pPhyAddr;
	gEncParam.nInVirtInput = (unsigned int)sMemInfo.MemSubBlock[0].pVirtAddr;
	gEncParam.nInInputSize = nSize;
	gEncParam.nInPhyOutput = (unsigned int)sMemInfo.MemSubBlock[1].pPhyAddr;
	gEncParam.nInVirtOutput=(unsigned int)sMemInfo.MemSubBlock[1].pVirtAddr;
	gEncParam.nInOutputBufLen = nSize;

	gEncParam.nForceIPicture = 0;
	gEncParam.nSkipPicture = 0;
	gEncParam.nEnableAutoSkip = 0;
	gEncParam.pInFrame = NULL;

	return 0;

err2:
	VPU_EncClose(gHandle);
err1:
	enc_mem_free(&gMemInfo);
err:
	VPU_EncUnLoad();

	return -1;
}

void enc_exit()
{
	VPU_EncClose(gHandle);
	VPU_EncUnLoad();
	enc_mem_free(&gMemInfo);
}

int enc_stream(unsigned int paddr, unsigned char **ppout)
{
	VpuEncRetCode res;

	gEncParam.nInPhyInput = paddr;

	res = VPU_EncEncodeFrame(gHandle, &gEncParam);
	if(VPU_ENC_RET_SUCCESS != res) {
		if(VPU_ENC_RET_FAILURE_TIMEOUT == res) {
			printf("VPU_ENC register frame buffer error : %d\n", res);
			// VPU_EncReset(gHandle);
		}
		return -1;
	}

	*ppout = (unsigned char *)gEncParam.nInVirtOutput;

	// printf("VPU_ENC encode stream res = 0x%08x\n", gEncParam.eOutRetCode);

	if((gEncParam.eOutRetCode & VPU_ENC_OUTPUT_DIS) ||
	   (gEncParam.eOutRetCode & VPU_ENC_OUTPUT_SEQHEADER))
	{
		return gEncParam.nOutOutputSize;
	}

	return 0;
}















