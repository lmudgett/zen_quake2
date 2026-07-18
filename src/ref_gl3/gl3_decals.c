// gl3_decals.c -- Quake 3-style impact marks: bullet holes, explosion
// scorches and energy burns projected onto world surfaces.
//
// GL3_AddDecal projects a textured square of the given radius onto nearby
// world surfaces: the BSP nodes within radius are walked (surfaces live on
// nodes), each facing surface's polygon is clipped to the projection box
// (Sutherland-Hodgman against the four side planes), and the fragments get
// ST coords from the tangent basis -- so marks wrap corners and trims like
// Q3's CG_ImpactMark. Directional impacts (bullets, blaster: dir = the
// impact plane normal from the temp entity) share one basis across all
// surfaces; explosions have no direction and project omnidirectionally,
// each surface using its own plane normal. Fragments live in a fixed
// vertex ring + one dynamic VBO, drawn depth-tested/no-write with a
// negative polygon offset right after the opaque world, and fade out at
// the end of their life. gl_decals 0 disables, gl_decaltime = seconds.
//
// Author: Len Mudgett

#include "gl3_local.h"

#define MAX_FRAG_VERTS	32		// one clipped fragment's vertex cap
#define DECAL_MAX_VERTS	480		// triangle-list verts one decal may take
#define DECAL_FADE		4.0f	// fade-out tail, seconds
#define DECAL_OFFSET	0.4f	// push off the wall, world units

cvar_t	*gl_decals;
static cvar_t	*gl_decaltime;

typedef struct
{
	vec3_t	pos;
	float	st[2];
	float	alpha;		// baked per-decal opacity (print fade steps) --
						// lets thousands of persistent marks draw batched
} dvert_t;

typedef struct
{
	qboolean	live;
	int			first, count;	// triangle-list range in the pool
	float		birth;			// r_newrefdef.time when stamped
	int			type;			// DECAL_* (ref.h)
	int			variant;		// texture pick for types with variants
	vec3_t		mins, maxs;		// world bounds for frustum culling
} decal_t;

// blood never washes off: pools, splats and boot prints outlive
// gl_decaltime. Each class rings through its OWN slot + vertex region, so
// nothing can evict across classes: firefight splatter churn can't recycle
// a death pool or a boot print -- only more of the same kind wrapping its
// ring, or a map change, takes persistent blood out.
//
// Region sizes are the single tuning knob: everything else (table ranges,
// totals) is derived, so one region can grow without touching the rest.
// Sized so a whole level's worth of carnage fits without ever wrapping:
// ~2000 kills of pools, ~8000 splats, ~2000 boot prints (VBO ~22MB).
#define POOL_SLOTS		2048	// death pools: ~1 per kill, big
#define POOL_VERTS		262144
#define SPLAT_SLOTS		8192	// splatter: many per fight, small
#define SPLAT_VERTS		524288
#define PRINT_SLOTS		2048	// boot prints: 8 per soak, tiny
#define PRINT_VERTS		131072
#define TRANS_SLOTS		256		// transient impact marks (fade anyway)
#define TRANS_VERTS		24576

#define MAX_DECALS	(POOL_SLOTS + SPLAT_SLOTS + PRINT_SLOTS + TRANS_SLOTS)
#define DECAL_POOL	(POOL_VERTS + SPLAT_VERTS + PRINT_VERTS + TRANS_VERTS)

enum { REG_POOL, REG_SPLAT, REG_PRINT, REG_TRANS, NUM_REGIONS };

typedef struct
{
	int	slot_lo, slot_hi;	// decal-slot range [lo, hi)
	int	vert_lo, vert_hi;	// vertex-ring range [lo, hi)
} dregdef_t;

static const dregdef_t regdefs[NUM_REGIONS] = {
	{ 0, POOL_SLOTS,
	  0, POOL_VERTS },
	{ POOL_SLOTS, POOL_SLOTS + SPLAT_SLOTS,
	  POOL_VERTS, POOL_VERTS + SPLAT_VERTS },
	{ POOL_SLOTS + SPLAT_SLOTS, MAX_DECALS - TRANS_SLOTS,
	  POOL_VERTS + SPLAT_VERTS, DECAL_POOL - TRANS_VERTS },
	{ MAX_DECALS - TRANS_SLOTS, MAX_DECALS,
	  DECAL_POOL - TRANS_VERTS, DECAL_POOL },
};
static int	reg_slot[NUM_REGIONS];		// next slot per region
static int	reg_vert[NUM_REGIONS];		// next free vert per region
static int	reg_hislot[NUM_REGIONS];	// highest slot ever used + 1: every
										// per-frame loop stops there instead
										// of walking thousands of dead slots
static int	reg_live[NUM_REGIONS];		// live decal count per region

static decal_t	decals[MAX_DECALS];

static GLuint	decal_vao, decal_vbo;
static GLuint	decal_prog;
static GLint	decal_u_mvp, decal_u_color, decal_u_grow;

#define HEAT_TIME	2.5f	// seconds a fresh energy burn stays hot
static GLuint	heat_prog;
static GLint	heat_u_mvp, heat_u_invscreen, heat_u_time, heat_u_heat;

#define POOL_VARIANTS	3
static image_t	*decal_images[8];
static image_t	*pool_images[POOL_VARIANTS];
static int		decal_images_seq = -1;

#define POOL_GROW_TIME	4.0f	// seconds a death pool takes to spread

// scratch for the decal being built
static dvert_t	build_verts[DECAL_MAX_VERTS];
static int		build_count;
static vec3_t	build_mins, build_maxs;

static const char *decal_vs =
	"#version 330 core\n"
	"in vec3 a_pos;\n"
	"in vec2 a_uv;\n"
	"in float a_alpha;\n"
	"uniform mat4 u_mvp;\n"
	"out vec2 v_uv;\n"
	"out float v_alpha;\n"
	"void main() {\n"
	"	gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
	"	v_uv = a_uv;\n"
	"	v_alpha = a_alpha;\n"
	"}\n";

// u_grow rescales the decal-local ST about its center so a mark can spread
// after being stamped (blood pools); the clamp keeps the shrunken sampling
// window from wrapping into the GL_REPEAT neighbors (borders are transparent)
static const char *decal_fs =
	"#version 330 core\n"
	"in vec2 v_uv;\n"
	"in float v_alpha;\n"
	"uniform sampler2D u_tex;\n"
	"uniform vec4 u_color;\n"
	"uniform float u_grow;\n"
	"out vec4 frag;\n"
	"void main() {\n"
	"	vec2 uv = clamp((v_uv - 0.5) / u_grow + 0.5, 0.0, 1.0);\n"
	"	frag = texture(u_tex, uv) * u_color;\n"
	"	frag.a *= v_alpha;\n"
	"}\n";

// heat-haze pass: a fresh energy burn shimmers -- the decal quad re-draws
// the grabbed scene behind it with a small time-animated swirl, masked by
// the decal texture's coverage so the wobble hugs the burn
static const char *heat_fs =
	"#version 330 core\n"
	"in vec2 v_uv;\n"
	"uniform sampler2D u_tex;\n"
	"uniform sampler2D u_scene;\n"
	"uniform vec2 u_invscreen;\n"
	"uniform float u_time;\n"
	"uniform float u_heat;\n"
	"out vec4 frag;\n"
	"void main() {\n"
	"	float m = texture(u_tex, v_uv).a * u_heat;\n"
	"	vec2 suv = gl_FragCoord.xy * u_invscreen;\n"
	"	vec2 off = vec2(sin(u_time*9.0 + v_uv.y*28.0 + v_uv.x*11.0),\n"
	"	                cos(u_time*7.3 + v_uv.x*24.0)) * (0.010 * m);\n"
	"	frag = vec4(texture(u_scene, suv + off).rgb, m);\n"
	"}\n";

void GL3_InitDecals (void)
{
	gl_decals = ri.Cvar_Get ("gl_decals", "1", CVAR_ARCHIVE);
	gl_decaltime = ri.Cvar_Get ("gl_decaltime", "60", CVAR_ARCHIVE);

	decal_prog = GL3_CompileProgram (decal_vs, decal_fs);
	decal_u_mvp = glGetUniformLocation (decal_prog, "u_mvp");
	decal_u_color = glGetUniformLocation (decal_prog, "u_color");
	decal_u_grow = glGetUniformLocation (decal_prog, "u_grow");
	glUseProgram (decal_prog);
	glUniform1i (glGetUniformLocation (decal_prog, "u_tex"), 0);
	glUseProgram (0);

	heat_prog = GL3_CompileProgram (decal_vs, heat_fs);
	heat_u_mvp = glGetUniformLocation (heat_prog, "u_mvp");
	heat_u_invscreen = glGetUniformLocation (heat_prog, "u_invscreen");
	heat_u_time = glGetUniformLocation (heat_prog, "u_time");
	heat_u_heat = glGetUniformLocation (heat_prog, "u_heat");
	glUseProgram (heat_prog);
	glUniform1i (glGetUniformLocation (heat_prog, "u_tex"), 0);
	glUniform1i (glGetUniformLocation (heat_prog, "u_scene"), 1);
	glUseProgram (0);

	glGenVertexArrays (1, &decal_vao);
	glGenBuffers (1, &decal_vbo);
	glBindVertexArray (decal_vao);
	glBindBuffer (GL_ARRAY_BUFFER, decal_vbo);
	glBufferData (GL_ARRAY_BUFFER, DECAL_POOL * sizeof(dvert_t), NULL, GL_DYNAMIC_DRAW);
	glEnableVertexAttribArray (0);
	glVertexAttribPointer (0, 3, GL_FLOAT, GL_FALSE, sizeof(dvert_t), (void *)0);
	glEnableVertexAttribArray (1);
	glVertexAttribPointer (1, 2, GL_FLOAT, GL_FALSE, sizeof(dvert_t),
		(void *)offsetof(dvert_t, st));
	glEnableVertexAttribArray (2);
	glVertexAttribPointer (2, 1, GL_FLOAT, GL_FALSE, sizeof(dvert_t),
		(void *)offsetof(dvert_t, alpha));
	glBindVertexArray (0);

	GL3_DecalsNewMap ();	// region cursors start valid before any map
}

void GL3_ShutdownDecals (void)
{
	if (decal_vbo)	glDeleteBuffers (1, &decal_vbo);
	if (decal_vao)	glDeleteVertexArrays (1, &decal_vao);
	if (decal_prog)	glDeleteProgram (decal_prog);
	if (heat_prog)	glDeleteProgram (heat_prog);
	decal_vbo = decal_vao = decal_prog = heat_prog = 0;
}

// map change: every mark belongs to the old world
void GL3_DecalsNewMap (void)
{
	memset (decals, 0, sizeof(decals));
	for (int i = 0; i < NUM_REGIONS; i++)
	{
		reg_slot[i] = regdefs[i].slot_lo;
		reg_vert[i] = regdefs[i].vert_lo;
		reg_hislot[i] = regdefs[i].slot_lo;
		reg_live[i] = 0;
	}
}

static image_t *DecalImage (int type)
{
	if (decal_images_seq != registration_sequence)
	{	// (re)resolve after map loads; FindImage loads from disk on a miss
		decal_images[DECAL_BULLET] = GL3_FindImage ("textures/decals/bullet.tga", it_sprite);
		decal_images[DECAL_SCORCH] = GL3_FindImage ("textures/decals/scorch.tga", it_sprite);
		decal_images[DECAL_ENERGY] = GL3_FindImage ("textures/decals/energy.tga", it_sprite);
		decal_images[DECAL_BFG] = decal_images[DECAL_SCORCH];	// green-tinted at draw
		decal_images[DECAL_RAIL] = GL3_FindImage ("textures/decals/rail.tga", it_sprite);
		decal_images[DECAL_BLOOD] = GL3_FindImage ("textures/decals/blood.tga", it_sprite);
		pool_images[0] = GL3_FindImage ("textures/decals/pool.tga", it_sprite);
		pool_images[1] = GL3_FindImage ("textures/decals/pool1.tga", it_sprite);
		pool_images[2] = GL3_FindImage ("textures/decals/pool2.tga", it_sprite);
		decal_images[DECAL_BLOOD_POOL] = pool_images[0];
		decal_images[DECAL_FOOTPRINT] = GL3_FindImage ("textures/decals/footprint.tga", it_sprite);
		decal_images_seq = registration_sequence;
	}
	if (type < 0 || type > DECAL_FOOTPRINT)
		type = DECAL_SCORCH;
	return decal_images[type];
}

// pools come in a few silhouettes so repeated deaths don't stamp clones
static image_t *PoolImage (int variant)
{
	image_t	*img = DecalImage (DECAL_BLOOD_POOL);	// ensures (re)resolve

	if (pool_images[variant % POOL_VARIANTS])
		img = pool_images[variant % POOL_VARIANTS];
	return img;
}

/*
=================
ClipFragment

Sutherland-Hodgman: clip a polygon to the inside (dist <= limit) of one
plane. Returns the new vertex count.
=================
*/
static int ClipFragment (int n, vec3_t in[], vec3_t normal, float limit, vec3_t out[])
{
	int		i, m = 0;
	float	da, db;

	for (i = 0; i < n && m < MAX_FRAG_VERTS - 1; i++)
	{
		float	*a = in[i], *b = in[(i+1) % n];

		da = DotProduct (a, normal) - limit;
		db = DotProduct (b, normal) - limit;

		if (da <= 0)
		{
			VectorCopy (a, out[m]); m++;
		}
		if (m < MAX_FRAG_VERTS - 1 && ((da < 0 && db > 0) || (da > 0 && db < 0)))
		{
			float	f = da / (da - db);
			vec3_t	delta;

			VectorSubtract (b, a, delta);
			VectorMA (a, f, delta, out[m]); m++;
		}
	}
	return m;
}

/*
=================
DecalSurface

Clip one surface's polygon to the projection square (side planes through
origin along the tangent axes) and append the fragment.
=================
*/
static void DecalSurface (msurface_t *surf, vec3_t origin, vec3_t t1, vec3_t t2,
	float radius, vec3_t snorm)
{
	static vec3_t	buf_a[MAX_FRAG_VERTS], buf_b[MAX_FRAG_VERTS];
	glpoly_t	*p;
	int			i, n;

	for (p = surf->polys; p; p = p->next)
	{
		float	inv = 1.0f / (2.0f * radius);

		n = p->numverts;
		if (n < 3 || n > MAX_FRAG_VERTS - 4)
			continue;
		for (i = 0; i < n; i++)
			VectorCopy (p->verts[i], buf_a[i]);

		// clip to the four side planes of the projection square
		n = ClipFragment (n, buf_a, t1,  radius + DotProduct (origin, t1), buf_b);
		if (n < 3) continue;
		n = ClipFragment (n, buf_b, t2,  radius + DotProduct (origin, t2), buf_a);
		if (n < 3) continue;
		{
			vec3_t	neg1 = { -t1[0], -t1[1], -t1[2] };
			vec3_t	neg2 = { -t2[0], -t2[1], -t2[2] };
			n = ClipFragment (n, buf_a, neg1, radius - DotProduct (origin, t1), buf_b);
			if (n < 3) continue;
			n = ClipFragment (n, buf_b, neg2, radius - DotProduct (origin, t2), buf_a);
			if (n < 3) continue;
		}

		if (build_count + (n - 2) * 3 > DECAL_MAX_VERTS)
			return;

		// fragment -> fan triangles with projected ST, floated off the wall
		for (i = 2; i < n; i++)
		{
			int	idx[3] = { 0, i - 1, i };
			int	k;
			for (k = 0; k < 3; k++)
			{
				dvert_t	*v = &build_verts[build_count++];
				vec3_t	d;

				VectorMA (buf_a[idx[k]], DECAL_OFFSET, snorm, v->pos);
				VectorSubtract (buf_a[idx[k]], origin, d);
				v->st[0] = DotProduct (d, t1) * inv + 0.5f;
				v->st[1] = DotProduct (d, t2) * inv + 0.5f;

				AddPointToBounds (v->pos, build_mins, build_maxs);
			}
		}
	}
}

/*
=================
DecalNode

Walk the world nodes touching the impact sphere; surfaces live on nodes.
=================
*/
static void DecalNode (mnode_t *node, vec3_t origin, vec3_t dir, qboolean directional,
	vec3_t t1, vec3_t t2, float radius, float rot)
{
	float		dist;
	msurface_t	*surf;
	int			i;

	if (node->contents != -1)
		return;			// leaf

	dist = DotProduct (origin, node->plane->normal) - node->plane->dist;
	if (dist > radius)
	{
		DecalNode (node->children[0], origin, dir, directional, t1, t2, radius, rot);
		return;
	}
	if (dist < -radius)
	{
		DecalNode (node->children[1], origin, dir, directional, t1, t2, radius, rot);
		return;
	}

	surf = r_worldmodel->surfaces + node->firstsurface;
	for (i = 0; i < node->numsurfaces; i++, surf++)
	{
		vec3_t	snorm, s1, s2;
		float	d;

		if (surf->flags & (SURF_DRAWSKY | SURF_DRAWTURB))
			continue;	// sky and liquids don't take marks
		if (surf->texinfo && (surf->texinfo->flags &
				(SURF_SKY | SURF_WARP | SURF_TRANS33 | SURF_TRANS66 | SURF_NODRAW)))
			continue;	// nor glass / invisible surfaces
		if (!surf->polys)
			continue;

		VectorCopy (surf->plane->normal, snorm);
		d = DotProduct (origin, snorm) - surf->plane->dist;
		if (surf->flags & SURF_PLANEBACK)
		{
			VectorNegate (snorm, snorm);
			d = -d;
		}
		// impact positions arrive wire-quantized (1/8-unit shorts) and trace
		// endpoints sit ON the plane, so d is often ~0 or a hair negative for
		// the very surface that was hit -- accept slightly behind. Fragment
		// verts come from the surface polys themselves (then float off along
		// the normal), so a small negative d can't push geometry into walls.
		if (d < -1.0f || d > radius)
			continue;	// well behind the face, or too far off it

		if (directional)
		{
			if (DotProduct (snorm, dir) < 0.4f)
				continue;	// facing away from the impact
			DecalSurface (surf, origin, t1, t2, radius, snorm);
		}
		else
		{
			// omni (explosions): project along this surface's own normal,
			// tangents from the normal + the decal's random roll
			vec3_t	ref = { 0, 0, 1 };
			if (fabsf (snorm[2]) > 0.7f)
				ref[0] = 1, ref[2] = 0;
			CrossProduct (snorm, ref, s1);
			VectorNormalize (s1);
			CrossProduct (snorm, s1, s2);
			{	// roll the basis so repeated blasts don't align
				vec3_t	r1, r2;
				float	c = cosf (rot), s = sinf (rot);
				for (int k = 0; k < 3; k++)
				{
					r1[k] = c * s1[k] + s * s2[k];
					r2[k] = -s * s1[k] + c * s2[k];
				}
				DecalSurface (surf, origin, r1, r2, radius, snorm);
			}
		}
	}

	DecalNode (node->children[0], origin, dir, directional, t1, t2, radius, rot);
	DecalNode (node->children[1], origin, dir, directional, t1, t2, radius, rot);
}

/*
=================
StampDecal

Commit the fragments accumulated in build_verts as one decal: ring-allocate
pool space (retiring whatever the range runs over) and upload.
=================
*/
static void StampDecal (int type, float basealpha)
{
	const dregdef_t	*rd;
	decal_t	*dc;
	int		reg, need, *head, *slot;

	// each decal class rings through its own slot + vertex region, so
	// churn in one class never evicts another (splatter can't eat pools)
	if (type == DECAL_BLOOD_POOL)		reg = REG_POOL;
	else if (type == DECAL_BLOOD)		reg = REG_SPLAT;
	else if (type == DECAL_FOOTPRINT)	reg = REG_PRINT;
	else								reg = REG_TRANS;
	rd = &regdefs[reg];
	head = &reg_vert[reg];
	slot = &reg_slot[reg];

	// ring-allocate vertex space; retire whatever the range runs over.
	// Regions own disjoint vertex ranges, so only this region's slots
	// can overlap -- no need to scan the other 10k+.
	need = build_count;
	if (*head + need > rd->vert_hi)
		*head = rd->vert_lo;
	for (int i = rd->slot_lo; i < reg_hislot[reg]; i++)
	{
		if (decals[i].live && decals[i].first < *head + need
			&& decals[i].first + decals[i].count > *head)
		{
			decals[i].live = false;
			reg_live[reg]--;
		}
	}

	dc = &decals[*slot];
	*slot = *slot + 1;
	if (*slot > reg_hislot[reg])
		reg_hislot[reg] = *slot;
	if (*slot >= rd->slot_hi)
		*slot = rd->slot_lo;

	// opacity rides in the vertices so persistent marks can draw batched
	for (int i = 0; i < build_count; i++)
		build_verts[i].alpha = basealpha;

	if (dc->live)
		reg_live[reg]--;	// slot ring wrapped onto a live decal
	dc->live = true;
	reg_live[reg]++;
	dc->first = *head;
	dc->count = build_count;
	dc->birth = r_newrefdef.time;
	dc->type = type;
	dc->variant = rand () % POOL_VARIANTS;
	VectorCopy (build_mins, dc->mins);
	VectorCopy (build_maxs, dc->maxs);

	glBindBuffer (GL_ARRAY_BUFFER, decal_vbo);
	glBufferSubData (GL_ARRAY_BUFFER, *head * sizeof(dvert_t),
		build_count * sizeof(dvert_t), build_verts);
	glBindBuffer (GL_ARRAY_BUFFER, 0);

	*head += need;
}

/*
=================
GL3_AddDecal

Stamp an impact mark. dir = impact surface normal for bullets/blaster
(directional); NULL or near-zero for explosions (omni). Exported to the
client through refexport_t.
=================
*/
void GL3_AddDecal (vec3_t origin, vec3_t dir, float radius, int type)
{
	vec3_t		t1, t2, axis;
	qboolean	directional;
	float		rot;

	if (!gl_decals || !gl_decals->value || !r_worldmodel || radius <= 0)
		return;
	if (r_newrefdef.rdflags & RDF_NOWORLDMODEL)
		return;

	directional = dir && VectorLength (dir) > 0.5f;
	rot = (rand () & 1023) * (2.0f * (float)M_PI / 1024.0f);

	if (directional)
	{	// shared tangent basis, randomly rolled around the impact normal
		vec3_t	ref = { 0, 0, 1 }, b1, b2;
		VectorCopy (dir, axis);
		VectorNormalize (axis);
		if (fabsf (axis[2]) > 0.7f)
			ref[0] = 1, ref[2] = 0;
		CrossProduct (axis, ref, b1);
		VectorNormalize (b1);
		CrossProduct (axis, b1, b2);
		for (int k = 0; k < 3; k++)
		{
			t1[k] = cosf (rot) * b1[k] + sinf (rot) * b2[k];
			t2[k] = -sinf (rot) * b1[k] + cosf (rot) * b2[k];
		}
	}
	else
		VectorClear (axis);

	build_count = 0;
	ClearBounds (build_mins, build_maxs);
	DecalNode (r_worldmodel->nodes, origin, axis, directional, t1, t2, radius, rot);

	if (build_count < 3)
		return;		// hit nothing markable

	StampDecal (type, 1.0f);
}

/*
=================
GL3_AddDecalOriented

Stamp a mark with a chosen heading (boot prints): forward, projected onto
the surface plane, becomes the texture's +V axis so the mark points the
way the walker is facing. alpha scales opacity (prints fade step by step).
=================
*/
void GL3_AddDecalOriented (vec3_t origin, vec3_t normal, vec3_t forward,
	float radius, float alpha, int type)
{
	vec3_t	axis, t1, t2;
	float	d;

	if (!gl_decals || !gl_decals->value || !r_worldmodel || radius <= 0)
		return;
	if (r_newrefdef.rdflags & RDF_NOWORLDMODEL)
		return;

	VectorCopy (normal, axis);
	if (VectorNormalize (axis) < 0.5f)
		return;

	// texture "up" = forward flattened onto the surface plane
	d = DotProduct (forward, axis);
	VectorMA (forward, -d, axis, t2);
	if (VectorNormalize (t2) < 0.01f)
		return;		// looking straight along the normal: no heading
	CrossProduct (axis, t2, t1);

	build_count = 0;
	ClearBounds (build_mins, build_maxs);
	DecalNode (r_worldmodel->nodes, origin, axis, true, t1, t2, radius, 0);

	if (build_count < 3)
		return;

	StampDecal (type, alpha);
}

/*
=================
GL3_BloodPoolAt

Is live blood lying on the floor at this point? Used by the client to
soak boots that step through it. Death pools and splatter both count;
splats standing on walls don't (bounds tall in z relative to their
footprint), and neither do already-stamped boot prints -- walking your
own trail shouldn't re-soak forever. 2D bounds check with a little
slack, loose in z (the blood hugs a floor near the query point).

origin == NULL asks only "is there ANY blood at all?" -- an O(1) count
check the client uses to skip its per-frame floor trace on clean maps.
=================
*/
qboolean GL3_BloodPoolAt (vec3_t origin)
{
	static const int	blood_regs[2] = { REG_POOL, REG_SPLAT };

	if (!origin)
		return reg_live[REG_POOL] + reg_live[REG_SPLAT] > 0;

	for (int r = 0; r < 2; r++)
	{
		const dregdef_t	*rd = &regdefs[blood_regs[r]];

		for (int i = rd->slot_lo; i < reg_hislot[blood_regs[r]]; i++)
		{
			decal_t	*dc = &decals[i];
			float	xext, yext, zext;

			if (!dc->live)
				continue;
			xext = dc->maxs[0] - dc->mins[0];
			yext = dc->maxs[1] - dc->mins[1];
			zext = dc->maxs[2] - dc->mins[2];
			if (zext > 0.5f * (xext > yext ? xext : yext))
				continue;	// standing on a wall, not lying on the floor
			if (origin[0] < dc->mins[0] - 2 || origin[0] > dc->maxs[0] + 2)
				continue;
			if (origin[1] < dc->mins[1] - 2 || origin[1] > dc->maxs[1] + 2)
				continue;
			if (origin[2] < dc->mins[2] - 24 || origin[2] > dc->maxs[2] + 24)
				continue;
			return true;
		}
	}
	return false;
}

/*
=================
GL3_DrawDecals

Right after the opaque world: depth-tested, no depth write, alpha-blended,
pulled toward the eye with a polygon offset against z-fighting.
=================
*/
/*
=================
DrawPersistentRegion

A level's worth of blood is thousands of little decals -- far too many
for a draw call each. Persistent marks never fade and their opacity is
baked into the vertices, so consecutive live decals sharing a texture
draw as ONE glDrawArrays over their contiguous vertex range. Only a
still-growing death pool (needs its own u_grow) breaks out of the batch.
=================
*/
static void DrawPersistentRegion (int reg, float now, GLuint *lasttex)
{
	const dregdef_t	*rd = &regdefs[reg];
	int		i, run_first = -1, run_count = 0;

	for (i = rd->slot_lo; i < reg_hislot[reg]; i++)
	{
		decal_t	*dc = &decals[i];
		image_t	*img;
		qboolean	growing;

		if (dc->live && now - dc->birth < -1.0f)
		{
			dc->live = false;		// server time restarted under us
			reg_live[reg]--;
		}
		if (!dc->live)
			continue;
		if (GL3_CullBox (dc->mins, dc->maxs))
		{	// off-screen: close the run -- with the blood behind the
			// player this culls the whole region to zero draws
			if (run_count)
			{
				glDrawArrays (GL_TRIANGLES, run_first, run_count);
				run_count = 0;
			}
			continue;
		}

		if (dc->type == DECAL_BLOOD_POOL)
			img = PoolImage (dc->variant);
		else
			img = DecalImage (dc->type);
		if (!img)
			continue;
		growing = (dc->type == DECAL_BLOOD_POOL
			&& now - dc->birth < POOL_GROW_TIME);

		if (run_count && (growing || img->texnum != *lasttex
			|| dc->first != run_first + run_count))
		{
			glDrawArrays (GL_TRIANGLES, run_first, run_count);
			run_count = 0;
		}
		if (img->texnum != *lasttex)
		{
			GL3_Bind (img->texnum);
			*lasttex = img->texnum;
		}

		if (growing)
		{
			glUniform1f (decal_u_grow,
				0.35f + 0.65f * ((now - dc->birth) / POOL_GROW_TIME));
			glDrawArrays (GL_TRIANGLES, dc->first, dc->count);
			glUniform1f (decal_u_grow, 1.0f);
			continue;
		}
		if (!run_count)
			run_first = dc->first;
		run_count += dc->count;
	}
	if (run_count)
		glDrawArrays (GL_TRIANGLES, run_first, run_count);
}

void GL3_DrawDecals (const float *viewproj)
{
	float	life, now;
	GLuint	lasttex = 0;
	int		i;

	if (!gl_decals || !gl_decals->value)
		return;

	life = gl_decaltime ? gl_decaltime->value : 60;
	if (life < 5)
		life = 5;
	now = r_newrefdef.time;

	glUseProgram (decal_prog);
	glUniformMatrix4fv (decal_u_mvp, 1, GL_FALSE, viewproj);
	glBindVertexArray (decal_vao);
	glEnable (GL_BLEND);
	glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDepthMask (GL_FALSE);
	glEnable (GL_POLYGON_OFFSET_FILL);
	glPolygonOffset (-1.0f, -2.0f);
	glActiveTexture (GL_TEXTURE0);

	// persistent blood: batched, never fades, vertex alpha carries the
	// print fade-steps
	glUniform1f (decal_u_grow, 1.0f);
	glUniform4f (decal_u_color, 1.0f, 1.0f, 1.0f, 1.0f);
	DrawPersistentRegion (REG_POOL, now, &lasttex);
	DrawPersistentRegion (REG_SPLAT, now, &lasttex);
	DrawPersistentRegion (REG_PRINT, now, &lasttex);

	// transient impact marks: few, individually faded and tinted
	for (i = regdefs[REG_TRANS].slot_lo; i < reg_hislot[REG_TRANS]; i++)
	{
		decal_t	*dc = &decals[i];
		float	age, alpha;
		image_t	*img;

		if (!dc->live)
			continue;
		age = now - dc->birth;
		if (age > life || age < -1.0f)	// -1: server time restarted under us
		{
			dc->live = false;
			reg_live[REG_TRANS]--;
			continue;
		}
		if (GL3_CullBox (dc->mins, dc->maxs))
			continue;

		alpha = (life - age) / DECAL_FADE;
		if (alpha > 1.0f)
			alpha = 1.0f;

		img = DecalImage (dc->type);
		if (!img)
			continue;
		if (img->texnum != lasttex)
		{
			GL3_Bind (img->texnum);
			lasttex = img->texnum;
		}

		if (dc->type == DECAL_BFG)
			glUniform4f (decal_u_color, 0.45f, 1.0f, 0.55f, alpha);
		else if (dc->type == DECAL_ENERGY && age < HEAT_TIME)
		{	// fresh burn glows hot: push the embers past 1.0 -- the HDR
			// scene target keeps it and the bloom pass makes it radiate
			float	heat = 1.0f - age / HEAT_TIME;
			glUniform4f (decal_u_color, 1.0f + 3.0f * heat,
				1.0f + 1.6f * heat, 1.0f + 0.4f * heat, alpha);
		}
		else
			glUniform4f (decal_u_color, 1.0f, 1.0f, 1.0f, alpha);

		glDrawArrays (GL_TRIANGLES, dc->first, dc->count);
	}

	glDisable (GL_POLYGON_OFFSET_FILL);
	glDepthMask (GL_TRUE);
	glDisable (GL_BLEND);
	glBindVertexArray (0);
}

/*
=================
GL3_DrawDecalsHeat

Heat shimmer over fresh energy burns: re-draws the grabbed scene through a
time-animated swirl on each young DECAL_ENERGY fragment. Runs after the
refraction scene grab (scene_tex = the resolved color copy; 0 = skip, e.g.
gl_refraction 0), so it distorts everything already drawn behind it.
=================
*/
void GL3_DrawDecalsHeat (const float *viewproj, GLuint scene_tex)
{
	float	now;
	int		i, bound = 0;

	if (!scene_tex || !gl_decals || !gl_decals->value)
		return;

	now = r_newrefdef.time;

	// energy burns are transient decals: only that region can hold them
	for (i = regdefs[REG_TRANS].slot_lo; i < reg_hislot[REG_TRANS]; i++)
	{
		decal_t	*dc = &decals[i];
		float	age, heat;

		if (!dc->live || dc->type != DECAL_ENERGY)
			continue;
		age = now - dc->birth;
		if (age < 0 || age >= HEAT_TIME)
			continue;
		if (GL3_CullBox (dc->mins, dc->maxs))
			continue;

		if (!bound)
		{	// lazy state setup: most frames have no hot burns
			image_t	*img = DecalImage (DECAL_ENERGY);
			if (!img)
				return;
			glUseProgram (heat_prog);
			glUniformMatrix4fv (heat_u_mvp, 1, GL_FALSE, viewproj);
			glUniform2f (heat_u_invscreen,
				1.0f / GL3_Post_Width (), 1.0f / GL3_Post_Height ());
			glUniform1f (heat_u_time, now);
			glActiveTexture (GL_TEXTURE1);
			glBindTexture (GL_TEXTURE_2D, scene_tex);
			glActiveTexture (GL_TEXTURE0);
			GL3_Bind (img->texnum);
			glBindVertexArray (decal_vao);
			glEnable (GL_BLEND);
			glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			glDepthMask (GL_FALSE);
			glEnable (GL_POLYGON_OFFSET_FILL);
			glPolygonOffset (-1.0f, -2.0f);
			bound = 1;
		}

		heat = 1.0f - age / HEAT_TIME;
		glUniform1f (heat_u_heat, heat);
		glDrawArrays (GL_TRIANGLES, dc->first, dc->count);
	}

	if (bound)
	{
		glDisable (GL_POLYGON_OFFSET_FILL);
		glDepthMask (GL_TRUE);
		glDisable (GL_BLEND);
		glBindVertexArray (0);
	}
}
