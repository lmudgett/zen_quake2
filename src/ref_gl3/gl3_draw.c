// gl3_draw.c -- 2D drawing (HUD, console, menus, cinematics) for the GL 3.3
// renderer. Quads are streamed through one VBO with the 2D shader.

#include "gl3_local.h"

static GLuint	draw_vao, draw_vbo;
GLuint	white_tex;					// 1x1 opaque white: fills, shells, beams
static image_t	*draw_chars;		// console font (pics/conchars)

static GLuint	raw_tex;			// cinematic upload target
static unsigned	raw_palette[256];	// active cinematic palette (RGBA)

typedef struct { float x, y, u, v; } drawvert_t;

static void GL3_Ortho (float *m, float l, float r, float b, float t)
{
	memset (m, 0, sizeof(float) * 16);
	m[0]  = 2.0f / (r - l);
	m[5]  = 2.0f / (t - b);
	m[10] = -1.0f;
	m[12] = -(r + l) / (r - l);
	m[13] = -(t + b) / (t - b);
	m[15] = 1.0f;
}

void GL3_Draw_Init (void)
{
	byte	whitepixel[4] = { 255, 255, 255, 255 };
	int		i;

	glGenVertexArrays (1, &draw_vao);
	glGenBuffers (1, &draw_vbo);
	glBindVertexArray (draw_vao);
	glBindBuffer (GL_ARRAY_BUFFER, draw_vbo);
	glEnableVertexAttribArray (0);
	glVertexAttribPointer (0, 2, GL_FLOAT, GL_FALSE, sizeof(drawvert_t), (void *)0);
	glEnableVertexAttribArray (1);
	glVertexAttribPointer (1, 2, GL_FLOAT, GL_FALSE, sizeof(drawvert_t), (void *)(2 * sizeof(float)));

	glGenTextures (1, &white_tex);
	glBindTexture (GL_TEXTURE_2D, white_tex);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, whitepixel);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	gl3state.currenttexture = -1;

	glGenTextures (1, &raw_tex);
	for (i = 0; i < 256; i++)
		raw_palette[i] = d_8to24table[i];

	draw_chars = GL3_FindImage ("pics/conchars.pcx", it_pic);
	if (!draw_chars)
		ri.Sys_Error (ERR_FATAL, "GL3_Draw_Init: couldn't load pics/conchars.pcx");
}

void GL3_Draw_Shutdown (void)
{
	if (draw_vbo) glDeleteBuffers (1, &draw_vbo);
	if (draw_vao) glDeleteVertexArrays (1, &draw_vao);
	if (white_tex) glDeleteTextures (1, &white_tex);
	if (raw_tex) glDeleteTextures (1, &raw_tex);
}

void GL3_SetRawPalette (const unsigned char *palette)
{
	int	i;
	if (palette)
	{
		for (i = 0; i < 256; i++)
		{
			byte *c = (byte *)&raw_palette[i];
			c[0] = palette[i * 3 + 0];
			c[1] = palette[i * 3 + 1];
			c[2] = palette[i * 3 + 2];
			c[3] = 255;
		}
	}
	else
	{
		for (i = 0; i < 256; i++)
			raw_palette[i] = d_8to24table[i];
	}
}

void GL3_Draw_SetOrtho (void)
{
	float	ortho[16];

	GL3_Ortho (ortho, 0, (float)gl3state.width, (float)gl3state.height, 0);

	glUseProgram (gl3_prog2d.program);
	glUniformMatrix4fv (gl3_prog2d.u_ortho, 1, GL_FALSE, ortho);

	// gamma / intensity (replaces the old hardware gamma ramp)
	{
		float gamma = vid_gamma ? vid_gamma->value : 1.0f;
		if (gamma < 0.5f) gamma = 0.5f;
		glUniform1f (gl3_prog2d.u_gamma, gamma);
		glUniform1f (gl3_prog2d.u_intensity, gl_intensity ? gl_intensity->value : 1.0f);
	}

	glDisable (GL_DEPTH_TEST);
	glDisable (GL_CULL_FACE);
	glEnable (GL_BLEND);
	glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glActiveTexture (GL_TEXTURE0);
	glBindVertexArray (draw_vao);
}

void GL3_Draw_Flush (void)
{
	// per-quad immediate submission; nothing batched to flush yet
}

static void GL3_DrawQuad (GLuint tex, float x, float y, float w, float h,
	float s1, float t1, float s2, float t2,
	float r, float g, float b, float a)
{
	drawvert_t	v[6];

	v[0] = (drawvert_t){ x,     y,     s1, t1 };
	v[1] = (drawvert_t){ x + w, y,     s2, t1 };
	v[2] = (drawvert_t){ x + w, y + h, s2, t2 };
	v[3] = (drawvert_t){ x,     y,     s1, t1 };
	v[4] = (drawvert_t){ x + w, y + h, s2, t2 };
	v[5] = (drawvert_t){ x,     y + h, s1, t2 };

	GL3_Bind (tex);
	glUniform4f (gl3_prog2d.u_color, r, g, b, a);
	glBindBuffer (GL_ARRAY_BUFFER, draw_vbo);
	glBufferData (GL_ARRAY_BUFFER, sizeof(v), v, GL_STREAM_DRAW);
	glDrawArrays (GL_TRIANGLES, 0, 6);
}

image_t *GL3_Draw_FindPic (char *name)
{
	char	fullname[MAX_QPATH];

	if (name[0] != '/' && name[0] != '\\')
	{
		Com_sprintf (fullname, sizeof(fullname), "pics/%s.pcx", name);
		return GL3_FindImage (fullname, it_pic);
	}
	return GL3_FindImage (name + 1, it_pic);
}

void GL3_Draw_GetPicSize (int *w, int *h, char *name)
{
	image_t	*img = GL3_Draw_FindPic (name);
	if (!img)
	{
		if (w) *w = -1;
		if (h) *h = -1;
		return;
	}
	if (w) *w = img->width;
	if (h) *h = img->height;
}

void GL3_Draw_Pic (int x, int y, char *name)
{
	image_t	*img = GL3_Draw_FindPic (name);
	if (!img)
	{
		ri.Con_Printf (PRINT_ALL, "Can't find pic: %s\n", name);
		return;
	}
	GL3_DrawQuad (img->texnum, (float)x, (float)y, (float)img->width, (float)img->height,
		0, 0, 1, 1, 1, 1, 1, 1);
}

void GL3_Draw_StretchPic (int x, int y, int w, int h, char *name)
{
	image_t	*img = GL3_Draw_FindPic (name);
	if (!img)
	{
		ri.Con_Printf (PRINT_ALL, "Can't find pic: %s\n", name);
		return;
	}
	GL3_DrawQuad (img->texnum, (float)x, (float)y, (float)w, (float)h,
		0, 0, 1, 1, 1, 1, 1, 1);
}

void GL3_Draw_Char (int x, int y, int num)
{
	int		row, col;
	float	frow, fcol, size = 0.0625f;

	num &= 255;
	if ((num & 127) == 32)
		return;			// space
	if (y <= -8)
		return;			// totally off screen
	if (!draw_chars)
		return;

	row = num >> 4;
	col = num & 15;
	frow = row * size;
	fcol = col * size;

	GL3_DrawQuad (draw_chars->texnum, (float)x, (float)y, 8, 8,
		fcol, frow, fcol + size, frow + size, 1, 1, 1, 1);
}

void GL3_Draw_TileClear (int x, int y, int w, int h, char *name)
{
	image_t	*img = GL3_Draw_FindPic (name);
	if (!img)
		return;
	// tile at the source resolution by repeating uv (image is CLAMP, so this
	// approximates the fill; the console backdrop is the main user)
	GL3_DrawQuad (img->texnum, (float)x, (float)y, (float)w, (float)h,
		x / 64.0f, y / 64.0f, (x + w) / 64.0f, (y + h) / 64.0f, 1, 1, 1, 1);
}

void GL3_Draw_Fill (int x, int y, int w, int h, int c)
{
	byte	*rgba = (byte *)&d_8to24table[c & 255];

	GL3_DrawQuad (white_tex, (float)x, (float)y, (float)w, (float)h, 0, 0, 1, 1,
		rgba[0] / 255.0f, rgba[1] / 255.0f, rgba[2] / 255.0f, 1.0f);
}

void GL3_Draw_FadeScreen (void)
{
	GL3_DrawQuad (white_tex, 0, 0, (float)gl3state.width, (float)gl3state.height,
		0, 0, 1, 1, 0, 0, 0, 0.8f);
}

// full-screen colour wash (damage/pickup/underwater), from refdef.blend.
// Draws the literal colour (intensity/gamma neutralised, then restored so the
// HUD the client draws afterwards keeps correct brightness).
void GL3_Draw_PolyBlend (float r, float g, float b, float a)
{
	if (a <= 0.0f)
		return;

	glUniform1f (gl3_prog2d.u_intensity, 1.0f);
	glUniform1f (gl3_prog2d.u_gamma, 1.0f);
	GL3_DrawQuad (white_tex, 0, 0, (float)gl3state.width, (float)gl3state.height,
		0, 0, 1, 1, r, g, b, a);
	glUniform1f (gl3_prog2d.u_intensity, gl_intensity ? gl_intensity->value : 1.0f);
	glUniform1f (gl3_prog2d.u_gamma, (vid_gamma && vid_gamma->value >= 0.5f) ? vid_gamma->value : 1.0f);
}

void GL3_Draw_StretchRaw (int x, int y, int w, int h, int cols, int rows, byte *data)
{
	static unsigned	buf[256 * 256];
	unsigned		*dst = buf;
	int				i, count = cols * rows;

	if (cols <= 0 || rows <= 0 || count > 256 * 256)
		return;

	for (i = 0; i < count; i++)
		dst[i] = raw_palette[data[i]];

	GL3_Bind (raw_tex);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, cols, rows, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glUniform4f (gl3_prog2d.u_color, 1, 1, 1, 1);
	{
		drawvert_t	v[6] = {
			{ (float)x,     (float)y,     0, 0 },
			{ (float)(x+w), (float)y,     1, 0 },
			{ (float)(x+w), (float)(y+h), 1, 1 },
			{ (float)x,     (float)y,     0, 0 },
			{ (float)(x+w), (float)(y+h), 1, 1 },
			{ (float)x,     (float)(y+h), 0, 1 },
		};
		glBindBuffer (GL_ARRAY_BUFFER, draw_vbo);
		glBufferData (GL_ARRAY_BUFFER, sizeof(v), v, GL_STREAM_DRAW);
		glDrawArrays (GL_TRIANGLES, 0, 6);
	}
	gl3state.currenttexture = -1;	// we changed raw_tex params
}
