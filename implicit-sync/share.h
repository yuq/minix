#ifndef _SHARE_H_
#define _SHARE_H_

#include <gbm.h>

#include <epoxy/gl.h>
#include <epoxy/egl.h>

struct present_buffer {
	uint32_t x, y;
        uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t format;
	uint64_t index;
};

struct present_done {
	uint64_t index;
};

struct render_state {
	int fd;

	GLuint program;
	EGLDisplay display;
	EGLSurface surface;
	EGLContext context;
	struct gbm_device *gbm;
	struct gbm_surface *gs;

	int target_width;
	int target_height;
};

ssize_t sock_fd_write(int sock, void *buf, ssize_t buflen, int fd);
ssize_t sock_fd_read(int sock, void *buf, ssize_t bufsize, int *fds, int *num_fd);

void client_main(int fd);
void server_main(int fd);

void render_target_init(struct render_state *s);
void init_gles(struct render_state *s, const char *vertex_shader,
	       const char *fragment_shader);


#endif
