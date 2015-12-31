#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#include <unistd.h>
#include <fcntl.h>

#include <asm/types.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include <linux/ipu.h>
#include <linux/fb.h>
#include <linux/mxcfb.h>
#include <linux/videodev2.h>

#include <signal.h>
#include <pthread.h>

#define FPS_VIDEO		25
#define FRAME_DURATION	1000000.0 / FPS_VIDEO
#define CAP_BUFFERS		4
#define OUT_BUFFERS		1

#define CAP_MODE		0
#define CAP_WIDTH		640
#define CAP_HEIGHT		480

static const char dev_cap[] = "/dev/video0";
static const char dev_fb0[] = "/dev/fb0";
static const char dev_out[] = "/dev/fb1";

static int xx_screen = 0;
static int yy_screen = 0;
static int cx_screen = 0;
static int cy_screen = 0;
static int cx_capture = 0;
static int cy_capture = 0;
static int fd_cap = 0;
static int fd_out = 0;
static int fd_ipu = 0;
static struct ipu_task gtask;

static int exitflag = 0;		// indicate exit
static bool boverlay = false;

struct video_buffer
{
	unsigned char *start;
	size_t offset;
	unsigned int length;
};

static struct video_buffer buffers_cap[CAP_BUFFERS];
static struct video_buffer buffers_out[OUT_BUFFERS];
static struct video_buffer srcbuf;

static int map_buffers()
{
	unsigned int i;
	struct v4l2_buffer buf;

	for (i = 0; i < CAP_BUFFERS; i++) {
		buffers_cap[i].start = NULL;

		memset(&buf, 0, sizeof (buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;
		if (ioctl(fd_cap, VIDIOC_QUERYBUF, &buf) < 0) {
			printf("cap-VIDIOC_QUERYBUF error : '%s'\n", strerror(errno));
			break;
		}

		buffers_cap[i].offset = buf.m.offset;
		buffers_cap[i].length = buf.length;
		buffers_cap[i].start = (unsigned char *)
			mmap (NULL, buffers_cap[i].length, PROT_READ | PROT_WRITE,
				  MAP_SHARED, fd_cap, buffers_cap[i].offset);

		if(buffers_cap[i].start == MAP_FAILED) {
			printf("mmap %d error : '%s'\n", i, strerror(errno));
			break;
		}

		memset(&buf, 0, sizeof (buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;
		buf.length = buffers_cap[i].length;
		buf.m.offset = buffers_cap[i].offset;
		if (ioctl (fd_cap, VIDIOC_QBUF, &buf) < 0) {
			printf("VIDIOC_QBUF error : '%s'\n", strerror(errno));
			break;
		}

		printf("mmap cap-buf : [%d] start=0x%08x, offset=0x%08x, len=0x%08x\n", i,
			buffers_cap[i].start, buffers_cap[i].offset, buffers_cap[i].length);
	}

	if(i >= CAP_BUFFERS) {
		srcbuf.length = buffers_cap[0].length;
		srcbuf.offset = buffers_cap[0].length;
		if(ioctl(fd_ipu, IPU_ALLOC, &srcbuf.offset) < 0) {
			printf("IPU_ALLOC error : '%s'\n", strerror(errno));
			i = CAP_BUFFERS - 1;
		} else {
			srcbuf.start = (unsigned char *) mmap (NULL, srcbuf.length,
				PROT_READ | PROT_WRITE, MAP_SHARED, fd_ipu, srcbuf.offset);

			if(buffers_cap[i].start == MAP_FAILED) {
				ioctl(fd_ipu, IPU_FREE, &srcbuf.offset);
				printf("ipu mmap error : '%s'\n", strerror(errno));
				i = CAP_BUFFERS - 1;
			}
		}
	}

	if(i < CAP_BUFFERS) {
		for (; i >= 0; i--) {
			if(buffers_cap[i].start && buffers_cap[i].start != MAP_FAILED)
				munmap(buffers_cap[i].start, buffers_cap[i].length);
		}
		return -1;
	}

	return 0;
}

static void umap_buffers()
{
	unsigned int i;
	ioctl(fd_ipu, IPU_FREE, &srcbuf.offset);
	for (i = 0; i < CAP_BUFFERS; i++) {
		munmap(buffers_cap[i].start, buffers_cap[i].length);
	}

}

static int display_init(const char *disp_dev)
{
	int fd_fb0, bpp;
	struct mxcfb_gbl_alpha alpha;
	struct fb_var_screeninfo vinfo;
	struct fb_fix_screeninfo finfo;

	if ((fd_fb0 = open(dev_fb0, O_RDWR)) < 0) {
		printf("Unable to open '%s' res=%d\n", dev_fb0, fd_fb0);
		return -1;
	}

	alpha.enable = 1;
	alpha.alpha = 0;

	if (ioctl(fd_fb0, MXCFB_SET_GBL_ALPHA, &alpha) < 0) {
		printf("Set global alpha failed\n");
		close(fd_fb0);
		return -1;
	}

	if (ioctl(fd_fb0, FBIOGET_VSCREENINFO, &vinfo)) {
		printf("Error reading screen vinfo.\n");
		close(fd_fb0);
		return -1;
	}

	close(fd_fb0);

	cx_screen = vinfo.xres;
	cy_screen = vinfo.yres;

	if ((fd_out = open(disp_dev, O_RDWR)) < 0) {
		printf("Unable to open '%s' res=%d\n", disp_dev, fd_out);
		return -1;
	}

	if (ioctl(fd_out, FBIOGET_VSCREENINFO, &vinfo)) {
		printf("Error reading %s vinfo.\n", disp_dev);
		close(fd_out);
		return -1;
	}

	vinfo.xres = cx_screen;
	vinfo.yres = cy_screen;
	vinfo.yres_virtual = OUT_BUFFERS * vinfo.yres;
	vinfo.yoffset = 0;

	if (ioctl(fd_out, FBIOPUT_VSCREENINFO, &vinfo)) {
		printf("Error write %s vinfo\n", disp_dev);
		close(fd_out);
		return -1;
	}

	bpp = vinfo.bits_per_pixel;
	printf(" ==== OUT [%d x %d] bpp=%d\n", cx_screen, cy_screen, bpp);

	if (ioctl(fd_out, FBIOGET_FSCREENINFO, &finfo)) {
		printf("Error reading %s finfo\n", disp_dev);
		close(fd_out);
		return -1;
	}

	int pixsize = finfo.line_length * vinfo.yres;

	for(int i = 0; i < OUT_BUFFERS; ++i) {
		buffers_out[i].length = pixsize;
		buffers_out[i].offset = finfo.smem_start + i * pixsize;
	}

	// if (ioctl(fd_out, FBIOPAN_DISPLAY, &vinfo)) {
		// printf("Error reading variable infomation.\n");
		// close(fd_out);
		// return -1;
	// }

	fd_ipu = open("/dev/mxc_ipu", O_RDWR);
	if(fd_ipu < 0) {
		printf("open mxc_ipu failed\n");
		close(fd_out);
		return -1;
	}

	bzero(&gtask, sizeof gtask);
	gtask.input.width = cx_capture;
	gtask.input.height = cy_capture;
	gtask.input.format = IPU_PIX_FMT_YUYV;
	gtask.output.paddr = buffers_out[0].offset;
	gtask.output.width = cx_screen;
	gtask.output.height = cy_screen;
	// gtask.output.crop.pos.x = 0;
	// gtask.output.crop.pos.y = 0;
	// gtask.output.crop.w = cx_screen;
	// gtask.output.crop.h = cy_screen;
	if(bpp == 16)
		gtask.output.format = IPU_PIX_FMT_RGB565;
	else
		gtask.output.format = IPU_PIX_FMT_BGRA32;

	return 0;
}

static void display_exit()
{
	close(fd_ipu);
	close(fd_out);
}

static int setup_cap(void)
{
	int sfmt;
	struct v4l2_capability cap;
	struct v4l2_cropcap cropcap;
	struct v4l2_crop crop;
	struct v4l2_format fmt;
	struct v4l2_requestbuffers req;
	struct v4l2_streamparm parm;
	struct v4l2_frmsizeenum fsize;

	v4l2_std_id id;
	unsigned int min;

	// struct v4l2_dbg_chip_ident chip;
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

	// printf("sensor supported frame size:\n");
	// fsize.index = 0;
	// while (ioctl(fd_cap, VIDIOC_ENUM_FRAMESIZES, &fsize) >= 0) {
		// printf("    %d x %d\n", fsize.discrete.width,
					       // fsize.discrete.height);
		// fsize.index++;
	// }

	// struct v4l2_fmtdesc fmtdesc;
	// fmtdesc.index = 0;
	// fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	// while (!ioctl(fd_cap, VIDIOC_ENUM_FMT, &fmtdesc)) {
		// printf("fmt %s: fourcc = 0x%08x [%c%c%c%c]\n",
				// fmtdesc.description, fmtdesc.pixelformat,
				// fmtdesc.pixelformat & 0xFF, fmtdesc.pixelformat >> 8,
				// fmtdesc.pixelformat >> 16, fmtdesc.pixelformat >> 24);
		// fmtdesc.index++;
	// }

	// min = 0;
	// if (ioctl(fd_cap, VIDIOC_S_INPUT, &min) < 0) {
		// printf("VIDIOC_S_INPUT failed\n");
		// return -1;
	// }

	parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	parm.parm.capture.timeperframe.numerator = 1;
	parm.parm.capture.timeperframe.denominator = FPS_VIDEO;
	parm.parm.capture.capturemode = CAP_MODE;
	if (ioctl(fd_cap, VIDIOC_S_PARM, &parm) < 0) {
		printf("VIDIOC_S_PARM failed\n");
		return -1;
	}

	bzero(&parm, sizeof parm);
	parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ioctl(fd_cap, VIDIOC_G_PARM, &parm) < 0) {
		printf("VIDIOC_S_PARM failed\n");
		return -1;
	}

	printf("capture fps = %d/%d\n", parm.parm.capture.timeperframe.denominator,
		parm.parm.capture.timeperframe.numerator);

	memset(&fmt, 0, sizeof(fmt));
	fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width       = CAP_WIDTH;
	fmt.fmt.pix.height      = CAP_HEIGHT;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	if (ioctl (fd_cap, VIDIOC_S_FMT, &fmt) < 0){
		printf("%s iformat not supported \n", dev_cap);
		return -1;
	}

	/* Note VIDIOC_S_FMT may change width and height. */

	/* Buggy driver paranoia. */
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

	cx_capture = fmt.fmt.pix.width;
	cy_capture = fmt.fmt.pix.height;
	sfmt = fmt.fmt.pix.pixelformat;
	printf(" ==== CAP [%d x %d] [%d] fmt=%c%c%c%c\n", cx_capture, cy_capture,
		fmt.fmt.pix.sizeimage, sfmt, sfmt >> 8, sfmt >> 16, sfmt >> 24);

	memset(&req, 0, sizeof (req));

	req.type	= V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.count	= CAP_BUFFERS;
	req.memory	= V4L2_MEMORY_MMAP;
	if (ioctl (fd_cap, VIDIOC_REQBUFS, &req) < 0) {
		printf("VIDIOC_REQBUFS error : '%s'\n", strerror(errno));
		return -1;
	}

	// if (req.count < 2) {
		// printf("Insufficient buffer memory on %s\n", dev_cap);
		// return -1;
	// }

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

	if (ioctl (fd_out, FBIOBLANK, FB_BLANK_UNBLANK) < 0) {
		printf("FB_BLANK_UNBLANK error : %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

static void stream_stop()
{
	if (ioctl (fd_out, FBIOBLANK, FB_BLANK_NORMAL) < 0)
		printf("FB_BLANK_NORMAL error : %s\n", strerror(errno));

	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	ioctl(fd_cap, VIDIOC_STREAMOFF, &type);
}

static int frame_get()
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

static void frame_render(int idx_cap)
{
	gtask.input.paddr = srcbuf.offset;
	memcpy(srcbuf.start, buffers_cap[idx_cap].start, srcbuf.length);
	if (ioctl(fd_ipu, IPU_QUEUE_TASK, &gtask) < 0) {
		printf("ioct IPU_QUEUE_TASK fail\n");
	}
}

static void exit_signal(int signo)
{
	exitflag = 1;
	printf("\n");
}

int main(int argc, char *argv[])
{
	int idx, tus;
	unsigned long ss;
	struct timeval ts, te;
	char cappath[128];

	if(argc >= 2) {
		strcpy(cappath, argv[1]);
	} else {
		strcpy(cappath, dev_cap);
	}

	signal(SIGQUIT, exit_signal);
	signal(SIGINT,  exit_signal);
	signal(SIGPIPE, SIG_IGN);

	if ((fd_cap = open(cappath, O_RDWR)) < 0) {
		printf("Unable to open [%s]\n", cappath);
		return 1;
	}

	if (setup_cap() < 0) {
		printf("Unable to setup capture\n");
		goto err0;
	}

	printf("capture device setup done\n");
	
	if(display_init(dev_out) < 0) {
		printf("display_init err\n");
		goto err0;
	}

	if (map_buffers() < 0) {
		printf("Unable to map v4l2 memory\n");
		goto err1;
	}

	printf("buffers mapping done\n");

	if (stream_start() < 0) {
		printf("Unable to start stream\n");
		goto err2;
	}

	printf("Stream starting... with fps=%d\n", FPS_VIDEO);

	static int xxx = 0;

	gettimeofday(&ts, 0);

	do {
		idx = frame_get();
		frame_render(idx);
		frame_put(idx);
		gettimeofday(&te, 0);
		tus = (te.tv_sec - ts.tv_sec) * 1000000 + (te.tv_usec - ts.tv_usec);

		if(tus <= FRAME_DURATION) {
			if(!exitflag) usleep(FRAME_DURATION - tus);
		} else {
			printf("rander a frame with %.3fms\n", tus / 1000.0);
		}

		gettimeofday(&ts, 0);

	} while(!exitflag);

	printf("Stopping stream...\n");

	stream_stop();
err2:
	umap_buffers();
err1:
	display_exit();
err0:
	close(fd_cap);

	return 0;
}
