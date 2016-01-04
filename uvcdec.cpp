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

#include "vpudec.h"


#define FPS_VIDEO		25
#define FRAME_DURATION	(1000000.0 / FPS_VIDEO)

#define CAP_BUFFERS		4
#define OUT_BUFFERS		1

#define CAP_WIDTH		1280
#define CAP_HEIGHT		720

#define CSC_USE_IPU		1		// comment this to use g2d do csc

struct video_buffer {
	unsigned char*	vaddr;
	size_t 			paddr;
	unsigned int 	size;
};

static const char dev_fb0[] = "/dev/fb0";
static const char dev_fb1[] = "/dev/fb1";
static const char dev_cap[] = "/dev/video0";
static const char dev_ipu[] = "/dev/mxc_ipu";

static int cx_screen = 0;
static int cy_screen = 0;
static int cx_capture = 0;
static int cy_capture = 0;
static int fd_cap = 0;
static int fd_out = 0;
static int fd_fle = 0;
static int exitflag = 0;		// indicate exit
static int framesize = 0;

static struct fb_var_screeninfo g_varinfo;

static struct video_buffer buffers_cap[CAP_BUFFERS];
static struct video_buffer buffers_out[OUT_BUFFERS];

#ifdef CSC_USE_IPU
static int fd_ipu;
static struct ipu_task gtask;
#else
static void *ghandler;
static struct g2d_surface gsrc;
static struct g2d_surface gdst;
#endif

static int map_buffers()
{
	unsigned int i;
	struct v4l2_buffer buf;

	for (i = 0; i < CAP_BUFFERS; i++) {
		memset(&buf, 0, sizeof (buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;

		if (ioctl(fd_cap, VIDIOC_QUERYBUF, &buf) < 0) {
			printf("cap-VIDIOC_QUERYBUF error\n");
			return -1;
		}

		buffers_cap[i].size = buf.length;
		buffers_cap[i].paddr = (size_t) buf.m.offset;
		buffers_cap[i].vaddr = (unsigned char *)
			mmap (NULL, buffers_cap[i].size, PROT_READ | PROT_WRITE,
						MAP_SHARED, fd_cap, buffers_cap[i].paddr);

		memset(&buf, 0, sizeof (buf));

		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;
		buf.m.offset = buffers_cap[i].paddr;

		if (ioctl (fd_cap, VIDIOC_QBUF, &buf) < 0) {
			printf("VIDIOC_QBUF error\n");
			return -1;
		}

		printf("mmap cap-buf : [%d] v=0x%08x, p=0x%08x, s=%d\n", i,
			buffers_cap[i].vaddr, buffers_cap[i].paddr, buffers_cap[i].size);
	}

	for (i = 0; i < OUT_BUFFERS; i++) {
		buffers_out[i].vaddr = (unsigned char *)
			mmap (NULL, buffers_out[i].size, PROT_READ | PROT_WRITE,
						MAP_SHARED, fd_out, i * buffers_out[i].size);

		if (buffers_out[i].vaddr == NULL || buffers_out[i].vaddr == (unsigned char *)-1) {
			printf("output memory[%d] mmap failed\n", i);
			return -1;
		}

		printf("mmap out-buf : [%d] v=0x%08x, p=0x%08x, s=%d\n", i,
			buffers_out[i].vaddr, buffers_out[i].paddr, buffers_out[i].size);
	}

	return 0;
}

static void umap_buffers()
{
	unsigned int i;

	for (i = 0; i < OUT_BUFFERS; i++)
		munmap(buffers_out[i].vaddr, buffers_out[i].size);

	for (i = 0; i < CAP_BUFFERS; i++)
		munmap(buffers_cap[i].vaddr, buffers_cap[i].size);
}

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

	memcpy(&g_varinfo, &vinfo0, sizeof(g_varinfo));
	// if (ioctl(fd_out, FBIOGET_VSCREENINFO, &g_varinfo)) {
		// printf("Error read vinfo.\n");
		// goto err;
	// }

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

	gtask.input.width = CAP_WIDTH;
	gtask.input.height = CAP_HEIGHT;
	gtask.input.format = IPU_PIX_FMT_YUV422P;

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
	gsrc.right = CAP_WIDTH;
	gsrc.bottom = CAP_HEIGHT;
	gsrc.width = CAP_WIDTH;
	gsrc.height = CAP_HEIGHT;
	gsrc.stride = CAP_WIDTH;
	gsrc.format = G2D_NV61;

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
#ifdef CSC_USE_IPU
	close(fd_ipu);
#else
	g2d_close(ghandler);
#endif
	close(fd_out);
}

static int setup_cap(void)
{
	// struct v4l2_dbg_chip_ident chip;
	struct v4l2_capability cap;
	struct v4l2_cropcap cropcap;
	struct v4l2_crop crop;
	struct v4l2_format fmt;
	struct v4l2_requestbuffers req;
	struct v4l2_streamparm parm;
	struct v4l2_fmtdesc fmtdesc;
	struct v4l2_frmsizeenum fsize;

	v4l2_std_id id;
	unsigned int min;
	unsigned int pixelfmt;

	// if (ioctl(fd_cap, VIDIOC_DBG_G_CHIP_IDENT, &chip)) {
		// printf("VIDIOC_DBG_G_CHIP_IDENT failed.\n");
		// return -1;
	// }
	// printf("sensor chip is [%s]\n", chip.match.name);

	if (ioctl (fd_cap, VIDIOC_QUERYCAP, &cap) < 0) {
		printf("%s is not V4L2 device\n", dev_cap);
		return -1;
	}

	if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
		printf("%s is no video capture device\n", dev_cap);
		return -1;
	}

	if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
		printf("%s does not support streaming i/o\n", dev_cap);
		return -1;
	}

	printf("capture driver : %s\n", cap.driver);

	// fmtdesc.index = 0;
	// fmtdesc.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	// while (!ioctl(fd_cap, VIDIOC_ENUM_FMT, &fmtdesc)) {
		// printf("fmt %s: fourcc = 0x%08x\n",
				// fmtdesc.description, fmtdesc.pixelformat);
		// fmtdesc.index++;
	// }

	// printf("sensor supported frame size:\n");
	// fsize.index = 0;
	// while (ioctl(fd_cap, VIDIOC_ENUM_FRAMESIZES, &fsize) >= 0) {
		// printf(" %dx%d\n", fsize.discrete.width,
					       // fsize.discrete.height);
		// fsize.index++;
	// }

	bzero(&cropcap, sizeof(cropcap));
	cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (0 == ioctl(fd_cap, VIDIOC_CROPCAP, &cropcap)) {
		crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		crop.c = cropcap.defrect;

		ioctl(fd_cap, VIDIOC_S_CROP, &crop);
	}

	// min = 1;
	// if (ioctl(fd_cap, VIDIOC_S_INPUT, &min) < 0)
	// {
		// printf("VIDIOC_S_INPUT failed\n");
		// return -1;
	// }

	parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	parm.parm.capture.timeperframe.numerator = 1;
	parm.parm.capture.timeperframe.denominator = FPS_VIDEO;
	if (ioctl(fd_cap, VIDIOC_S_PARM, &parm) < 0) {
		printf("VIDIOC_S_PARM failed\n");
		return -1;
	}

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ioctl (fd_cap, VIDIOC_G_FMT, &fmt) < 0){
		printf("%s get iformat failed\n", dev_cap);
		return -1;
	}

	pixelfmt = fmt.fmt.pix.pixelformat;
	printf(" === CAP GET [%d x %d] fmt=%c%c%c%c\n", fmt.fmt.pix.width,
		fmt.fmt.pix.height, pixelfmt, pixelfmt >> 8, pixelfmt >> 16, pixelfmt >> 24);


	memset(&fmt, 0, sizeof(fmt));
	fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width       = CAP_WIDTH;
	fmt.fmt.pix.height      = CAP_HEIGHT;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
	fmt.fmt.pix.field       = V4L2_FIELD_INTERLACED;
	if (ioctl (fd_cap, VIDIOC_S_FMT, &fmt) < 0){
		printf("%s iformat not supported \n", dev_cap);
		return -1;
	}

	min = fmt.fmt.pix.width * 2;
	if (fmt.fmt.pix.bytesperline < min)
		fmt.fmt.pix.bytesperline = min;

	min = fmt.fmt.pix.bytesperline * fmt.fmt.pix.height;
	if (fmt.fmt.pix.sizeimage < min)
		fmt.fmt.pix.sizeimage = min;

	if (ioctl(fd_cap, VIDIOC_G_FMT, &fmt) < 0) {
		printf("VIDIOC_G_FMT failed\n");
		return -1;
	}
	pixelfmt = fmt.fmt.pix.pixelformat;
	cx_capture = fmt.fmt.pix.width;
	cy_capture = fmt.fmt.pix.height;
	printf(" === CAP [%d x %d] sz=%d fmt=%c%c%c%c\n", cx_capture, cy_capture,
		fmt.fmt.pix.sizeimage, pixelfmt, pixelfmt >> 8, pixelfmt >> 16, pixelfmt >> 24);

	memset(&req, 0, sizeof (req));

	req.count	= CAP_BUFFERS;
	req.type	= V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory	= V4L2_MEMORY_MMAP;

	if (ioctl (fd_cap, VIDIOC_REQBUFS, &req) < 0) {
		printf("%s does not support memory mapping\n", dev_cap);
		return -1;
	}

	if (req.count < 2) {
		printf("Insufficient buffer memory on %s\n", dev_cap);
		return -1;
	}

	return 0;
}

static int stream_start()
{
	enum v4l2_buf_type type;

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ioctl (fd_cap, VIDIOC_STREAMON, &type) < 0) {
		printf("VIDIOC_STREAMON error\n");
		return -1;
	}

	return 0;
}

static void stream_stop()
{
	enum v4l2_buf_type type;

	ioctl(fd_out, FBIOBLANK, FB_BLANK_POWERDOWN);

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	ioctl(fd_cap, VIDIOC_STREAMOFF, &type);
}

static int frame_get(int *size)
{
	int res = 0;
	struct v4l2_buffer buffer;
	memset(&buffer, 0, sizeof(buffer));
	buffer.type 	= V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buffer.memory 	= V4L2_MEMORY_MMAP;

	res = ioctl(fd_cap, VIDIOC_DQBUF, &buffer);
	if (res < 0) {
		printf("cap-VIDIOC_DQBUF failed err=%d.\n", res);
		return -1;
	}
	*size = buffer.bytesused;
	return buffer.index;
}

static void frame_put(int idx_cap)
{
	struct v4l2_buffer buffer;
	memset(&buffer, 0, sizeof(buffer));
	buffer.type 	= V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buffer.memory 	= V4L2_MEMORY_MMAP;
	buffer.index 	= idx_cap;

	if (ioctl(fd_cap, VIDIOC_QBUF, &buffer) < 0)
		printf("VIDIOC_QBUF failed\n");
}

static void frame_render(int idx_cap, int fsize)
{
	static unsigned int xx = 0;

	int res = 0;
	VpuDecInitInfo vdii;
	VpuDecOutFrameInfo *pfi;
	res = dec_stream(buffers_cap[idx_cap].vaddr, fsize, &vdii);

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
	}

	// memcpy(buffers_out[0].vaddr, buffers_cap[idx_cap].vaddr, buffers_out[0].size);

	if(xx == 1) {
		ioctl(fd_out, FBIOBLANK, FB_BLANK_UNBLANK);
	}

	xx++;
}

static void exit_signal(int signo)
{
	exitflag = 1;
	printf("\n");
}

int main(int argc, char *argv[])
{
	int idx, tus, fsize;
	char capdev_str[128];
	unsigned long ss;
	struct timeval ts, te;

	if(argc > 1) {
		// printf("  Usage : %s [file_path]\n", argv[0]);
		strcpy(capdev_str, argv[1]);
	} else {
		strcpy(capdev_str, dev_cap);
	}

	signal(SIGQUIT, exit_signal);
	signal(SIGINT,  exit_signal);
	signal(SIGPIPE, SIG_IGN);

	if(display_init() < 0) {
		printf("display init error\n");
		return 0;
	}

	if(dec_init(CAP_WIDTH, CAP_HEIGHT, VPU_V_MJPG) < 0) {
		printf("decodec init error\n");
		goto err;
	}

	if ((fd_cap = open(capdev_str, O_RDWR)) < 0) {
		printf("Unable to open [%s]\n", capdev_str);
		goto err0;
	}

	if (setup_cap() < 0) {
		printf("Unable to setup capture\n");
		goto err1;
	}

	printf("capture device setup done\n");

	if (map_buffers() < 0) {
		printf("Unable to map I/O memory\n");
		goto err1;
	}

	printf("buffers mapping done\n");

	if (stream_start() < 0) {
		printf("Unable to start stream\n");
		goto err2;
	}

	printf("Stream starting... with fps=%d\n", FPS_VIDEO);

	gettimeofday(&ts, 0);

	do {
		idx = frame_get(&fsize);
		frame_render(idx, fsize);
		frame_put(idx);
		gettimeofday(&te, 0);
		tus = (te.tv_sec - ts.tv_sec) * 1000000 + (te.tv_usec - ts.tv_usec);
		if(tus <= FRAME_DURATION) {
			if(!exitflag) usleep(FRAME_DURATION - tus);
		} else {
			// printf("rander a frame with %.3fms\n", tus / 1000.0);
		}

		printf("Rander a frame sz = %d, takes %.3fms\n", fsize, tus / 1000.0);

		gettimeofday(&ts, 0);

	} while(!exitflag);

	printf("Stopping stream...\n");

	stream_stop();
err2:
	umap_buffers();
err1:
	close(fd_cap);
err0:
	dec_exit();
err:
	display_exit();

	return 0;
}

int end()
{

}
