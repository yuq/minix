#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include <fcntl.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include "share.h"

static struct render_state state;

static int drm_fd;
static drmModeConnectorPtr connector = NULL;
static drmModeFBPtr fb;
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
	fb = drmModeGetFB(fd, crtc->buffer_id);
	assert(fb);

	drm_fd = fd;
	state.fd = fd;
	state.target_width = fb->width;
	state.target_height = fb->height;

	drmFree(encoder);
	drmFree(res);
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

static void composite(int fd)
{
	struct present_buffer data;
	int buffer_fd;

	ssize_t size = sock_fd_read(fd, &data, sizeof(data), &buffer_fd);
	assert(size > 0);

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

	epoxy_has_egl_extension(state.display, "EGL_KHR_image_pixmap");

	EGLImageKHR image = eglCreateImageKHR(
		state.display, state.context,
		EGL_NATIVE_PIXMAP_KHR, bo, NULL);
	assert(image != EGL_NO_IMAGE_KHR);

	glActiveTexture(GL_TEXTURE0);

	epoxy_has_gl_extension("GL_OES_EGL_image");

	GLuint texid;
	glGenTextures(1, &texid);
	glBindTexture(GL_TEXTURE_2D, texid);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);

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

	eglSwapBuffers(state.display, state.surface);
}

static void display_output(void)
{
	struct gbm_bo *bo = gbm_surface_lock_front_buffer(state.gs);
	assert(bo);

        uint32_t my_fb;
	assert(!drmModeAddFB(drm_fd, gbm_bo_get_width(bo),
			     gbm_bo_get_height(bo), 24,
			     gbm_bo_get_bpp(bo),
			     gbm_bo_get_stride(bo),
			     gbm_bo_get_handle(bo).u32,
			     &my_fb));

	// show my_fb
	assert(!drmModeSetCrtc(drm_fd, crtc->crtc_id, my_fb, 0, 0,
			       &connector->connector_id, 1, &crtc->mode));

	// hold on for a moment
	sleep(10);

	// restore previous fb
	assert(!drmModeSetCrtc(drm_fd, crtc->crtc_id, fb->fb_id, 0, 0,
			       &connector->connector_id, 1, &crtc->mode));

	gbm_surface_release_buffer(state.gs, bo);
}

void server_main(int fd)
{
	// init display
	display_init();

	// init render
	render_target_init(&state);
	init_gles(&state, vertex_shader, fragment_shader);

	// background color
	glClearColor(0.15, 0.15, 0.15, 0);

	// get client output and past on fb
	composite(fd);

	// show on screen
        display_output();
}
