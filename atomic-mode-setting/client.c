#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <math.h>

#include <fcntl.h>
#include <unistd.h>

#include "share.h"

#define TARGET_SIZE 256

static struct render_state state = {
	.target_width = TARGET_SIZE,
	.target_height = TARGET_SIZE,
};

static const char vertex_shader[] =
	"uniform mat3 modelView;"
	"attribute vec3 positionIn;"
	"void main() {"
	"    gl_Position = vec4(modelView * positionIn, 1);"
	"}";

static const char fragment_shader[] =
	"void main() {"
	"    gl_FragColor = vec4(1.0, 0.0, 0.0, 1);"
	"}";

static int render(uint64_t index, int wait_fd)
{
	// start GPU task after server is done with this render buffer
	if (wait_fd >= 0) {
		wait_fence(state.display, wait_fd);
		close(wait_fd);
	}

	GLfloat vertex[] = {
		-1, -1, 0,
		-1, 1, 0,
		1, 1, 0,
	};

	GLint position = glGetAttribLocation(state.program, "positionIn");
	glEnableVertexAttribArray(position);
	glVertexAttribPointer(position, 3, GL_FLOAT, 0, 0, vertex);

	// rotate around Y axis
	static const int seconds_per_round = 5;
	static const int monitor_fps = 60;
	static const double pi = 3.1415926;
	double sita = (2 * pi) / (seconds_per_round * monitor_fps) * index;
	GLfloat matrix[] = {
		cos(sita), 0, sin(sita),
		0, 1, 0,
		-sin(sita), 0, cos(sita),
	};

	GLint model_view = glGetUniformLocation(state.program, "modelView");
	glUniformMatrix3fv(model_view, 1, GL_FALSE, matrix);

	glClear(GL_COLOR_BUFFER_BIT);

	glDrawArrays(GL_TRIANGLES, 0, 3);

	// after this GPU task is done, this fence will be signaled
	int signal_fd = get_fence(state.display);

	// swap back buffer to front
	eglSwapBuffers(state.display, state.surface);

	return signal_fd;
}

// assume max number of bos in a gbm_surface is less than 32
#define MAX_BOS 32
static struct gbm_bo *busy_bos[MAX_BOS] = {0};

static void present(int fd, int signal_fd, uint64_t index)
{
	struct gbm_bo *bo = gbm_surface_lock_front_buffer(state.gs);
	assert(bo);

	struct present_buffer data = {
		.x = 128,
		.y = 128,
		.width = gbm_bo_get_width(bo),
		.height = gbm_bo_get_height(bo),
		.stride = gbm_bo_get_stride(bo),
		.format = gbm_bo_get_format(bo),
		.index = index,
	};

	int fds[2] = { gbm_bo_get_fd(bo), signal_fd };
	int num_fd = signal_fd >= 0 ? 2 : 1;
	ssize_t size = sock_fd_write(fd, &data, sizeof(data), fds, num_fd);
	assert(size > 0);

	// close after usage
	close(fds[0]);
	if (signal_fd >= 0)
		close(signal_fd);

	busy_bos[index % MAX_BOS] = bo;
}

static int get_free_buffer(int fd)
{
	if (gbm_surface_has_free_buffers(state.gs))
		return -1;

	int wait_fd = -1;
	int num_fd = 1;
	struct present_done data;
	ssize_t size = sock_fd_read(fd, &data, sizeof(data), &wait_fd, &num_fd);
	assert(size > 0);
	assert(num_fd <= 1);

	struct gbm_bo *bo = busy_bos[data.index % MAX_BOS];
	busy_bos[data.index % MAX_BOS] = NULL;
	assert(bo);

	gbm_surface_release_buffer(state.gs, bo);
	return wait_fd;
}

void client_main(int fd)
{
	state.fd = open("/dev/dri/renderD128", O_RDWR);
	assert(state.fd >= 0);
	
	// render
	render_target_init(&state);
	init_gles(&state, vertex_shader, fragment_shader);

	// background color
	glClearColor(0, 0, 0, 0);

	for (uint64_t i = 0; true; i++) {
		// ensure back buffer is free and release buffer when receive
		// present done from server
		int wait_fd = get_free_buffer(fd);

		// do OpenGL rendering
		int signal_fd = render(i, wait_fd);

		// send to server for display
		present(fd, signal_fd, i);
	}
}
