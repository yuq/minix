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

static void render(uint64_t index)
{
	GLfloat vertex[] = {
		-1, -1, 0,
		-1, 1, 0,
		1, 1, 0,
	};

	GLint position = glGetAttribLocation(state.program, "positionIn");
	glEnableVertexAttribArray(position);
	glVertexAttribPointer(position, 3, GL_FLOAT, 0, 0, vertex);

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

	eglSwapBuffers(state.display, state.surface);
}

// assume max number of bos in a gbm_surface is less than 32
#define MAX_BOS 32
static struct gbm_bo *busy_bos[MAX_BOS] = {0};

static void present(int fd, uint64_t index)
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

	int bo_fd = gbm_bo_get_fd(bo);
	ssize_t size = sock_fd_write(fd, &data, sizeof(data), bo_fd);
	assert(size > 0);

	// close after usage
	close(bo_fd);

	busy_bos[index % MAX_BOS] = bo;
}

static void get_free_buffer(int fd)
{
	if (gbm_surface_has_free_buffers(state.gs))
		return;

	struct present_done data;
	ssize_t size = sock_fd_read(fd, &data, sizeof(data), NULL, NULL);
	assert(size > 0);

	struct gbm_bo *bo = busy_bos[data.index % MAX_BOS];
	busy_bos[data.index % MAX_BOS] = NULL;
	assert(bo);

	gbm_surface_release_buffer(state.gs, bo);
}

void client_main(int fd)
{
	state.fd = open("/dev/dri/renderD128", O_RDWR);
	assert(state.fd >= 0);
	
	// render
	render_target_init(&state);
	init_gles(&state, vertex_shader, fragment_shader);

	for (uint64_t i = 0; true; i++) {
		// ensure back buffer is free and release buffer when receive
		// present done from server
		get_free_buffer(fd);

		// do OpenGL rendering
		render(i);

		// send to server for display
		present(fd, i);
	}
}
