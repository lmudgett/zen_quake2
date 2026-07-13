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

typedef enum { it_skin, it_sprite, it_wall, it_pic, it_sky } imagetype_t;

typedef struct image_s
{
	char		name[MAX_QPATH];
	imagetype_t	type;
	int			width, height;			// source dimensions
	int			registration_sequence;	// free unreferenced images between maps
	struct msurface_s	*texturechain;	// used by world rendering later
	GLuint		texnum;					// GL texture object
	qboolean	has_alpha;
} image_t;

#define MAX_GLTEXTURES	1024

typedef struct
{
	int			width, height;		// current drawable size in pixels
	qboolean	fullscreen;
	SDL_Window	*window;
	SDL_GLContext	context;

	int			currenttexture;		// bound GL texture cache
} gl3state_t;

extern gl3state_t	gl3state;

extern unsigned		d_8to24table[256];	// palette as RGBA
extern image_t		gl3textures[MAX_GLTEXTURES];
extern int			numgl3textures;
extern int			registration_sequence;
extern image_t		*r_notexture;		// fallback checkerboard

// gl3_image.c -- Quake 2 asset loading (PCX/TGA/WAL) and GL texture upload
void     GL3_InitImages (void);
void     GL3_ShutdownImages (void);
image_t *GL3_FindImage (char *name, imagetype_t type);
image_t *GL3_LoadPic (char *name, byte *pic, int width, int height, imagetype_t type, int bits);
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

// 3D world program
typedef struct
{
	GLuint	program;
	GLint	u_mvp;			// mat4
	GLint	u_gamma;
	GLint	u_intensity;
	GLint	u_lm_enabled;	// int (0/1)
} gl3prog3d_t;

extern gl3prog3d_t	gl3_prog3d;

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
void GL3_Draw_StretchRaw (int x, int y, int w, int h, int cols, int rows, byte *data);

// cvars
extern cvar_t	*gl_mode;
extern cvar_t	*vid_fullscreen;
extern cvar_t	*vid_gamma;
extern cvar_t	*gl_clear;
extern cvar_t	*gl_intensity;

// gl3_glimp.c -- window / context management
void     GL3_ShutdownWindow (void);
void     GL3_StartFrame (void);		// make context current
void     GL3_SwapBuffers (void);
qboolean GL3_SetMode (int mode, qboolean fullscreen);

// ------------------------------------------------------------------ 3D / world

#include "gl3_model.h"

extern refdef_t	r_newrefdef;			// current view being rendered
extern int		r_framecount;
extern int		r_visframecount;
extern int		c_brush_polys;

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
void GL3_DrawWorld (void);				// draw the visible world this frame
void GL3_MarkLeaves (void);

// entity alias models (gl3_mesh.c)
typedef struct
{
	GLuint	program;
	GLint	u_mvp;
	GLint	u_gamma;
	GLint	u_intensity;
} gl3progalias_t;
extern gl3progalias_t	gl3_prog_alias;

void GL3_InitMesh (void);				// create VAO/VBO
void GL3_ShutdownMesh (void);
void GL3_DrawAliasModel (entity_t *e, const float *viewproj);

extern float	gl3_viewproj[16];		// projection * view for the current frame

#endif // GL3_LOCAL_H
