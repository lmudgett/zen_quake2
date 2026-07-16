// gl3_local.h -- internal header for the modern OpenGL 3.3 core renderer.
// Works with the original Quake 2 refresh ABI (ref.h) and file formats.

#ifndef GL3_LOCAL_H
#define GL3_LOCAL_H

#include <glad/gl.h>
#include <SDL3/SDL.h>

#include "../client/ref.h"

#ifdef _WIN32
#define Q2_DLL_EXPORT __declspec(dllexport)
#else
#define Q2_DLL_EXPORT __attribute__((visibility("default")))
#endif

// the import table the engine hands us (printing, cvars, files, ...)
extern refimport_t	ri;

// degrees -> radians: one multiply by pi/180
#define DEG2RADF(deg)	((deg) * 0.017453292519943295f)

typedef enum { it_skin, it_sprite, it_wall, it_pic, it_sky } imagetype_t;

typedef struct image_s
{
	char		name[MAX_QPATH];
	imagetype_t	type;
	int			width, height;			// source dimensions
	int			registration_sequence;	// free unreferenced images between maps
	struct msurface_s	*texturechain;	// used by world rendering later
	GLuint		texnum;					// GL texture object
	GLuint		normaltex;				// tangent-space normal map (0 = none)
	qboolean	has_alpha;
} image_t;

#define MAX_GLTEXTURES	1024

typedef struct
{
	int			width, height;		// current drawable size in pixels
	int			vw, vh;				// virtual 2D resolution reported to the client
	float		scale;				// real pixels per virtual pixel (UI scale)
	qboolean	fullscreen;
	SDL_Window	*window;
	SDL_GLContext	context;

	int			currenttexture;		// bound GL texture cache
	int			stereo_eye;			// anaglyph pass: -1 left, +1 right, 0 mono
} gl3state_t;

extern gl3state_t	gl3state;

extern unsigned		d_8to24table[256];	// palette as RGBA
extern GLuint		gl3_flat_normal;	// 1x1 "no bump" normal map
extern image_t		gl3textures[MAX_GLTEXTURES];
extern int			numgl3textures;
extern int			registration_sequence;
extern image_t		*r_notexture;		// fallback checkerboard

// gl3_image.c -- Quake 2 asset loading (PCX/TGA/WAL) and GL texture upload
void     GL3_InitImages (void);
void     GL3_ShutdownImages (void);
image_t *GL3_FindImage (char *name, imagetype_t type);
image_t *GL3_LoadPic (char *name, byte *pic, int width, int height, imagetype_t type, int bits);
void     GL3_FreeUnusedImages (void);
void     GL3_UpdateAnisotropy (void);
void     GL3_TextureMode (const char *string);
void     GL3_ImageList_f (void);
void     GL3_Bind (GLuint texnum);

// gl3_shaders.c -- GLSL program management
typedef struct
{
	GLuint	program;
	GLint	u_ortho;		// mat4 for 2D
	GLint	u_color;		// vec4 tint
	GLint	u_gamma;		// float
	GLint	u_intensity;	// float
} gl3prog2d_t;

extern gl3prog2d_t	gl3_prog2d;

extern GLuint	white_tex;	// 1x1 white (gl3_draw.c)

// 3D world program
typedef struct
{
	GLuint	program;
	GLint	u_mvp;			// mat4
	GLint	u_gamma;
	GLint	u_intensity;
	GLint	u_lm_enabled;	// int (0/1)
	GLint	u_alpha;		// float surface alpha (1 opaque)
	GLint	u_scroll;		// SURF_FLOWING texture scroll
	GLint	u_num_dlights;	// gl_dynamic 2: per-pixel dynamic lights
	GLint	u_dlights;		// vec4[32] xyz + radius
	GLint	u_dlcolors;		// vec3[32]
	GLint	u_bump;			// normal-mapped dlights on/off
	GLint	u_tbn_t;		// per-surface tangent basis (texinfo axes)
	GLint	u_tbn_b;
	GLint	u_tbn_n;
	GLint	u_lightmode;	// dev: 1 = r_fullbright, 2 = gl_lightmap
} gl3prog3d_t;

extern gl3prog3d_t	gl3_prog3d;

// water/turb warp program (per-fragment sin distortion)
typedef struct
{
	GLuint	program;
	GLint	u_mvp;
	GLint	u_time;
	GLint	u_gamma;
	GLint	u_intensity;
	GLint	u_alpha;		// translucent water
	GLint	u_scroll;		// SURF_FLOWING scroll (raw units)
} gl3progwarp_t;
extern gl3progwarp_t	gl3_prog_warp;

void GL3_InitShaders (void);
void GL3_ShutdownShaders (void);
GLuint GL3_CompileProgram (const char *vtx, const char *frag);

// gl3_draw.c -- 2D drawing
void GL3_Draw_Init (void);
void GL3_Draw_Shutdown (void);
void GL3_Draw_SetOrtho (void);						// call each frame before 2D
void GL3_Draw_Flush (void);							// emit any batched quads
struct image_s *GL3_Draw_FindPic (char *name);
void GL3_Draw_GetPicSize (int *w, int *h, char *name);
void GL3_Draw_Pic (int x, int y, char *name);
void GL3_Draw_StretchPic (int x, int y, int w, int h, char *name);
void GL3_Draw_Char (int x, int y, int num);
void GL3_Draw_TileClear (int x, int y, int w, int h, char *name);
void GL3_Draw_Fill (int x, int y, int w, int h, int c);
void GL3_Draw_FadeScreen (void);
void GL3_Draw_PolyBlend (float r, float g, float b, float a);
void GL3_Draw_StretchRaw (int x, int y, int w, int h, int cols, int rows, byte *data);

// cvars
extern cvar_t	*gl_mode;
extern cvar_t	*vid_fullscreen;
extern cvar_t	*vid_gamma;
extern cvar_t	*gl_clear;
extern cvar_t	*gl_intensity;
extern cvar_t	*gl_wateralpha;
extern cvar_t	*gl_slimealpha;
extern cvar_t	*gl_2dscale;
extern cvar_t	*gl_modulate;	// registered lazily in gl3_surf.c
extern cvar_t	*gl_anisotropy;
extern cvar_t	*gl_shadows;		// soft blob shadows under entities
extern cvar_t	*gl_retexture;		// hi-res .png/.tga/.jpg texture overrides
extern cvar_t	*cache_assets;		// keep textures/models resident across maps
extern cvar_t	*gl_bump;			// 0 off, 1 _n maps only, 2 + auto-generate
extern cvar_t	*r_lefthand;		// "hand": 1 = mirrored view weapon, 2 = hidden
extern cvar_t	*gl_flashblend;		// dlights as additive glow balls, not lightmaps
extern cvar_t	*r_fullbright;		// dev: draw the world without lightmaps
extern cvar_t	*gl_lightmap;		// dev: draw lightmaps only
extern cvar_t	*r_speeds;			// dev: per-frame poly counts to the console
extern cvar_t	*gl_texturemode;	// GL_NEAREST .. GL_LINEAR_MIPMAP_LINEAR
extern cvar_t	*gl_msaa;			// gl3_post.c
extern cvar_t	*gl_renderscale;
extern cvar_t	*gl_bloom;
extern cvar_t	*r_voxelize;			// voxel render mode: rebuild the scene as cubes
extern cvar_t	*r_voxelsize;			// voxel grid resolution, world units
extern cvar_t	*r_voxelize_entities;	// also voxelize alias models + the view weapon

// gl3_post.c -- scene FBO and post-processing (gamma, underwater warp, bloom)
void  GL3_Post_Init (void);
void  GL3_Post_Shutdown (void);
void  GL3_Post_BeginScene (void);	// bind+clear the scene target
void  GL3_Post_EndScene (void);		// resolve, bloom, draw to the window
float GL3_Post_FrameScale (void);	// virtual 2D coords -> scene FBO pixels
int   GL3_Post_Width (void);
int   GL3_Post_Height (void);
GLuint GL3_Post_ResolveDepth (void);	// copy scene depth to a texture (soft particles)

// anisotropic filtering enums (extension is ubiquitous; glad may omit them)
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT		0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT	0x84FF
#endif

// anaglyph stereo channel mask for the current eye (gl3_main.c)
void GL3_ApplyStereoMask (void);

// gl3_glimp.c -- window / context management
void     GL3_ShutdownWindow (void);
void     GL3_StartFrame (void);		// make context current
void     GL3_SwapBuffers (void);
qboolean GL3_SetMode (int mode, qboolean fullscreen);
void     GL3_UpdateViddef (void);	// recompute UI scale, report size to client
void     GL3_CheckWindowChanges (void);	// apply fullscreen/mode/scale cvar changes

// ------------------------------------------------------------------ 3D / world

#include "gl3_model.h"

extern refdef_t	r_newrefdef;			// current view being rendered
extern int		r_framecount;
extern int		r_visframecount;
extern int		c_brush_polys;
extern int		c_alias_polys;

// 4x4 column-major matrix helpers (gl3_math.c)
void GL3_MatIdentity (float *m);
void GL3_MatMul (float *out, const float *a, const float *b);	// out = a*b
void GL3_MatPerspective (float *m, float fovy_deg, float aspect, float znear, float zfar);
void GL3_MatRotate (float *m, float deg, float x, float y, float z);
void GL3_MatTranslate (float *m, float x, float y, float z);

// lightmap / surface building, called by the model loader (gl3_surf.c)
void GL3_BeginBuildingLightmaps (model_t *m);
void GL3_CreateSurfaceLightmap (msurface_t *surf);
void GL3_EndBuildingLightmaps (void);

// world rendering (gl3_surf.c)
void GL3_BuildWorldVBO (void);			// upload all world polys after registration
void GL3_ShutdownSurf (void);			// free world VBO/VAO and lightmap textures
void GL3_MarkVisibleSurfaces (void);	// stamp surf->drawframe for this frame's PVS (voxel mode)
void GL3_UploadDlights (const vec3_t move);	// per-pixel dlight uniforms (gl_dynamic 2)
void GL3_DrawWorld (void);				// draw the visible world this frame
void GL3_DrawWorldTranslucent (void);	// TRANS33/66 surfaces, blended pass
void GL3_DrawWater (const float *viewproj, float time);	// turb/warp surfaces
void GL3_DrawBrushModel (entity_t *e, const float *viewproj);	// inline bmodels (doors/plats)
void GL3_MarkLeaves (void);				// flag the PVS-visible leafs/nodes
void GL3_PushDlights (void);			// mark surfaces touched by dlights
void GL3_SetFrustum (void);				// rebuild frustum planes from the refdef
qboolean GL3_CullBox (vec3_t mins, vec3_t maxs);	// true = fully outside the frustum

// entity alias models (gl3_mesh.c)
typedef struct
{
	GLuint	program;
	GLint	u_mvp;
	GLint	u_gamma;
	GLint	u_intensity;
	GLint	u_alphacut;		// fragment discard threshold (alpha-test replacement)
} gl3progalias_t;
extern gl3progalias_t	gl3_prog_alias;

void GL3_InitMesh (void);				// create VAO/VBO
void GL3_ShutdownMesh (void);
void GL3_DrawAliasModel (entity_t *e, const float *viewproj);
void GL3_DrawSpriteModel (entity_t *e, const float *viewproj);	// SP2 billboards
void GL3_DrawBeam (entity_t *e, const float *viewproj);		// RF_BEAM cylinders
void GL3_DrawNullModel (entity_t *e, const float *viewproj);	// missing-model diamond
void GL3_RenderDlights (const float *viewproj);				// gl_flashblend glow balls

// particles (gl3_particles.c): instanced billboard quads
typedef struct
{
	GLuint	program;
	GLint	u_mvp;
	GLint	u_right;		// camera axes for billboarding
	GLint	u_up;
	GLint	u_size;			// half-size, world units
	GLint	u_soft;			// soft-particle fade (1/range; 0 = off)
	GLint	u_invdepthsize;	// 1 / scene depth texture size
} gl3progpart_t;
extern gl3progpart_t	gl3_prog_part;

void GL3_InitParticles (void);
void GL3_ShutdownParticles (void);
void GL3_DrawParticles (const float *viewproj);

// voxel render mode (gl3_voxel.c): textured cube-face mesh, world + entities
typedef struct
{
	GLuint	program;
	GLint	u_mvp;			// projection * view
	GLint	u_model;		// model matrix (identity for the world)
	GLint	u_intensity;	// texture intensity, matches the world
	GLint	u_entcolor;		// per-entity light tint (world uses 1,1,1)
} gl3progvoxel_t;
extern gl3progvoxel_t	gl3_prog_voxel;

void GL3_InitVoxels (void);
void GL3_ShutdownVoxels (void);
void GL3_BuildWorldVoxels (void);
void GL3_DrawWorldVoxels (const float *viewproj);
void GL3_VoxelizeAliasModel (model_t *mod);
void GL3_DrawAliasModelVoxels (entity_t *e, const float *viewproj);

// sky (gl3_sky.c)
void GL3_InitSky (void);
void GL3_ShutdownSky (void);
void GL3_SetSky (char *name, float rotate, vec3_t axis);
void GL3_DrawSkyBox (const float *viewproj, const float *vieworg);

extern float	gl3_viewproj[16];		// projection * view for the current frame

#endif // GL3_LOCAL_H
