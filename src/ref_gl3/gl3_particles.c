// gl3_particles.c -- particle rendering (blaster bolts, blood, sparks, gib
// trails). Particles come from the client each frame in
// r_newrefdef.particles as {origin, palette color index, alpha}, and draw
// as instanced camera-facing quads: round with a soft edge, and depth-faded
// where they touch geometry (soft particles).

#include "gl3_local.h"

static GLuint	part_vao;
static GLuint	part_corner_vbo;	// static quad corners
static GLuint	part_inst_vbo;		// streamed per-particle data

typedef struct { float pos[3]; float color[4]; } partinst_t;

static partinst_t	*s_partbuf;		// grow-on-demand instance scratch
static int			s_partbuf_max;

void GL3_InitParticles (void)
{
	static const float corners[8] = { -1,-1,  1,-1,  -1,1,  1,1 };

	glGenVertexArrays (1, &part_vao);
	glGenBuffers (1, &part_corner_vbo);
	glGenBuffers (1, &part_inst_vbo);

	glBindVertexArray (part_vao);

	glBindBuffer (GL_ARRAY_BUFFER, part_corner_vbo);
	glBufferData (GL_ARRAY_BUFFER, sizeof(corners), corners, GL_STATIC_DRAW);
	glEnableVertexAttribArray (0);		// a_pos: quad corner
	glVertexAttribPointer (0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void *)0);

	glBindBuffer (GL_ARRAY_BUFFER, part_inst_vbo);
	glEnableVertexAttribArray (4);		// a_ppos: instance center
	glVertexAttribPointer (4, 3, GL_FLOAT, GL_FALSE, sizeof(partinst_t), (void *)0);
	glVertexAttribDivisor (4, 1);
	glEnableVertexAttribArray (3);		// a_color: instance color
	glVertexAttribPointer (3, 4, GL_FLOAT, GL_FALSE, sizeof(partinst_t), (void *)(3 * sizeof(float)));
	glVertexAttribDivisor (3, 1);

	glBindVertexArray (0);
}

void GL3_ShutdownParticles (void)
{
	if (part_inst_vbo)   glDeleteBuffers (1, &part_inst_vbo);
	if (part_corner_vbo) glDeleteBuffers (1, &part_corner_vbo);
	if (part_vao)        glDeleteVertexArrays (1, &part_vao);
	free (s_partbuf);
	s_partbuf = NULL;
	s_partbuf_max = 0;
}

void GL3_DrawParticles (const float *viewproj)
{
	int			n = r_newrefdef.num_particles;
	partinst_t	*buf;
	vec3_t		fwd, right, up;
	GLuint		depthtex;
	int			i;

	if (n <= 0)
		return;

	// reuse a persistent scratch buffer instead of malloc/free every frame
	if (n > s_partbuf_max)
	{
		free (s_partbuf);
		s_partbuf = malloc (n * sizeof(partinst_t));
		if (!s_partbuf) { s_partbuf_max = 0; return; }
		s_partbuf_max = n;
	}
	buf = s_partbuf;

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

	AngleVectors (r_newrefdef.viewangles, fwd, right, up);

	// scene depth for the soft fade (blit is done against the live target)
	depthtex = GL3_Post_ResolveDepth ();

	glUseProgram (gl3_prog_part.program);
	glUniformMatrix4fv (gl3_prog_part.u_mvp, 1, GL_FALSE, viewproj);
	glUniform3f (gl3_prog_part.u_right, right[0], right[1], right[2]);
	glUniform3f (gl3_prog_part.u_up, up[0], up[1], up[2]);
	glUniform1f (gl3_prog_part.u_size, 1.2f);		// half-size, world units
	glUniform1f (gl3_prog_part.u_soft, 1.0f / 6.0f);	// fade across 6 units
	glUniform2f (gl3_prog_part.u_invdepthsize,
		1.0f / GL3_Post_Width (), 1.0f / GL3_Post_Height ());

	glActiveTexture (GL_TEXTURE0);
	glBindTexture (GL_TEXTURE_2D, depthtex);
	gl3state.currenttexture = -1;

	glEnable (GL_BLEND);
	glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDepthMask (GL_FALSE);			// particles don't write depth

	glBindVertexArray (part_vao);
	glBindBuffer (GL_ARRAY_BUFFER, part_inst_vbo);
	glBufferData (GL_ARRAY_BUFFER, n * sizeof(partinst_t), buf, GL_STREAM_DRAW);
	glDrawArraysInstanced (GL_TRIANGLE_STRIP, 0, 4, n);
	glBindVertexArray (0);

	glDepthMask (GL_TRUE);
	glDisable (GL_BLEND);
}
