#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include <fcntl.h>
#include <unistd.h>

#include <gbm.h>
#include <png.h>

#include <epoxy/gl.h>
#include <epoxy/egl.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

GLuint program;
EGLDisplay display;
EGLSurface surface;
EGLContext context;
struct gbm_device *gbm;
struct gbm_surface *gs;

int drm_fd;
drmModeConnectorPtr connector = NULL;
drmModeFBPtr fb;
drmModeCrtcPtr crtc;
int display_width;
int display_height;

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
	display_width = fb->width;
	display_height = fb->height;

	drmFree(encoder);
	drmFree(res);
}

static EGLConfig get_config(void)
{
	EGLint egl_config_attribs[] = {
		EGL_BUFFER_SIZE,	32,
		EGL_DEPTH_SIZE,		EGL_DONT_CARE,
		EGL_STENCIL_SIZE,	EGL_DONT_CARE,
		EGL_RENDERABLE_TYPE,	EGL_OPENGL_ES2_BIT,
		EGL_SURFACE_TYPE,	EGL_WINDOW_BIT,
		EGL_NONE,
	};

	EGLint num_configs;
	assert(eglGetConfigs(display, NULL, 0, &num_configs) == EGL_TRUE);

	EGLConfig *configs = malloc(num_configs * sizeof(EGLConfig));
	assert(eglChooseConfig(display, egl_config_attribs,
			       configs, num_configs, &num_configs) == EGL_TRUE);
	assert(num_configs);
	printf("num config %d\n", num_configs);

	// Find a config whose native visual ID is the desired GBM format.
	for (int i = 0; i < num_configs; ++i) {
		EGLint gbm_format;

		assert(eglGetConfigAttrib(display, configs[i],
					  EGL_NATIVE_VISUAL_ID, &gbm_format) == EGL_TRUE);
		printf("gbm format %x\n", gbm_format);

		if (gbm_format == GBM_FORMAT_ARGB8888) {
			EGLConfig ret = configs[i];
			free(configs);
			return ret;
		}
	}

	// Failed to find a config with matching GBM format.
	abort();
}

static void render_target_init(void)
{
	assert(epoxy_has_egl_extension(EGL_NO_DISPLAY, "EGL_MESA_platform_gbm"));

	gbm = gbm_create_device(drm_fd);
	assert(gbm != NULL);

	display = eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_MESA, gbm, NULL);
	assert(display != EGL_NO_DISPLAY);

	EGLint majorVersion;
	EGLint minorVersion;
	assert(eglInitialize(display, &majorVersion, &minorVersion) == EGL_TRUE);

	assert(eglBindAPI(EGL_OPENGL_ES_API) == EGL_TRUE);

	EGLConfig config = get_config();

	gs = gbm_surface_create(
		gbm, display_width, display_height, GBM_BO_FORMAT_ARGB8888,
		GBM_BO_USE_LINEAR|GBM_BO_USE_SCANOUT|GBM_BO_USE_RENDERING);
	assert(gs);

	surface = eglCreatePlatformWindowSurfaceEXT(display, config, gs, NULL);
	assert(surface != EGL_NO_SURFACE);

	const EGLint contextAttribs[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};
	context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttribs);
	assert(context != EGL_NO_CONTEXT);

	assert(eglMakeCurrent(display, surface, surface, context) == EGL_TRUE);
}

static GLuint compile_shader(const char *source, GLenum type)
{
	GLuint shader;
	GLint compiled;

	shader = glCreateShader(type);
	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);

	glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
	if (!compiled) {
		GLint infoLen = 0;
		glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLen);
		if (infoLen > 1) {
			char *infoLog = malloc(infoLen);
			glGetShaderInfoLog(shader, infoLen, NULL, infoLog);
			fprintf(stderr, "Error compiling shader:\n%s\n", infoLog);
			free(infoLog);
		}
		glDeleteShader(shader);
		return 0;
	}

	return shader;
}

static const char vertex_shader[] =
	"attribute vec3 positionIn;"
	"void main() {"
	"    gl_Position = vec4(positionIn, 1);"
	"}";

static const char fragment_shader[] =
	"void main() {"
	"    gl_FragColor = vec4(1.0, 0.0, 0.0, 1);"
	"}";

static void init_gles(void)
{
	GLint linked;
	GLuint vertexShader;
	GLuint fragmentShader;
	assert((vertexShader = compile_shader(vertex_shader, GL_VERTEX_SHADER)) != 0);
	assert((fragmentShader = compile_shader(fragment_shader, GL_FRAGMENT_SHADER)) != 0);
	assert((program = glCreateProgram()) != 0);
	glAttachShader(program, vertexShader);
	glAttachShader(program, fragmentShader);
	glLinkProgram(program);
	glGetProgramiv(program, GL_LINK_STATUS, &linked);
	if (!linked) {
		GLint infoLen = 0;
		glGetProgramiv(program, GL_INFO_LOG_LENGTH, &infoLen);
		if (infoLen > 1) {
			char *infoLog = malloc(infoLen);
			glGetProgramInfoLog(program, infoLen, NULL, infoLog);
			fprintf(stderr, "Error linking program:\n%s\n", infoLog);
			free(infoLog);
		}
		glDeleteProgram(program);
		exit(1);
	}

	glClearColor(0, 0, 0, 0);
	glViewport(0, 0, display_width, display_height);

	glUseProgram(program);
}

static void render(void)
{
	GLfloat vertex[] = {
		-1, -1, 0,
		-1, 1, 0,
		1, 1, 0,
	};

	GLint position = glGetAttribLocation(program, "positionIn");
	glEnableVertexAttribArray(position);
	glVertexAttribPointer(position, 3, GL_FLOAT, 0, 0, vertex);

	glClear(GL_COLOR_BUFFER_BIT);

	glDrawArrays(GL_TRIANGLES, 0, 3);

	eglSwapBuffers(display, surface);
}

static void display_output(void)
{
	struct gbm_bo *bo = gbm_surface_lock_front_buffer(gs);
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

	gbm_surface_release_buffer(gs, bo);
}

int main(void)
{
	// init display
	display_init();

	// render to fb
	render_target_init();
	init_gles();
	render();

	// show on screen
        display_output();

	return 0;
}
