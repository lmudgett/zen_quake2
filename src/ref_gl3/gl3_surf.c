// gl3_surf.c -- world (BSP brush) surface rendering. Builds one static VBO
// from all world polygons at registration time and draws visible surfaces
// with the 3D shader. (Lightmaps are added in a follow-up; this first cut
// renders diffuse-only so geometry can be verified.)

#include "gl3_local.h"

// r_worldmodel is defined in gl3_model.c (declared extern in gl3_model.h)

static GLuint	world_vao, world_vbo;
static int		world_numverts;

int		c_brush_polys;
int		r_framecount = 1;
int		r_visframecount;

// ------------------------------------------------------------------ lightmaps (stubs for the diffuse-only cut)

void GL3_BeginBuildingLightmaps (model_t *m)
{
}

void GL3_CreateSurfaceLightmap (msurface_t *surf)
{
	// no atlas yet; give the polygon-builder valid (zero) lightmap coords
	surf->light_s = 0;
	surf->light_t = 0;
	surf->lightmaptexturenum = 0;
}

void GL3_EndBuildingLightmaps (void)
{
}

// ------------------------------------------------------------------ world VBO

// count polygon verts across every world surface
static int GL3_CountWorldVerts (void)
{
	int	i, total = 0;
	msurface_t	*surf = r_worldmodel->surfaces;

	for (i = 0; i < r_worldmodel->numsurfaces; i++, surf++)
	{
		glpoly_t *p;
		for (p = surf->polys; p; p = p->next)
			total += p->numverts;
	}
	return total;
}

void GL3_BuildWorldVBO (void)
{
	int		i, v = 0;
	float	*data, *dst;
	msurface_t	*surf;

	if (!r_worldmodel)
		return;

	world_numverts = GL3_CountWorldVerts ();
	if (!world_numverts)
		return;

	data = malloc (world_numverts * VERTEXSIZE * sizeof(float));
	dst = data;

	surf = r_worldmodel->surfaces;
	for (i = 0; i < r_worldmodel->numsurfaces; i++, surf++)
	{
		glpoly_t *p;
		for (p = surf->polys; p; p = p->next)
		{
			p->vbo_firstvert = v;
			memcpy (dst, p->verts, p->numverts * VERTEXSIZE * sizeof(float));
			dst += p->numverts * VERTEXSIZE;
			v += p->numverts;
		}
	}

	glGenVertexArrays (1, &world_vao);
	glGenBuffers (1, &world_vbo);
	glBindVertexArray (world_vao);
	glBindBuffer (GL_ARRAY_BUFFER, world_vbo);
	glBufferData (GL_ARRAY_BUFFER, world_numverts * VERTEXSIZE * sizeof(float), data, GL_STATIC_DRAW);

	// layout: pos(3) uv(2) lmuv(2), stride VERTEXSIZE floats
	glEnableVertexAttribArray (0);
	glVertexAttribPointer (0, 3, GL_FLOAT, GL_FALSE, VERTEXSIZE * sizeof(float), (void *)0);
	glEnableVertexAttribArray (1);
	glVertexAttribPointer (1, 2, GL_FLOAT, GL_FALSE, VERTEXSIZE * sizeof(float), (void *)(3 * sizeof(float)));
	glEnableVertexAttribArray (2);
	glVertexAttribPointer (2, 2, GL_FLOAT, GL_FALSE, VERTEXSIZE * sizeof(float), (void *)(5 * sizeof(float)));

	free (data);

	ri.Con_Printf (PRINT_ALL, "world VBO: %d verts, %d surfaces\n",
		world_numverts, r_worldmodel->numsurfaces);
}

// ------------------------------------------------------------------ draw

void GL3_MarkLeaves (void)
{
	// PVS culling comes later; for now everything is considered visible
}

static image_t *GL3_TextureAnimation (mtexinfo_t *tex)
{
	if (!tex->next)
		return tex->image;
	// animated textures step by (int)(time*2); base frame is fine for a still
	return tex->image;
}

void GL3_DrawWorld (void)
{
	int		i;
	msurface_t	*surf;

	if (!r_worldmodel || !world_vao)
		return;

	glBindVertexArray (world_vao);

	surf = r_worldmodel->surfaces;
	for (i = 0; i < r_worldmodel->numsurfaces; i++, surf++)
	{
		image_t	*img;
		glpoly_t *p;

		if (surf->flags & SURF_DRAWSKY)
			continue;			// sky handled separately (later)

		img = surf->texinfo ? GL3_TextureAnimation (surf->texinfo) : NULL;
		if (!img)
			img = r_notexture;
		GL3_Bind (img->texnum);

		for (p = surf->polys; p; p = p->next)
		{
			glDrawArrays (GL_TRIANGLE_FAN, p->vbo_firstvert, p->numverts);
			c_brush_polys++;
		}
	}
}
