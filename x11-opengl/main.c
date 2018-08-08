#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include <fcntl.h>
#include <unistd.h>

#include <gbm.h>
#include <png.h>

#include <epoxy/gl.h>
#include <epoxy/egl.h>

#include <X11/Xlib.h>

GLuint program;
EGLDisplay display;
EGLSurface surface;
EGLContext context;

Display *dpy;
Window window;

#define TARGET_SIZE 256

static void xinit(void)
{
	assert((dpy = XOpenDisplay(NULL)) != NULL);

	int screen = DefaultScreen(dpy);
	Window root = DefaultRootWindow(dpy);
	window = XCreateWindow(dpy, root, 0, 0, TARGET_SIZE, TARGET_SIZE, 0,
			       DefaultDepth(dpy, screen), InputOutput,
			       DefaultVisual(dpy, screen), 
			       0, NULL);
	XMapWindow(dpy, window);
	XFlush(dpy);
}

static EGLConfig get_config(void)
{
        EGLConfig config;
	EGLint numConfigs;
	const EGLint configAttribs[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RED_SIZE, 8,
		EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8,
		EGL_DEPTH_SIZE, 24,
		EGL_NONE
	};
	assert(eglChooseConfig(display, configAttribs, &config, 1, &numConfigs) == EGL_TRUE);
	return config;
}

static void render_target_init(void)
{
	xinit();

	display = eglGetDisplay((EGLNativeDisplayType)dpy);
	assert(display != EGL_NO_DISPLAY);

	EGLint majorVersion;
	EGLint minorVersion;
	assert(eglInitialize(display, &majorVersion, &minorVersion) == EGL_TRUE);

	assert(eglBindAPI(EGL_OPENGL_ES_API) == EGL_TRUE);

	EGLConfig config = get_config();

	surface = eglCreateWindowSurface(display, config, (EGLNativeWindowType)window, NULL);
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
	glViewport(0, 0, TARGET_SIZE, TARGET_SIZE);

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

int main(void)
{
	render_target_init();
	init_gles();
	render();

	sleep(10);
	return 0;
}
