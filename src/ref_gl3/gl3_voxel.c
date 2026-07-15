// gl3_voxel.c -- textured voxel render mode (world + entities).
//
// When r_voxelize is on, the world (and, with r_voxelize_entities, alias
// models and the view weapon) is rebuilt as a mesh of axis-aligned cubes at
// r_voxelsize resolution. Each surface is snapped to a grid, only the exposed
// cube faces are emitted, and each face is textured with UVs projected from
// the source surface exactly as the normal renderer projects them -- so the
// real Quake textures flow across the blocky surface. Faces are grouped per
// source surface and drawn only for PVS/frustum-visible surfaces, reusing the
// engine's existing culling, which is what keeps fine resolutions affordable.
//
// Author: Len Mudgett

#include "gl3_local.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

cvar_t	*r_voxelize;
cvar_t	*r_voxelsize;
cvar_t	*r_voxelize_entities;

// one vertex of the voxel-face mesh: world/model pos, projected uv, baked shade
typedef struct { float pos[3]; float uv[2]; float shade; } voxvert_t;
#define VOXVERT_FLOATS	6

// ---------------------------------------------------------------- occupancy
// A grid cell is (x,y,z) integer coordinates relative to vox_origin. During a
// build we hash every occupied cell (tagged with the surface that filled it)
// so Pass B can test whether a neighbour cell is solid and cull hidden faces.

typedef struct { int x, y, z, surf, next; } voxcell_t;

static voxcell_t	*cell_pool;
static int			cell_count, cell_cap;
static int			*cell_hash;			// bucket -> head cell index, or -1
static int			cell_hashmask;

static vec3_t		vox_origin;			// world-space grid origin (r_worldmodel->mins)
static float		vox_size;			// cell edge in world units

static unsigned CellKey (int x, int y, int z)
{
	return (unsigned)(x * 73856093) ^ (unsigned)(y * 19349663) ^ (unsigned)(z * 83492791);
}

static qboolean CellExists (int x, int y, int z)
{
	int	i;

	for (i = cell_hash[CellKey (x, y, z) & cell_hashmask]; i != -1; i = cell_pool[i].next)
		if (cell_pool[i].x == x && cell_pool[i].y == y && cell_pool[i].z == z)
			return true;
	return false;
}

static void AddCellXYZ (int x, int y, int z, int surf)
{
	unsigned	h = CellKey (x, y, z) & cell_hashmask;
	int			i;

	for (i = cell_hash[h]; i != -1; i = cell_pool[i].next)
		if (cell_pool[i].x == x && cell_pool[i].y == y && cell_pool[i].z == z)
			return;					// already occupied (first surface owns it)

	if (cell_count == cell_cap)
	{
		cell_cap = cell_cap ? cell_cap * 2 : 65536;
		cell_pool = realloc (cell_pool, cell_cap * sizeof(voxcell_t));
		if (!cell_pool)
			ri.Sys_Error (ERR_FATAL, "GL3_BuildWorldVoxels: out of memory");
	}
	cell_pool[cell_count].x = x;
	cell_pool[cell_count].y = y;
	cell_pool[cell_count].z = z;
	cell_pool[cell_count].surf = surf;
	cell_pool[cell_count].next = cell_hash[h];
	cell_hash[h] = cell_count;
	cell_count++;
}

static void AddCellAt (const float *p, int surf)
{
	AddCellXYZ ((int)floorf ((p[0] - vox_origin[0]) / vox_size),
				(int)floorf ((p[1] - vox_origin[1]) / vox_size),
				(int)floorf ((p[2] - vox_origin[2]) / vox_size), surf);
}

// rasterize one triangle into cells: its 3 vertices, its edges (walked at
// half-cell steps), and its interior (barycentric-sampled at half-cell steps).
// Oversampling guarantees a gap-free shell for large flat BSP faces.
static void RasterTri (const float *a, const float *b, const float *c, int surf)
{
	vec3_t	ab, ac;
	float	e1, e2, u, v, step;
	int		i, j, n, en;

	AddCellAt (a, surf);
	AddCellAt (b, surf);
	AddCellAt (c, surf);

	VectorSubtract (b, a, ab);
	VectorSubtract (c, a, ac);
	e1 = VectorLength (ab);
	e2 = VectorLength (ac);
	step = vox_size * 0.5f;

	// edges
	{
		const float	*ev[3][2] = { { a, b }, { b, c }, { c, a } };
		vec3_t		d, p;
		float		len;
		int			k;
		for (k = 0; k < 3; k++)
		{
			VectorSubtract (ev[k][1], ev[k][0], d);
			len = VectorLength (d);
			en = (int)(len / step) + 1;
			for (i = 1; i < en; i++)
			{
				float t = (float)i / en;
				p[0] = ev[k][0][0] + d[0] * t;
				p[1] = ev[k][0][1] + d[1] * t;
				p[2] = ev[k][0][2] + d[2] * t;
				AddCellAt (p, surf);
			}
		}
	}

	// interior
	n = (int)((e1 > e2 ? e1 : e2) / step) + 1;
	for (i = 0; i <= n; i++)
	{
		u = (float)i / n;
		for (j = 0; j <= n - i; j++)
		{
			vec3_t	p;
			v = (float)j / n;
			p[0] = a[0] + ab[0] * u + ac[0] * v;
			p[1] = a[1] + ab[1] * u + ac[1] * v;
			p[2] = a[2] + ab[2] * u + ac[2] * v;
			AddCellAt (p, surf);
		}
	}
}

// ---------------------------------------------------------------- cube faces
// The 6 axis faces. For each: outward normal, and the two in-plane half-edge
// axes U,V ordered so U x V = +normal, giving CCW-from-outside winding. A
// per-face shade fakes directional lighting (Quake Z is up).

typedef struct { int axis, dir; int ua, va; float shade; } voxface_t;
static const voxface_t voxfaces[6] = {
	{ 0, +1, 1, 2, 0.80f },	// +X  U=+Y V=+Z
	{ 0, -1, 2, 1, 0.80f },	// -X  U=+Z V=+Y
	{ 1, +1, 2, 0, 0.65f },	// +Y  U=+Z V=+X
	{ 1, -1, 0, 2, 0.65f },	// -Y  U=+X V=+Z
	{ 2, +1, 0, 1, 1.00f },	// +Z (up)    U=+X V=+Y
	{ 2, -1, 1, 0, 0.45f },	// -Z (down)  U=+Y V=+X
};

// project a world point through a surface's texinfo, as GL3_BuildPolygonFromSurface
static void SurfUV (msurface_t *surf, const float *p, float *s, float *t)
{
	mtexinfo_t	*tex = surf->texinfo;
	*s = (DotProduct (p, tex->vecs[0]) + tex->vecs[0][3]) / tex->image->width;
	*t = (DotProduct (p, tex->vecs[1]) + tex->vecs[1][3]) / tex->image->height;
}

// ---------------------------------------------------------------- world mesh

static GLuint		world_vao, world_vbo;
static int			world_totalverts;
static float		world_built_size = -1.0f;
static int			world_built_seq = -1;	// registration_sequence the mesh was built for

typedef struct { int firstvert, numverts; image_t *img; } voxsurf_t;
static voxsurf_t	*world_surfranges;

static void FreeWorldVoxels (void)
{
	if (world_vbo) { glDeleteBuffers (1, &world_vbo); world_vbo = 0; }
	if (world_vao) { glDeleteVertexArrays (1, &world_vao); world_vao = 0; }
	free (world_surfranges);
	world_surfranges = NULL;
	world_totalverts = 0;
	world_built_seq = -1;
	world_built_size = -1.0f;
}

// write the 6 verts (2 tris) of one exposed face into dst; return dst advanced
static voxvert_t *EmitFace (voxvert_t *dst, msurface_t *surf, const voxcell_t *cell,
	const voxface_t *f)
{
	vec3_t	center, corner[4];
	float	half = vox_size * 0.5f;
	int		k;
	static const int order[6] = { 0, 1, 2, 0, 2, 3 };	// two CCW tris

	// cell center in world space
	center[0] = vox_origin[0] + (cell->x + 0.5f) * vox_size;
	center[1] = vox_origin[1] + (cell->y + 0.5f) * vox_size;
	center[2] = vox_origin[2] + (cell->z + 0.5f) * vox_size;
	// slide to the face plane along the outward normal
	center[f->axis] += f->dir * half;

	// 4 corners: center +/- U +/- V (CCW from outside)
	for (k = 0; k < 4; k++)
	{
		float	su = (k == 1 || k == 2) ? 1.0f : -1.0f;	// -U,+U,+U,-U
		float	sv = (k >= 2) ? 1.0f : -1.0f;				// -V,-V,+V,+V
		VectorCopy (center, corner[k]);
		corner[k][f->ua] += su * half;
		corner[k][f->va] += sv * half;
	}

	for (k = 0; k < 6; k++)
	{
		float	*c = corner[order[k]];
		VectorCopy (c, dst->pos);
		SurfUV (surf, c, &dst->uv[0], &dst->uv[1]);
		dst->shade = f->shade;
		dst++;
	}
	return dst;
}

void GL3_BuildWorldVoxels (void)
{
	int			i, s, d;
	msurface_t	*surf;
	int			*facecount, *cursor;
	voxvert_t	*mesh, *dst;
	int			nsurf;

	FreeWorldVoxels ();

	if (!r_worldmodel)
		return;

	nsurf = r_worldmodel->numsurfaces;
	vox_size = r_voxelsize->value;
	if (vox_size < 1.0f)
		vox_size = 1.0f;
	VectorCopy (r_worldmodel->mins, vox_origin);

	// occupancy hash (transient) -- sized generously, freed after the build
	cell_hashmask = (1 << 20) - 1;
	cell_hash = malloc ((cell_hashmask + 1) * sizeof(int));
	memset (cell_hash, 0xff, (cell_hashmask + 1) * sizeof(int));	// -1
	cell_pool = NULL;
	cell_count = cell_cap = 0;

	// Pass A: rasterize every world surface into occupied cells
	surf = r_worldmodel->surfaces;
	for (i = 0; i < nsurf; i++, surf++)
	{
		glpoly_t	*p;

		if (surf->flags & (SURF_DRAWSKY | SURF_DRAWTURB))
			continue;						// sky/water get their own look
		if (!surf->texinfo || !surf->texinfo->image)
			continue;

		for (p = surf->polys; p; p = p->next)
		{
			int	k;
			// fan-triangulate the polygon
			for (k = 1; k < p->numverts - 1; k++)
				RasterTri (p->verts[0], p->verts[k], p->verts[k + 1], i);
		}
	}

	// Pass B step 1: count exposed faces per surface
	facecount = calloc (nsurf, sizeof(int));
	for (i = 0; i < cell_count; i++)
	{
		voxcell_t	*cell = &cell_pool[i];
		for (d = 0; d < 6; d++)
		{
			const voxface_t	*f = &voxfaces[d];
			int	nx = cell->x + (f->axis == 0 ? f->dir : 0);
			int	ny = cell->y + (f->axis == 1 ? f->dir : 0);
			int	nz = cell->z + (f->axis == 2 ? f->dir : 0);
			if (!CellExists (nx, ny, nz))
				facecount[cell->surf]++;
		}
	}

	// per-surface vertex ranges (6 verts per face)
	world_surfranges = calloc (nsurf, sizeof(voxsurf_t));
	world_totalverts = 0;
	for (s = 0; s < nsurf; s++)
	{
		world_surfranges[s].firstvert = world_totalverts;
		world_surfranges[s].numverts = facecount[s] * 6;
		world_surfranges[s].img = r_worldmodel->surfaces[s].texinfo
			? r_worldmodel->surfaces[s].texinfo->image : NULL;
		world_totalverts += facecount[s] * 6;
	}

	if (!world_totalverts)
	{
		free (facecount); free (cell_hash); free (cell_pool);
		cell_hash = NULL; cell_pool = NULL;
		world_built_seq = registration_sequence;
		world_built_size = vox_size;
		return;
	}

	// Pass B step 2: emit faces into per-surface ranges
	mesh = malloc (world_totalverts * sizeof(voxvert_t));
	cursor = malloc (nsurf * sizeof(int));
	for (s = 0; s < nsurf; s++)
		cursor[s] = world_surfranges[s].firstvert;

	for (i = 0; i < cell_count; i++)
	{
		voxcell_t	*cell = &cell_pool[i];
		surf = &r_worldmodel->surfaces[cell->surf];
		for (d = 0; d < 6; d++)
		{
			const voxface_t	*f = &voxfaces[d];
			int	nx = cell->x + (f->axis == 0 ? f->dir : 0);
			int	ny = cell->y + (f->axis == 1 ? f->dir : 0);
			int	nz = cell->z + (f->axis == 2 ? f->dir : 0);
			if (CellExists (nx, ny, nz))
				continue;
			dst = mesh + cursor[cell->surf];
			dst = EmitFace (dst, surf, cell, f);
			cursor[cell->surf] += 6;
		}
	}

	// upload
	glGenVertexArrays (1, &world_vao);
	glGenBuffers (1, &world_vbo);
	glBindVertexArray (world_vao);
	glBindBuffer (GL_ARRAY_BUFFER, world_vbo);
	glBufferData (GL_ARRAY_BUFFER, world_totalverts * sizeof(voxvert_t), mesh, GL_STATIC_DRAW);
	glEnableVertexAttribArray (0);	// a_pos
	glVertexAttribPointer (0, 3, GL_FLOAT, GL_FALSE, sizeof(voxvert_t), (void *)0);
	glEnableVertexAttribArray (1);	// a_uv
	glVertexAttribPointer (1, 2, GL_FLOAT, GL_FALSE, sizeof(voxvert_t), (void *)(3 * sizeof(float)));
	glEnableVertexAttribArray (3);	// a_color = baked shade (single float)
	glVertexAttribPointer (3, 1, GL_FLOAT, GL_FALSE, sizeof(voxvert_t), (void *)(5 * sizeof(float)));
	glBindVertexArray (0);

	free (mesh);
	free (cursor);
	free (facecount);
	free (cell_hash);
	free (cell_pool);
	cell_hash = NULL;
	cell_pool = NULL;

	world_built_seq = registration_sequence;
	world_built_size = vox_size;

	ri.Con_Printf (PRINT_ALL, "world voxels: %d faces, %d cells @ %g units\n",
		world_totalverts / 6, cell_count, vox_size);
}

static const float voxel_identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };

void GL3_DrawWorldVoxels (const float *viewproj)
{
	int			i;
	msurface_t	*surf;

	if (!r_worldmodel)
		return;

	// (re)build on map change (registration_sequence) or a live r_voxelsize change
	if (world_built_seq != registration_sequence || world_built_size != r_voxelsize->value)
		GL3_BuildWorldVoxels ();

	if (!world_totalverts)
		return;

	// stamp surf->drawframe for this frame's PVS + frustum (reuses the world path)
	GL3_MarkVisibleSurfaces ();

	glUseProgram (gl3_prog_voxel.program);
	glUniformMatrix4fv (gl3_prog_voxel.u_mvp, 1, GL_FALSE, viewproj);
	glUniformMatrix4fv (gl3_prog_voxel.u_model, 1, GL_FALSE, voxel_identity);
	glUniform1f (gl3_prog_voxel.u_intensity, gl_intensity->value);
	glUniform3f (gl3_prog_voxel.u_entcolor, 1.0f, 1.0f, 1.0f);

	glActiveTexture (GL_TEXTURE0);
	glBindVertexArray (world_vao);

	surf = r_worldmodel->surfaces;
	for (i = 0; i < r_worldmodel->numsurfaces; i++, surf++)
	{
		voxsurf_t	*vs = &world_surfranges[i];
		if (surf->drawframe != r_framecount)
			continue;
		if (!vs->numverts || !vs->img)
			continue;
		GL3_Bind (vs->img->texnum);
		glDrawArrays (GL_TRIANGLES, vs->firstvert, vs->numverts);
	}
	glBindVertexArray (0);
}

// ---------------------------------------------------------------- entities
// Phase 2: filled in by a later change. Stubs keep the module linkable.

void GL3_VoxelizeAliasModel (model_t *mod)
{
	(void)mod;
}

void GL3_DrawAliasModelVoxels (entity_t *e, const float *viewproj)
{
	(void)e; (void)viewproj;
}

// ---------------------------------------------------------------- init/shutdown

void GL3_InitVoxels (void)
{
	r_voxelize          = ri.Cvar_Get ("r_voxelize", "0", CVAR_ARCHIVE);
	r_voxelsize         = ri.Cvar_Get ("r_voxelsize", "8", CVAR_ARCHIVE);
	r_voxelize_entities = ri.Cvar_Get ("r_voxelize_entities", "1", CVAR_ARCHIVE);
}

void GL3_ShutdownVoxels (void)
{
	FreeWorldVoxels ();
}
