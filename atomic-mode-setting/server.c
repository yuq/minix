#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/epoll.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include "share.h"

static struct render_state state;

static int drm_fd;
static drmModeConnectorPtr connector = NULL;
static drmModeFBPtr orig_fb;
static drmModeCrtcPtr crtc;

static void display_init(void)
{
	int fd = open("/dev/dri/card0", O_RDWR);
	assert(fd >= 0);

	drmModeResPtr res = drmModeGetResources(fd);
	assert(res);

	for (int i = 0; i < res->count_connectors; i++) {
		connector = drmModeGetConnector(fd, res->connectors[i]);
		assert(connector);

		// find a connected connection
		if (connector->connection == DRM_MODE_CONNECTED)
			break;

		drmFree(connector);
		connector = NULL;
	}
	assert(connector);

	drmModeEncoderPtr encoder = drmModeGetEncoder(fd, connector->encoder_id);
	assert(encoder);

	crtc = drmModeGetCrtc(fd, encoder->crtc_id);
	assert(crtc);

	// original fb used for terminal
	orig_fb = drmModeGetFB(fd, crtc->buffer_id);
	assert(orig_fb);

	drm_fd = fd;
	state.fd = fd;
	state.target_width = orig_fb->width;
	state.target_height = orig_fb->height;

	drmFree(encoder);
	drmFree(res);
}

static uint32_t plane_id = 0;
static uint32_t property_fb_id = 0;
static uint32_t property_in_fence_fd = 0;
static uint32_t property_out_fence_ptr = 0;

static void atomic_mode_setting_init(void)
{
	// enable atomic mode setting
	assert(!drmSetClientCap(drm_fd, DRM_CLIENT_CAP_ATOMIC, 1));

	drmModePlaneRes *plane_res = drmModeGetPlaneResources(drm_fd);
	assert(plane_res);
	// find plane used by target crtc
	for (int i = 0; i < plane_res->count_planes; i++) {
		drmModePlanePtr plane = drmModeGetPlane(drm_fd, plane_res->planes[i]);
		if (plane->crtc_id == crtc->crtc_id)
			plane_id = plane->plane_id;
		drmModeFreePlane(plane);
		if (plane_id)
			break;
	}
	drmModeFreePlaneResources(plane_res);
	assert(plane_id);

	drmModeObjectProperties *props;
	drmModePropertyPtr property;

	props = drmModeObjectGetProperties(drm_fd, plane_id, DRM_MODE_OBJECT_PLANE);
	assert(props);
	// find property according to the name
	for (int i = 0; i < props->count_props; i++) {
		property = drmModeGetProperty(drm_fd, props->props[i]);
		if (!strcmp(property->name, "FB_ID"))
			property_fb_id = property->prop_id;
		else if (!strcmp(property->name, "IN_FENCE_FD"))
			property_in_fence_fd = property->prop_id;
		drmModeFreeProperty(property);
	}
	drmModeFreeObjectProperties(props);
	assert(property_fb_id);
	assert(property_in_fence_fd);

	props = drmModeObjectGetProperties(drm_fd, crtc->crtc_id, DRM_MODE_OBJECT_CRTC);
	assert(props);
	for (int i = 0; i < props->count_props; i++) {
		property = drmModeGetProperty(drm_fd, props->props[i]);
		if (!strcmp(property->name, "OUT_FENCE_PTR"))
			property_out_fence_ptr = property->prop_id;
		drmModeFreeProperty(property);
	}
	drmModeFreeObjectProperties(props);
	assert(property_out_fence_ptr);
}

static const char vertex_shader[] =
	"attribute vec3 positionIn;\n"
	"attribute vec2 texcoordIn;\n"
	"varying vec2 texcoord;\n"
	"void main()\n"
	"{\n"
	"    gl_Position = vec4(positionIn, 1);\n"
	"    texcoord = texcoordIn;\n"
	"}\n";

static const char fragment_shader[] =
	"precision mediump float;\n"
	"uniform sampler2D texMap;\n"
	"varying vec2 texcoord;\n"
	"void main() {\n"
	"    gl_FragColor = texture2D(texMap, texcoord);\n"
	"}\n";

static int composite(int fd)
{
	struct present_buffer data;
	int fds[2];
	int num_fd = 2;

	// get new window frame from client
	ssize_t size = sock_fd_read(fd, &data, sizeof(data), fds, &num_fd);
	assert(size > 0);
	assert(num_fd >= 1);

	int buffer_fd = fds[0];
	int wait_fd = num_fd == 2 ? fds[1] : -1;

	struct gbm_import_fd_data gbm_data = {
		.fd = buffer_fd,
		.width = data.width,
		.height = data.height,
		.stride = data.stride,
		.format = data.format,
	};

	struct gbm_bo *bo = gbm_bo_import(
		state.gbm, GBM_BO_IMPORT_FD,
		&gbm_data, GBM_BO_USE_RENDERING);
	assert(bo);

	// close after usage
	close(buffer_fd);

	epoxy_has_egl_extension(state.display, "EGL_KHR_image_pixmap");

	EGLImageKHR image = eglCreateImageKHR(
		state.display, state.context,
		EGL_NATIVE_PIXMAP_KHR, bo, NULL);
	assert(image != EGL_NO_IMAGE_KHR);

	// destroy after usage
	gbm_bo_destroy(bo);

	glActiveTexture(GL_TEXTURE0);

	epoxy_has_gl_extension("GL_OES_EGL_image");

	GLuint texid;
	glGenTextures(1, &texid);
	glBindTexture(GL_TEXTURE_2D, texid);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);

	// destroy after usage
	eglDestroyImageKHR(state.display, image);

	// wait client render is done before composite
	if (wait_fd >= 0) {
		wait_fence(state.display, wait_fd);
		close(wait_fd);
	}

	GLfloat x = -1.0 + data.x * 2.0 / state.target_width;
	GLfloat y = 1.0 - data.y * 2.0 / state.target_height;
	GLfloat w = x + data.width * 2.0 / state.target_width;
	GLfloat h = y - data.height * 2.0 / state.target_height;
		
	GLfloat vertex[] = {
		x, h, 0,
		x, y, 0,
		w, y, 0,
		w, h, 0,
	};

	GLfloat texcoord[] = {
		0, 1,
		0, 0,
		1, 0,
		1, 1,
	};

	GLushort index[] = {
		0, 1, 3,
		1, 2, 3,
	};

	GLint pos = glGetAttribLocation(state.program, "positionIn");
        glEnableVertexAttribArray(pos);
	glVertexAttribPointer(pos, 3, GL_FLOAT, 0, 0, vertex);

	GLint tex = glGetAttribLocation(state.program, "texcoordIn");
        glEnableVertexAttribArray(tex);
	glVertexAttribPointer(tex, 2, GL_FLOAT, 0, 0, texcoord);

	GLint texMap = glGetUniformLocation(state.program, "texMap");
	glUniform1i(texMap, 0); // GL_TEXTURE0

	glClear(GL_COLOR_BUFFER_BIT);

	glDrawElements(GL_TRIANGLES, sizeof(index)/sizeof(GLushort), GL_UNSIGNED_SHORT, index);

	// after composite is done, this fence will be signaled
	int signal_fd = get_fence(state.display);

	// swap back buffer to front
	eglSwapBuffers(state.display, state.surface);

	// delete after usage
	glDeleteTextures(1, &texid);

	// infom client window frame has been consumed
	struct present_done done = {
		.index = data.index,
	};
	if (signal_fd >= 0)
		size = sock_fd_write(fd, &done, sizeof(done), &signal_fd, 1);
	else
		size = sock_fd_write(fd, &done, sizeof(done), NULL, 0);
	assert(size > 0);

	return signal_fd;
}

struct display_framebuffer {
	struct gbm_bo *bo;
	uint32_t fb_id;
	struct display_framebuffer *next;
	int wait_fd;
};

// assume max number of bos in a gbm_surface is less than 32
#define MAX_BOS 32
struct display_framebuffer fbs[MAX_BOS] = {0};
// fb which is on screen
struct display_framebuffer *showing_fb = NULL;
// fbs pending to be show on screen
struct display_framebuffer *pending_fbs = NULL;

static void atomic_page_flip(struct display_framebuffer *fb)
{
	drmModeAtomicReq *req;

	req = drmModeAtomicAlloc();
	assert(req);

	assert(drmModeAtomicAddProperty(req, plane_id, property_fb_id,
					fb->fb_id) >= 0);

	assert(drmModeAtomicAddProperty(req, plane_id, property_in_fence_fd,
					fb->wait_fd) >= 0);

	assert(!drmModeAtomicCommit(drm_fd, req,
				    DRM_MODE_PAGE_FLIP_EVENT |
				    DRM_MODE_ATOMIC_NONBLOCK,
				    NULL));

	drmModeAtomicFree(req);

	if (fb->wait_fd >= 0)
		close(fb->wait_fd);
}

static void display_output(int wait_fd)
{
	struct gbm_bo *bo = gbm_surface_lock_front_buffer(state.gs);
	assert(bo);

	// find existing framebuffer, create one if not exist
	struct display_framebuffer *fb = NULL;
	for (int i = 0; i < MAX_BOS; i++) {
		if (!fbs[i].bo || fbs[i].bo == bo) {
			fb = fbs + i;
			break;
		}
	}

	if (!fb->bo) {
		assert(!drmModeAddFB(drm_fd, gbm_bo_get_width(bo),
				     gbm_bo_get_height(bo), 24,
				     gbm_bo_get_bpp(bo),
				     gbm_bo_get_stride(bo),
				     gbm_bo_get_handle(bo).u32,
				     &fb->fb_id));
		fb->bo = bo;
	}

	fb->wait_fd = wait_fd;
	fb->next = NULL;
	if (!pending_fbs) {
		// need to kick start page flip first time
		atomic_page_flip(fb);
		pending_fbs = fb;
	} else {
		// pend page flip request will be consumed by drm event handler
		struct display_framebuffer *pfb = pending_fbs;
		// queue request to list tail
		while (pfb->next) pfb = pfb->next;
		pfb->next = fb;
	}
}

static void
page_flip_handler(int fd, uint32_t frame, uint32_t sec, uint32_t usec,
		  void *user_ptr)
{
	assert(pending_fbs);

	// release replaced previous showing framebuffer
	if (showing_fb)
		gbm_surface_release_buffer(state.gs, showing_fb->bo);

	showing_fb = pending_fbs;
	pending_fbs = pending_fbs->next;
	if (pending_fbs)
		atomic_page_flip(pending_fbs);
}

static int dispatch_fd;

static void dispatch_add(int fd)
{
	struct epoll_event event = {
		.events = EPOLLIN,
		.data.fd = fd,
	};
	assert(!epoll_ctl(dispatch_fd, EPOLL_CTL_ADD, fd, &event));
}

static void dispatch_remove(int fd)
{
	assert(!epoll_ctl(dispatch_fd, EPOLL_CTL_DEL, fd, NULL));
}

static bool stop = false;

static void sigint_handler(int arg)
{
	stop = true;
}

void server_main(int fd)
{
	// register CTRL+C terminate interrupt
	signal(SIGINT, sigint_handler);

	// init display
	display_init();

	// init atomic modesetting
	atomic_mode_setting_init();

	// init render
	render_target_init(&state);
	init_gles(&state, vertex_shader, fragment_shader);

	// background color
	glClearColor(0.15, 0.15, 0.15, 0);

	dispatch_fd = epoll_create1(0);
	assert(dispatch_fd >= 0);

	dispatch_add(drm_fd);
	dispatch_add(fd);

	bool client_added = true;
	while (!stop) {
		struct epoll_event events[64];

		int n = epoll_wait(dispatch_fd, events, 64, -1);
		if (n < 0)
			break;

		for (int i = 0; i < n; i++) {
			int efd = events[i].data.fd;

			assert(events[i].events == EPOLLIN);

			if (efd == drm_fd) {
				drmEventContext ev = {
					.version = DRM_EVENT_CONTEXT_VERSION,
					.page_flip_handler = page_flip_handler,
				};
				assert(!drmHandleEvent(efd, &ev));
			} else if (efd == fd) {
				// get client output and past on fb
				int signal_fd = composite(fd);

				// show on screen
				display_output(signal_fd);
			} else {
				fprintf(stderr, "invalid epoll event fd %d\n", efd);
				exit(1);
			}
		}

		if (gbm_surface_has_free_buffers(state.gs)) {
			if (!client_added) {
				dispatch_add(fd);
				client_added = true;
			}
		} else {
			// does not handle new client request if no free framebuffer
			if (client_added) {
				dispatch_remove(fd);
				client_added = false;
			}
		}
	}

	// restore previous fb
	assert(!drmModeSetCrtc(drm_fd, crtc->crtc_id, orig_fb->fb_id, 0, 0,
			       &connector->connector_id, 1, &crtc->mode));
}
