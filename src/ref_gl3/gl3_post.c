// gl3_post.c -- scene framebuffer and post-processing for the GL 3.3
// renderer. The 3D view renders into an offscreen FBO (optionally
// multisampled and scaled by gl_renderscale); a final pass draws it to the
// window applying gamma once (matching id's hardware ramp, which acted on
// the fully blended image), the underwater screen warp, and bloom.

#include "gl3_local.h"

cvar_t	*gl_msaa;			// FBO sample count (0/2/4/8), vid-restart-free
cvar_t	*gl_renderscale;	// internal 3D resolution factor (0.25 .. 2.0)
cvar_t	*gl_bloom;			// bloom strength (0 = off, ~0.4 default look)

typedef struct
{
	GLuint	program;
	GLint	u_gamma;
	GLint	u_time;
	GLint	u_warp;			// RDF_UNDERWATER screen warp on/off
	GLint	u_bloom;		// bloom add strength
} gl3progpost_t;

typedef struct
{
	GLuint	program;
} gl3progbright_t;

typedef struct
{
	GLuint	program;
	GLint	u_dir;			// blur direction (1/w,0) or (0,1/h)
} gl3progblur_t;

static gl3progpost_t	prog_post;
static gl3progbright_t	prog_bright;
static gl3progblur_t	prog_blur;

// scene target (multisampled render target + resolved texture)
static GLuint	fbo_ms;				// multisampled FBO (0 when gl_msaa 0)
static GLuint	rb_ms_color, rb_ms_depth;
static GLuint	fbo_scene;			// resolve target / direct target
static GLuint	tex_scene;
static GLuint	rb_scene_depth;		// depth when rendering directly (no MSAA)

// bloom ping-pong at half resolution
static GLuint	fbo_bloom[2];
static GLuint	tex_bloom[2];

// depth copy for soft particles (sampling the live depth attachment while
// it is bound for rendering would be undefined)
static GLuint	fbo_depthcopy;
static GLuint	tex_depthcopy;

static int		fb_width, fb_height;	// current scene FBO size
static int		fb_samples;
static int		bloom_w, bloom_h;

static GLuint	post_vao;			// empty VAO for attribute-less fullscreen tri

// ------------------------------------------------------------------ shaders

static const char *vtxPost =
	"#version 330 core\n"
	"out vec2 v_uv;\n"
	"void main() {\n"	// attribute-less fullscreen triangle
	"    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
	"    v_uv = p;\n"
	"    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
	"}\n";

static const char *fragPost =
	"#version 330 core\n"
	"in vec2 v_uv;\n"
	"uniform sampler2D u_scene;\n"
	"uniform sampler2D u_bloomtex;\n"
	"uniform float u_gamma;\n"
	"uniform float u_time;\n"
	"uniform int u_warp;\n"
	"uniform float u_bloom;\n"
	"out vec4 frag;\n"
	"void main() {\n"
	"    vec2 uv = v_uv;\n"
	"    if (u_warp != 0) {\n"
	// underwater warble: shrink slightly so the sine offsets never
	// sample outside the frame
	"        uv = uv * 0.94 + 0.03;\n"
	"        uv.x += sin(v_uv.y * 14.0 + u_time * 1.6) * 0.008;\n"
	"        uv.y += sin(v_uv.x * 14.0 + u_time * 1.6) * 0.008;\n"
	"    }\n"
	"    vec3 c = texture(u_scene, uv).rgb;\n"
	"    c += texture(u_bloomtex, uv).rgb * u_bloom;\n"
	"    frag = vec4(pow(c, vec3(u_gamma)), 1.0);\n"
	"}\n";

static const char *fragBright =
	"#version 330 core\n"
	"in vec2 v_uv;\n"
	"uniform sampler2D u_scene;\n"
	"out vec4 frag;\n"
	"void main() {\n"
	"    vec3 c = texture(u_scene, v_uv).rgb;\n"
	"    float lum = dot(c, vec3(0.299, 0.587, 0.114));\n"
	// soft threshold: keep only the genuinely bright bits (lava, lights,
	// muzzle flashes); the scene's HDR overbright feeds this naturally
	"    float k = max(lum - 0.72, 0.0);\n"
	"    frag = vec4(c * (k / (k + 0.35)), 1.0);\n"
	"}\n";

static const char *fragBlur =
	"#version 330 core\n"
	"in vec2 v_uv;\n"
	"uniform sampler2D u_scene;\n"
	"uniform vec2 u_dir;\n"
	"out vec4 frag;\n"
	"void main() {\n"	// 9-tap gaussian, bilinear-optimized offsets
	"    vec3 c = texture(u_scene, v_uv).rgb * 0.2270270270;\n"
	"    c += texture(u_scene, v_uv + u_dir * 1.3846153846).rgb * 0.3162162162;\n"
	"    c += texture(u_scene, v_uv - u_dir * 1.3846153846).rgb * 0.3162162162;\n"
	"    c += texture(u_scene, v_uv + u_dir * 3.2307692308).rgb * 0.0702702703;\n"
	"    c += texture(u_scene, v_uv - u_dir * 3.2307692308).rgb * 0.0702702703;\n"
	"    frag = vec4(c, 1.0);\n"
	"}\n";

// ------------------------------------------------------------------ FBOs

static void GL3_Post_DestroyTargets (void)
{
	if (fbo_ms)			{ glDeleteFramebuffers (1, &fbo_ms); fbo_ms = 0; }
	if (rb_ms_color)	{ glDeleteRenderbuffers (1, &rb_ms_color); rb_ms_color = 0; }
	if (rb_ms_depth)	{ glDeleteRenderbuffers (1, &rb_ms_depth); rb_ms_depth = 0; }
	if (fbo_scene)		{ glDeleteFramebuffers (1, &fbo_scene); fbo_scene = 0; }
	if (tex_scene)		{ glDeleteTextures (1, &tex_scene); tex_scene = 0; }
	if (rb_scene_depth)	{ glDeleteRenderbuffers (1, &rb_scene_depth); rb_scene_depth = 0; }
	if (fbo_bloom[0])	{ glDeleteFramebuffers (2, fbo_bloom); fbo_bloom[0] = fbo_bloom[1] = 0; }
	if (tex_bloom[0])	{ glDeleteTextures (2, tex_bloom); tex_bloom[0] = tex_bloom[1] = 0; }
	if (fbo_depthcopy)	{ glDeleteFramebuffers (1, &fbo_depthcopy); fbo_depthcopy = 0; }
	if (tex_depthcopy)	{ glDeleteTextures (1, &tex_depthcopy); tex_depthcopy = 0; }
	fb_width = fb_height = fb_samples = 0;
}

static void GL3_Post_CreateTargets (void)
{
	float	rscale;
	int		i, w, h, samples, maxsamples;

	rscale = gl_renderscale ? gl_renderscale->value : 1.0f;
	if (rscale < 0.25f) rscale = 0.25f;
	if (rscale > 2.0f)  rscale = 2.0f;

	w = (int)(gl3state.width * rscale + 0.5f);
	h = (int)(gl3state.height * rscale + 0.5f);
	if (w < 64)  w = 64;
	if (h < 64)  h = 64;

	samples = gl_msaa ? (int)gl_msaa->value : 0;
	glGetIntegerv (GL_MAX_SAMPLES, &maxsamples);
	if (samples > maxsamples) samples = maxsamples;
	if (samples < 2) samples = 0;

	if (w == fb_width && h == fb_height && samples == fb_samples && fbo_scene)
		return;		// current targets still fit

	GL3_Post_DestroyTargets ();

	fb_width = w;
	fb_height = h;
	fb_samples = samples;

	// resolved scene texture (also the direct target without MSAA).
	// RGBA16F keeps the overbright lightmap range for the bloom pass.
	glGenTextures (1, &tex_scene);
	glBindTexture (GL_TEXTURE_2D, tex_scene);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, NULL);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glGenFramebuffers (1, &fbo_scene);
	glBindFramebuffer (GL_FRAMEBUFFER, fbo_scene);
	glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex_scene, 0);

	if (samples)
	{
		glGenRenderbuffers (1, &rb_ms_color);
		glBindRenderbuffer (GL_RENDERBUFFER, rb_ms_color);
		glRenderbufferStorageMultisample (GL_RENDERBUFFER, samples, GL_RGBA16F, w, h);
		glGenRenderbuffers (1, &rb_ms_depth);
		glBindRenderbuffer (GL_RENDERBUFFER, rb_ms_depth);
		glRenderbufferStorageMultisample (GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8, w, h);

		glGenFramebuffers (1, &fbo_ms);
		glBindFramebuffer (GL_FRAMEBUFFER, fbo_ms);
		glFramebufferRenderbuffer (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb_ms_color);
		glFramebufferRenderbuffer (GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb_ms_depth);
	}
	else
	{
		glGenRenderbuffers (1, &rb_scene_depth);
		glBindRenderbuffer (GL_RENDERBUFFER, rb_scene_depth);
		glRenderbufferStorage (GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
		glBindFramebuffer (GL_FRAMEBUFFER, fbo_scene);
		glFramebufferRenderbuffer (GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb_scene_depth);
	}

	// bloom ping-pong at half scene resolution
	bloom_w = w / 2 > 1 ? w / 2 : 1;
	bloom_h = h / 2 > 1 ? h / 2 : 1;
	glGenTextures (2, tex_bloom);
	glGenFramebuffers (2, fbo_bloom);
	for (i = 0; i < 2; i++)
	{
		glBindTexture (GL_TEXTURE_2D, tex_bloom[i]);
		glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA16F, bloom_w, bloom_h, 0, GL_RGBA, GL_HALF_FLOAT, NULL);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glBindFramebuffer (GL_FRAMEBUFFER, fbo_bloom[i]);
		glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex_bloom[i], 0);
	}

	// depth copy target for soft particles
	glGenTextures (1, &tex_depthcopy);
	glBindTexture (GL_TEXTURE_2D, tex_depthcopy);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w, h, 0,
		GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glGenFramebuffers (1, &fbo_depthcopy);
	glBindFramebuffer (GL_FRAMEBUFFER, fbo_depthcopy);
	glFramebufferTexture2D (GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, tex_depthcopy, 0);
	glDrawBuffer (GL_NONE);
	glReadBuffer (GL_NONE);

	glBindFramebuffer (GL_FRAMEBUFFER, 0);
	glBindTexture (GL_TEXTURE_2D, 0);
	gl3state.currenttexture = -1;
}

// blit the current scene depth into a sampleable texture and re-bind the
// scene target; returns the depth texture (used by soft particles)
GLuint GL3_Post_ResolveDepth (void)
{
	GLuint	src = fb_samples ? fbo_ms : fbo_scene;

	glBindFramebuffer (GL_READ_FRAMEBUFFER, src);
	glBindFramebuffer (GL_DRAW_FRAMEBUFFER, fbo_depthcopy);
	glBlitFramebuffer (0, 0, fb_width, fb_height, 0, 0, fb_width, fb_height,
		GL_DEPTH_BUFFER_BIT, GL_NEAREST);
	glBindFramebuffer (GL_FRAMEBUFFER, src);

	return tex_depthcopy;
}

// ------------------------------------------------------------------ frame

// scale factor from the client's virtual 2D coords to scene-FBO pixels
float GL3_Post_FrameScale (void)
{
	return (float)fb_width / (float)gl3state.vw;
}

int GL3_Post_Width (void)  { return fb_width; }
int GL3_Post_Height (void) { return fb_height; }

// bind the scene target and clear it; 3D rendering goes here
void GL3_Post_BeginScene (void)
{
	GL3_Post_CreateTargets ();		// applies gl_msaa/gl_renderscale changes

	glBindFramebuffer (GL_FRAMEBUFFER, fb_samples ? fbo_ms : fbo_scene);
	glViewport (0, 0, fb_width, fb_height);
	glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);	// eye mask is window-only
	glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
	glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	if (fb_samples)
		glEnable (GL_MULTISAMPLE);
}

// resolve, run bloom, and draw the post-processed scene onto the window
void GL3_Post_EndScene (void)
{
	qboolean	bloom = gl_bloom && gl_bloom->value > 0.0f;

	if (fb_samples)
	{
		glDisable (GL_MULTISAMPLE);
		glBindFramebuffer (GL_READ_FRAMEBUFFER, fbo_ms);
		glBindFramebuffer (GL_DRAW_FRAMEBUFFER, fbo_scene);
		glBlitFramebuffer (0, 0, fb_width, fb_height, 0, 0, fb_width, fb_height,
			GL_COLOR_BUFFER_BIT, GL_NEAREST);
	}

	glDisable (GL_DEPTH_TEST);
	glDisable (GL_BLEND);
	glDisable (GL_CULL_FACE);
	glBindVertexArray (post_vao);
	glActiveTexture (GL_TEXTURE0);

	if (bloom)
	{
		// bright-pass into bloom[0]
		glBindFramebuffer (GL_FRAMEBUFFER, fbo_bloom[0]);
		glViewport (0, 0, bloom_w, bloom_h);
		glUseProgram (prog_bright.program);
		glBindTexture (GL_TEXTURE_2D, tex_scene);
		glDrawArrays (GL_TRIANGLES, 0, 3);

		// separable gaussian: horizontal into [1], vertical back into [0]
		glUseProgram (prog_blur.program);
		glBindFramebuffer (GL_FRAMEBUFFER, fbo_bloom[1]);
		glUniform2f (prog_blur.u_dir, 1.0f / bloom_w, 0.0f);
		glBindTexture (GL_TEXTURE_2D, tex_bloom[0]);
		glDrawArrays (GL_TRIANGLES, 0, 3);

		glBindFramebuffer (GL_FRAMEBUFFER, fbo_bloom[0]);
		glUniform2f (prog_blur.u_dir, 0.0f, 1.0f / bloom_h);
		glBindTexture (GL_TEXTURE_2D, tex_bloom[1]);
		glDrawArrays (GL_TRIANGLES, 0, 3);
	}

	// final pass to the window
	glBindFramebuffer (GL_FRAMEBUFFER, 0);
	glViewport (0, 0, gl3state.width, gl3state.height);
	GL3_ApplyStereoMask ();		// anaglyph: this eye's channels only
	glUseProgram (prog_post.program);
	glUniform1f (prog_post.u_gamma,
		(vid_gamma && vid_gamma->value >= 0.5f) ? vid_gamma->value : 1.0f);
	glUniform1f (prog_post.u_time, r_newrefdef.time);
	glUniform1i (prog_post.u_warp, (r_newrefdef.rdflags & RDF_UNDERWATER) ? 1 : 0);
	glUniform1f (prog_post.u_bloom, bloom ? gl_bloom->value : 0.0f);

	glActiveTexture (GL_TEXTURE1);
	glBindTexture (GL_TEXTURE_2D, bloom ? tex_bloom[0] : tex_scene);	// unit 1: bloom (scene as dummy when off)
	glActiveTexture (GL_TEXTURE0);
	glBindTexture (GL_TEXTURE_2D, tex_scene);

	glDrawArrays (GL_TRIANGLES, 0, 3);

	glBindVertexArray (0);
	gl3state.currenttexture = -1;
}

// ------------------------------------------------------------------ init

void GL3_Post_Init (void)
{
	gl_msaa = ri.Cvar_Get ("gl_msaa", "4", CVAR_ARCHIVE);
	gl_renderscale = ri.Cvar_Get ("gl_renderscale", "1", CVAR_ARCHIVE);
	gl_bloom = ri.Cvar_Get ("gl_bloom", "0.4", CVAR_ARCHIVE);

	prog_post.program = GL3_CompileProgram (vtxPost, fragPost);
	prog_post.u_gamma = glGetUniformLocation (prog_post.program, "u_gamma");
	prog_post.u_time = glGetUniformLocation (prog_post.program, "u_time");
	prog_post.u_warp = glGetUniformLocation (prog_post.program, "u_warp");
	prog_post.u_bloom = glGetUniformLocation (prog_post.program, "u_bloom");
	glUseProgram (prog_post.program);
	glUniform1i (glGetUniformLocation (prog_post.program, "u_scene"), 0);
	glUniform1i (glGetUniformLocation (prog_post.program, "u_bloomtex"), 1);

	prog_bright.program = GL3_CompileProgram (vtxPost, fragBright);
	glUseProgram (prog_bright.program);
	glUniform1i (glGetUniformLocation (prog_bright.program, "u_scene"), 0);

	prog_blur.program = GL3_CompileProgram (vtxPost, fragBlur);
	prog_blur.u_dir = glGetUniformLocation (prog_blur.program, "u_dir");
	glUseProgram (prog_blur.program);
	glUniform1i (glGetUniformLocation (prog_blur.program, "u_scene"), 0);

	glGenVertexArrays (1, &post_vao);
}

void GL3_Post_Shutdown (void)
{
	GL3_Post_DestroyTargets ();
	if (post_vao)			{ glDeleteVertexArrays (1, &post_vao); post_vao = 0; }
	if (prog_post.program)	{ glDeleteProgram (prog_post.program); prog_post.program = 0; }
	if (prog_bright.program){ glDeleteProgram (prog_bright.program); prog_bright.program = 0; }
	if (prog_blur.program)	{ glDeleteProgram (prog_blur.program); prog_blur.program = 0; }
}
