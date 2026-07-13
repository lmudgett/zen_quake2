// gl3_particles.c -- point-sprite particle rendering (blaster bolts, blood,
// sparks, gibs trails). Particles come from the client each frame in
// r_newrefdef.particles as {origin, palette color index, alpha}.

#include "gl3_local.h"

static GLuint	part_vao, part_vbo;

typedef struct { float pos[3]; float color[4]; } partvert_t;

void GL3_InitParticles (void)
{
	glGenVertexArrays (1, &part_vao);
	glGenBuffers (1, &part_vbo);
	glBindVertexArray (part_vao);
	glBindBuffer (GL_ARRAY_BUFFER, part_vbo);
	glEnableVertexAttribArray (0);
	glVertexAttribPointer (0, 3, GL_FLOAT, GL_FALSE, sizeof(partvert_t), (void *)0);
	glEnableVertexAttribArray (3);
	glVertexAttribPointer (3, 4, GL_FLOAT, GL_FALSE, sizeof(partvert_t), (void *)(3 * sizeof(float)));
}

void GL3_ShutdownParticles (void)
{
	if (part_vbo) glDeleteBuffers (1, &part_vbo);
	if (part_vao) glDeleteVertexArrays (1, &part_vao);
}

void GL3_DrawParticles (const float *viewproj)
{
	int			n = r_newrefdef.num_particles;
	partvert_t	*buf;
	int			i;

	if (n <= 0)
		return;

	buf = malloc (n * sizeof(partvert_t));

	for (i = 0; i < n; i++)
	{
		particle_t	*p = &r_newrefdef.particles[i];
		byte		*rgb = (byte *)&d_8to24table[p->color & 255];

		buf[i].pos[0] = p->origin[0];
		buf[i].pos[1] = p->origin[1];
		buf[i].pos[2] = p->origin[2];
		buf[i].color[0] = rgb[0] / 255.0f;
		buf[i].color[1] = rgb[1] / 255.0f;
		buf[i].color[2] = rgb[2] / 255.0f;
		buf[i].color[3] = p->alpha;
	}

	glUseProgram (gl3_prog_part.program);
	glUniformMatrix4fv (gl3_prog_part.u_mvp, 1, GL_FALSE, viewproj);
	glUniform1f (gl3_prog_part.u_gamma, vid_gamma->value < 0.5f ? 0.5f : vid_gamma->value);
	// base size in pixels at w=1; scaled by 1/w in the shader, tuned to the drawable height
	glUniform1f (gl3_prog_part.u_psize, gl3state.height * 0.8f);

	glEnable (GL_PROGRAM_POINT_SIZE);
	glEnable (GL_BLEND);
	glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDepthMask (GL_FALSE);			// particles don't write depth

	glBindVertexArray (part_vao);
	glBindBuffer (GL_ARRAY_BUFFER, part_vbo);
	glBufferData (GL_ARRAY_BUFFER, n * sizeof(partvert_t), buf, GL_STREAM_DRAW);
	glDrawArrays (GL_POINTS, 0, n);

	glDepthMask (GL_TRUE);
	glDisable (GL_BLEND);
	glDisable (GL_PROGRAM_POINT_SIZE);

	free (buf);
}
