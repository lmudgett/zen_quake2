// gl3_sky.c -- skybox rendering. Drawn as a full 6-face cube centered on the
// viewer, behind the world (sky brush surfaces aren't drawn, so the box shows
// through them). Uses the 3D world shader with the lightmap disabled.

#include "gl3_local.h"

static image_t	*sky_images[6];
static char		skyname[MAX_QPATH];
static float	skyrotate;
static vec3_t	skyaxis;

static GLuint	sky_vao, sky_vbo;

static const float sky_min = 1.0f / 512.0f;
static const float sky_max = 511.0f / 512.0f;

// s,t on a face -> a direction vector index selector (Quake sky orientation)
static const int st_to_vec[6][3] =
{
	{ 3, -1, 2 }, { -3, 1, 2 },
	{ 1, 3, 2 },  { -1, -3, 2 },
	{ -2, -1, 3 }, { 2, -1, -3 }
};

static const int skytexorder[6] = { 0, 2, 1, 3, 4, 5 };

static const char *suf[6] = { "rt", "bk", "lf", "ft", "up", "dn" };

// emit one sky-box vertex (position offset from the viewer + texcoord)
static void MakeSkyVec (float s, float t, int axis, const float *vieworg, float *out)
{
	vec3_t	v, b;
	int		j, k;

	b[0] = s * 2300.0f;
	b[1] = t * 2300.0f;
	b[2] = 2300.0f;

	for (j = 0; j < 3; j++)
	{
		k = st_to_vec[axis][j];
		if (k < 0)
			v[j] = -b[-k - 1];
		else
			v[j] = b[k - 1];
	}

	s = (s + 1) * 0.5f;
	t = (t + 1) * 0.5f;
	if (s < sky_min) s = sky_min; else if (s > sky_max) s = sky_max;
	if (t < sky_min) t = sky_min; else if (t > sky_max) t = sky_max;
	t = 1.0f - t;

	// position (world space, offset so the box tracks the camera) + uv + lmuv(0,0)
	out[0] = v[0] + vieworg[0];
	out[1] = v[1] + vieworg[1];
	out[2] = v[2] + vieworg[2];
	out[3] = s;
	out[4] = t;
	out[5] = 0;
	out[6] = 0;
}

void GL3_InitSky (void)
{
	glGenVertexArrays (1, &sky_vao);
	glGenBuffers (1, &sky_vbo);
	glBindVertexArray (sky_vao);
	glBindBuffer (GL_ARRAY_BUFFER, sky_vbo);
	glEnableVertexAttribArray (0);
	glVertexAttribPointer (0, 3, GL_FLOAT, GL_FALSE, VERTEXSIZE * sizeof(float), (void *)0);
	glEnableVertexAttribArray (1);
	glVertexAttribPointer (1, 2, GL_FLOAT, GL_FALSE, VERTEXSIZE * sizeof(float), (void *)(3 * sizeof(float)));
}

void GL3_ShutdownSky (void)
{
	if (sky_vbo) glDeleteBuffers (1, &sky_vbo);
	if (sky_vao) glDeleteVertexArrays (1, &sky_vao);
}

void GL3_SetSky (char *name, float rotate, vec3_t axis)
{
	int		i;
	char	pathname[MAX_QPATH];

	strncpy (skyname, name, sizeof(skyname) - 1);
	skyname[sizeof(skyname) - 1] = 0;
	skyrotate = rotate;
	VectorCopy (axis, skyaxis);

	for (i = 0; i < 6; i++)
	{
		Com_sprintf (pathname, sizeof(pathname), "env/%s%s.tga", skyname, suf[i]);
		sky_images[i] = GL3_FindImage (pathname, it_sky);
		if (!sky_images[i])
		{
			Com_sprintf (pathname, sizeof(pathname), "env/%s%s.pcx", skyname, suf[i]);
			sky_images[i] = GL3_FindImage (pathname, it_sky);
		}
		if (!sky_images[i])
			sky_images[i] = r_notexture;
	}
}

// draw the full skybox around the viewer (called before the world, no depth)
void GL3_DrawSkyBox (const float *viewproj, const float *vieworg)
{
	int		i;
	float	verts[6 * VERTEXSIZE];

	if (!sky_images[0])
		return;

	glUseProgram (gl3_prog3d.program);
	glUniformMatrix4fv (gl3_prog3d.u_mvp, 1, GL_FALSE, viewproj);
	glUniform1i (gl3_prog3d.u_lm_enabled, 0);
	glUniform1f (gl3_prog3d.u_gamma, vid_gamma->value < 0.5f ? 0.5f : vid_gamma->value);
	glUniform1f (gl3_prog3d.u_intensity, 1.0f);

	glDisable (GL_DEPTH_TEST);
	glDepthMask (GL_FALSE);
	glDisable (GL_BLEND);
	glActiveTexture (GL_TEXTURE0);
	glBindVertexArray (sky_vao);

	for (i = 0; i < 6; i++)
	{
		// two triangles covering the whole face
		MakeSkyVec (-1, -1, i, vieworg, &verts[0 * VERTEXSIZE]);
		MakeSkyVec (-1,  1, i, vieworg, &verts[1 * VERTEXSIZE]);
		MakeSkyVec ( 1,  1, i, vieworg, &verts[2 * VERTEXSIZE]);
		MakeSkyVec (-1, -1, i, vieworg, &verts[3 * VERTEXSIZE]);
		MakeSkyVec ( 1,  1, i, vieworg, &verts[4 * VERTEXSIZE]);
		MakeSkyVec ( 1, -1, i, vieworg, &verts[5 * VERTEXSIZE]);

		GL3_Bind (sky_images[skytexorder[i]]->texnum);
		glBindBuffer (GL_ARRAY_BUFFER, sky_vbo);
		glBufferData (GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STREAM_DRAW);
		glDrawArrays (GL_TRIANGLES, 0, 6);
	}

	glDepthMask (GL_TRUE);
	glEnable (GL_DEPTH_TEST);
}
