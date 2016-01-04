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

#include <linux/fb.h>
#include <linux/input.h>
#include <linux/videodev2.h>

#include <signal.h>
#include <pthread.h>

#include <g2d.h>
#include <linux/ipu.h>
#include <linux/mxcfb.h>

extern "C" {
#include <vpu_lib.h>
#include <vpu_io.h>
};

#include <imx-mm/vpu/vpu_wrapper.h>

#include "vpudec.h"


#define FPS_VIDEO		25
#define FRAME_DURATION	(1000000.0 / FPS_VIDEO)

#define OUT_BUFFERS		1
#define CSC_USE_IPU		1		// comment this to use g2d do csc

static const char dev_fb0[] = "/dev/fb0";
static const char dev_fb1[] = "/dev/fb1";
static const char dev_fle[] = "test.h264";
static const char dev_ipu[] = "/dev/mxc_ipu";

static int cx_screen = 0;
static int cy_screen = 0;
static int fd_out = 0;
static int fd_fle = 0;
static int exitflag = 0;		// indicate exit
static int framesize = 0;

static struct fb_var_screeninfo g_varinfo;

struct video_buffer {
	unsigned char * vaddr;
	unsigned int	paddr;
	unsigned int 	size;
};

static struct video_buffer buffers_out[OUT_BUFFERS];

#ifdef CSC_USE_IPU
static int fd_ipu;
static struct ipu_task gtask;
#else
static void *ghandler;
static struct g2d_surface gsrc;
static struct g2d_surface gdst;
#endif

static int display_init()
{
	int i, osize, fb0;
	struct mxcfb_gbl_alpha alpha;
	struct fb_var_screeninfo vinfo0;
	struct fb_fix_screeninfo finfo1;

	if ((fb0 = open(dev_fb0, O_RDWR )) < 0) {
		printf("Unable to open '%s' res=%d\n", dev_fb0, fb0);
		return -1;
	}

	if (ioctl(fb0, FBIOGET_VSCREENINFO, &vinfo0)) {
		printf("Error reading variable infomation.\n");
		close(fb0);
		return -1;
	}

	cx_screen = vinfo0.xres;
	cy_screen = vinfo0.yres;

	osize = vinfo0.xres * vinfo0.yres * vinfo0.bits_per_pixel / 8;

	printf(" === DSP [%d x %d] bpp=%d, sz=%d, fmt=%c%c%c%c\n", vinfo0.xres,
		vinfo0.yres, vinfo0.bits_per_pixel, osize, vinfo0.nonstd,
		vinfo0.nonstd >> 8, vinfo0.nonstd >> 16, vinfo0.nonstd >> 24);

	alpha.enable = 1;
	alpha.alpha = 0;

	if (ioctl(fb0, MXCFB_SET_GBL_ALPHA, &alpha) < 0) {
		printf("Set global alpha failed\n");
		close(fb0);
		return -1;
	}

	close(fb0);

	fd_out = open(dev_fb1, O_RDWR);
	if(fd_out < 0) {
		printf("Unable to open '%s' res=%d\n", dev_fb1, fd_out);
		return -1;
	}

	// memcpy(&g_varinfo, &vinfo0, sizeof(g_varinfo));
	if (ioctl(fd_out, FBIOGET_VSCREENINFO, &g_varinfo)) {
		printf("Error read vinfo.\n");
		goto err;
	}

	g_varinfo.xoffset = 0;
	g_varinfo.yoffset = 0;
	g_varinfo.xres = cx_screen;
	g_varinfo.yres = cy_screen;
	g_varinfo.xres_virtual = cx_screen;
	g_varinfo.yres_virtual = cy_screen * OUT_BUFFERS;
	g_varinfo.nonstd = 0;

	if (ioctl(fd_out, FBIOPUT_VSCREENINFO, &g_varinfo)) {
		printf("Error write vinfo.\n");
		goto err;
	}

	if (ioctl(fd_out, FBIOGET_VSCREENINFO, &g_varinfo)) {
		printf("Error read vinfo.\n");
		goto err;
	}

	osize = g_varinfo.xres * g_varinfo.yres *
			g_varinfo.bits_per_pixel / 8;

	printf(" === OUT [%d x %d] bpp=%d, sz=%d, fmt=%c%c%c%c\n", g_varinfo.xres,
		g_varinfo.yres, g_varinfo.bits_per_pixel, osize, g_varinfo.nonstd,
		g_varinfo.nonstd >> 8, g_varinfo.nonstd >> 16, g_varinfo.nonstd >> 24);

	if (ioctl(fd_out, FBIOGET_FSCREENINFO, &finfo1)) {
		printf("Error read finfo.\n");
		goto err;
	}

	printf("'%s' : smem=0x%08X, slen=%d\n", dev_fb1,
		finfo1.smem_start, finfo1.smem_len);

	for(i = 0; i < OUT_BUFFERS; i++) {
		buffers_out[i].paddr = finfo1.smem_start + osize * i;
		buffers_out[i].size = osize;
	}

#ifdef CSC_USE_IPU
	fd_ipu = open(dev_ipu, O_RDWR);
	if(fd_ipu < 0) {
		printf("Unable to open '%s' res=%d\n", dev_ipu, fd_ipu);
		goto err;
	}

	bzero(&gtask, sizeof gtask);

	gtask.priority = IPU_TASK_PRIORITY_HIGH;

	gtask.input.width = 160;
	gtask.input.height = 128;
	gtask.input.format = IPU_PIX_FMT_YUV420P;

	gtask.output.width = cx_screen;
	gtask.output.height = cy_screen;
	gtask.output.paddr = buffers_out[0].paddr;
	if(g_varinfo.bits_per_pixel == 16)
		gtask.output.format = IPU_PIX_FMT_RGB565;
	else
		gtask.output.format = IPU_PIX_FMT_RGB32;

#else

	g2d_open(&ghandler);
	if(ghandler == NULL) {
		printf("Error open g2d.\n");
		goto err;
	}

	bzero(&gsrc, sizeof gsrc);
	bzero(&gdst, sizeof gdst);

	gsrc.left = 0;
	gsrc.top = 0;
	gsrc.right = 160;
	gsrc.bottom = 128;
	gsrc.width = 160;
	gsrc.height = 128;
	gsrc.stride = 160;
	gsrc.format = G2D_NV12;

	gdst.left = 0;
	gdst.top = 0;
	gdst.right = cx_screen;
	gdst.bottom = cy_screen;
	gdst.width = cx_screen;
	gdst.height = cy_screen;
	gdst.stride = cx_screen;
	gdst.planes[0] = buffers_out[0].paddr;
	if(g_varinfo.bits_per_pixel == 16)
		gdst.format = G2D_RGB565;
	else
		gdst.format = G2D_RGBX8888;

#endif
	return 0;
err:
	close(fd_out);
	return -1;
}

static void display_exit()
{
	ioctl(fd_out, FBIOBLANK, FB_BLANK_POWERDOWN);

#ifdef CSC_USE_IPU
	close(fd_ipu);
#else
	g2d_close(ghandler);
#endif
	close(fd_out);
}

static void exit_signal(int signo)
{
	exitflag = 1;
	printf("\n");
}

int main(int argc, char *argv[])
{
	int res, tus;
	char filepath[128];
	unsigned long ss;
	struct timeval ts, te;

	int rlen;
	bool bfeed = true;
	bool bfirst = true;
	unsigned char rbuf[4096];
	VpuDecInitInfo vdii;
	VpuDecOutFrameInfo *pfi;

	if(argc > 1) {
		// printf("  Usage : %s [file_path]\n", argv[0]);
		strcpy(filepath, argv[1]);
	} else {
		strcpy(filepath, dev_fle);
	}

	signal(SIGQUIT, exit_signal);
	signal(SIGINT,  exit_signal);
	signal(SIGPIPE, SIG_IGN);

	if(dec_init(160, 128, VPU_V_AVC) < 0) {
		printf("decodec init error\n");
		return 0;
	}

	if ((fd_fle = open(filepath, O_RDWR)) < 0) {
		printf("Unable to open [%s]\n", filepath);
		goto err;
	}

	if(display_init() < 0) {
		printf("display init error\n");
		goto err0;
	}

	printf("Stream starting... with fps=%d\n", FPS_VIDEO);

	gettimeofday(&ts, 0);

	do {
		if(bfeed) {
			rlen = read(fd_fle, rbuf, 4096);
			if(rlen <= 0) rlen = 0;
		} else {
			rlen = 0;
		}

		res = dec_stream(rbuf, rlen, &vdii);
		if((res & VPU_DEC_OUTPUT_DIS) || (res & VPU_DEC_OUTPUT_MOSAIC_DIS)) {
			pfi = dec_get_frame();

			// memcpy(buffers_out[0].vaddr, pfi->pDisplayFrameBuf->pbufVirtY,
				   // buffers_out[0].size);

#ifdef CSC_USE_IPU
			gtask.input.paddr = (int)pfi->pDisplayFrameBuf->pbufY;
			ioctl(fd_ipu, IPU_QUEUE_TASK, &gtask);
#else
			gsrc.planes[0] = (int)pfi->pDisplayFrameBuf->pbufY;
			g2d_blit(ghandler, &gsrc, &gdst);
			g2d_flush(ghandler);
#endif

			dec_put_frame();

			if(bfirst) {
				ioctl(fd_out, FBIOBLANK, FB_BLANK_UNBLANK);
				bfirst = false;
			}
		}

		if(res & VPU_DEC_OUTPUT_EOS)
			exitflag = 1;

		if(res & VPU_DEC_NO_ENOUGH_INBUF) {
			bfeed = false;
		} else {
			bfeed = true;
		}

		if(res < 0) {
			printf("dec_stream failed.\n");
			exitflag = 1;
		}

		gettimeofday(&te, 0);
		tus = (te.tv_sec - ts.tv_sec) * 1000000 + (te.tv_usec - ts.tv_usec);
		if(tus <= FRAME_DURATION) {
			if(!exitflag) usleep(FRAME_DURATION - tus);
		} else {
			// printf("Rander a frame takes %.3fms\n", tus / 1000.0);
		}

		// printf("Rander a frame takes %.3fms\n", tus / 1000.0);

		gettimeofday(&ts, 0);

	} while(!exitflag);

	printf("Stopping stream...\n");

err1:
	display_exit();
err0:
	close(fd_fle);
err:
	dec_exit();

	return 0;
}

int end()
{

}
