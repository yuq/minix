#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include <fcntl.h>
#include <unistd.h>

#include "share.h"

#define TARGET_SIZE 256

static struct render_state state = {
	.target_width = TARGET_SIZE,
	.target_height = TARGET_SIZE,
};

static const char vertex_shader[] =
	"attribute vec3 positionIn;"
	"void main() {"
	"    gl_Position = vec4(positionIn, 1);"
	"}";

static const char fragment_shader[] =
	"void main() {"
	"    gl_FragColor = vec4(1.0, 0.0, 0.0, 1);"
	"}";

static void render(void)
{
	GLfloat vertex[] = {
		-1, -1, 0,
		-1, 1, 0,
		1, 1, 0,
	};

	GLint position = glGetAttribLocation(state.program, "positionIn");
	glEnableVertexAttribArray(position);
	glVertexAttribPointer(position, 3, GL_FLOAT, 0, 0, vertex);

	glClear(GL_COLOR_BUFFER_BIT);

	glDrawArrays(GL_TRIANGLES, 0, 3);

	eglSwapBuffers(state.display, state.surface);
}

static void present(int fd)
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
	};

	ssize_t size = sock_fd_write(fd, &data, sizeof(data), gbm_bo_get_fd(bo));
	assert(size > 0);

	gbm_surface_release_buffer(state.gs, bo);
}

void client_main(int fd)
{
	state.fd = open("/dev/dri/renderD128", O_RDWR);
	assert(state.fd >= 0);
	
	// render
	render_target_init(&state);
	init_gles(&state, vertex_shader, fragment_shader);
	render();

	// wait for a moment
	sleep(1);

	// send to server for display
	present(fd);
}
