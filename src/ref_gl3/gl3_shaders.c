// gl3_shaders.c -- GLSL program compilation for the GL 3.3 core renderer.

#include "gl3_local.h"

gl3prog2d_t	gl3_prog2d;

static GLuint GL3_Compile (GLenum type, const char *src)
{
	GLuint	sh = glCreateShader (type);
	GLint	ok = 0;

	glShaderSource (sh, 1, &src, NULL);
	glCompileShader (sh);
	glGetShaderiv (sh, GL_COMPILE_STATUS, &ok);
	if (!ok)
	{
		char	log[2048];
		glGetShaderInfoLog (sh, sizeof(log), NULL, log);
		ri.Sys_Error (ERR_FATAL, "GL3 shader compile failed:\n%s", log);
	}
	return sh;
}

GLuint GL3_CompileProgram (const char *vtx, const char *frag)
{
	GLuint	vs = GL3_Compile (GL_VERTEX_SHADER, vtx);
	GLuint	fs = GL3_Compile (GL_FRAGMENT_SHADER, frag);
	GLuint	prog = glCreateProgram ();
	GLint	ok = 0;

	glAttachShader (prog, vs);
	glAttachShader (prog, fs);
	// fixed attribute locations shared by all our vertex formats
	glBindAttribLocation (prog, 0, "a_pos");
	glBindAttribLocation (prog, 1, "a_uv");
	glBindAttribLocation (prog, 2, "a_lmuv");
	glBindAttribLocation (prog, 3, "a_color");
	glLinkProgram (prog);
	glGetProgramiv (prog, GL_LINK_STATUS, &ok);
	if (!ok)
	{
		char	log[2048];
		glGetProgramInfoLog (prog, sizeof(log), NULL, log);
		ri.Sys_Error (ERR_FATAL, "GL3 program link failed:\n%s", log);
	}
	glDeleteShader (vs);
	glDeleteShader (fs);
	return prog;
}

// ---- 2D textured/colored quads ----
static const char *vtx2d =
	"#version 330 core\n"
	"in vec2 a_pos;\n"
	"in vec2 a_uv;\n"
	"uniform mat4 u_ortho;\n"
	"out vec2 v_uv;\n"
	"void main() {\n"
	"    v_uv = a_uv;\n"
	"    gl_Position = u_ortho * vec4(a_pos, 0.0, 1.0);\n"
	"}\n";

static const char *frag2d =
	"#version 330 core\n"
	"in vec2 v_uv;\n"
	"uniform sampler2D u_tex;\n"
	"uniform vec4 u_color;\n"
	"uniform float u_gamma;\n"
	"uniform float u_intensity;\n"
	"out vec4 frag;\n"
	"void main() {\n"
	"    vec4 c = texture(u_tex, v_uv) * u_color;\n"
	// vanilla gamma-table semantics: pow(c, gamma), gamma < 1 brightens
	"    c.rgb = pow(c.rgb * u_intensity, vec3(u_gamma));\n"
	"    frag = c;\n"
	"}\n";

// ---- 3D world (diffuse * lightmap) ----
static const char *vtx3d =
	"#version 330 core\n"
	"in vec3 a_pos;\n"
	"in vec2 a_uv;\n"
	"in vec2 a_lmuv;\n"
	"uniform mat4 u_mvp;\n"
	"uniform float u_scroll;\n"		// SURF_FLOWING
	"out vec2 v_uv;\n"
	"out vec2 v_lmuv;\n"
	"void main() {\n"
	"    v_uv = a_uv + vec2(u_scroll, 0.0);\n"
	"    v_lmuv = a_lmuv;\n"
	"    gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
	"}\n";

static const char *frag3d =
	"#version 330 core\n"
	"in vec2 v_uv;\n"
	"in vec2 v_lmuv;\n"
	"uniform sampler2D u_tex;\n"
	"uniform sampler2D u_lightmap;\n"
	"uniform int u_lm_enabled;\n"
	"uniform float u_gamma;\n"
	"uniform float u_intensity;\n"
	"uniform float u_alpha;\n"
	"out vec4 frag;\n"
	"void main() {\n"
	"    vec4 diff = texture(u_tex, v_uv);\n"
	"    vec3 c = diff.rgb * u_intensity;\n"
	"    if (u_lm_enabled != 0)\n"
	"        c *= texture(u_lightmap, v_lmuv).rgb * 2.0;\n"	// overbright
	"    c = pow(c, vec3(u_gamma));\n"
	"    frag = vec4(c, diff.a * u_alpha);\n"
	"}\n";

gl3prog3d_t	gl3_prog3d;

// ---- alias models (MD2): texture * per-vertex light ----
static const char *vtxAlias =
	"#version 330 core\n"
	"in vec3 a_pos;\n"
	"in vec2 a_uv;\n"
	"in vec4 a_color;\n"
	"uniform mat4 u_mvp;\n"
	"out vec2 v_uv;\n"
	"out vec4 v_color;\n"
	"void main() {\n"
	"    v_uv = a_uv;\n"
	"    v_color = a_color;\n"
	"    gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
	"}\n";

static const char *fragAlias =
	"#version 330 core\n"
	"in vec2 v_uv;\n"
	"in vec4 v_color;\n"
	"uniform sampler2D u_tex;\n"
	"uniform float u_gamma;\n"
	"uniform float u_intensity;\n"
	"uniform float u_alphacut;\n"	// alpha-test replacement (0 = off); sprites use 0.666
	"out vec4 frag;\n"
	"void main() {\n"
	"    vec4 t = texture(u_tex, v_uv);\n"
	"    if (t.a * v_color.a < u_alphacut) discard;\n"
	"    vec3 c = t.rgb * v_color.rgb * u_intensity;\n"
	"    c = pow(c, vec3(u_gamma));\n"
	"    frag = vec4(c, t.a * v_color.a);\n"
	"}\n";

gl3progalias_t	gl3_prog_alias;

// ---- water / turb surfaces: raw texcoords warped per-fragment by sin(time) ----
static const char *vtxWarp =
	"#version 330 core\n"
	"in vec3 a_pos;\n"
	"in vec2 a_uv;\n"				// RAW (undivided) surface texcoords
	"uniform mat4 u_mvp;\n"
	"uniform float u_scroll;\n"		// SURF_FLOWING, raw units
	"out vec2 v_uv;\n"
	"void main() {\n"
	"    v_uv = a_uv + vec2(u_scroll * 64.0, 0.0);\n"
	"    gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
	"}\n";

static const char *fragWarp =
	"#version 330 core\n"
	"in vec2 v_uv;\n"
	"uniform sampler2D u_tex;\n"
	"uniform float u_time;\n"
	"uniform float u_gamma;\n"
	"uniform float u_intensity;\n"
	"uniform float u_alpha;\n"		// translucent water (TRANS33/66)
	"out vec4 frag;\n"
	"void main() {\n"
	"    vec2 w;\n"
	"    w.x = v_uv.x + sin(v_uv.y * 0.125 + u_time) * 8.0;\n"
	"    w.y = v_uv.y + sin(v_uv.x * 0.125 + u_time) * 8.0;\n"
	"    w /= 64.0;\n"
	"    vec4 t = texture(u_tex, w);\n"
	"    vec3 c = pow(t.rgb * u_intensity, vec3(u_gamma));\n"
	"    frag = vec4(c, t.a * u_alpha);\n"
	"}\n";

gl3progwarp_t	gl3_prog_warp;

// ---- particles (round point sprites, size scaled by distance) ----
static const char *vtxPart =
	"#version 330 core\n"
	"in vec3 a_pos;\n"
	"in vec4 a_color;\n"
	"uniform mat4 u_mvp;\n"
	"uniform float u_psize;\n"
	"out vec4 v_color;\n"
	"void main() {\n"
	"    v_color = a_color;\n"
	"    gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
	"    gl_PointSize = clamp(u_psize / gl_Position.w, 2.0, 40.0);\n"	// id min/max point sizes
	"}\n";

static const char *fragPart =
	"#version 330 core\n"
	"in vec4 v_color;\n"
	"uniform float u_gamma;\n"
	"out vec4 frag;\n"
	"void main() {\n"
	// square untextured points, like id's GL_EXT_point_parameters path.
	// (no gl_PointCoord round-mask: it reads as (0,0) on some drivers here,
	// which used to discard every particle fragment)
	"    vec3 c = pow(v_color.rgb, vec3(u_gamma));\n"
	"    frag = vec4(c, v_color.a);\n"
	"}\n";

gl3progpart_t	gl3_prog_part;

void GL3_InitShaders (void)
{
	gl3_prog2d.program = GL3_CompileProgram (vtx2d, frag2d);
	gl3_prog2d.u_ortho = glGetUniformLocation (gl3_prog2d.program, "u_ortho");
	gl3_prog2d.u_color = glGetUniformLocation (gl3_prog2d.program, "u_color");
	gl3_prog2d.u_gamma = glGetUniformLocation (gl3_prog2d.program, "u_gamma");
	gl3_prog2d.u_intensity = glGetUniformLocation (gl3_prog2d.program, "u_intensity");

	glUseProgram (gl3_prog2d.program);
	glUniform1i (glGetUniformLocation (gl3_prog2d.program, "u_tex"), 0);	// sampler on unit 0

	gl3_prog3d.program = GL3_CompileProgram (vtx3d, frag3d);
	gl3_prog3d.u_mvp = glGetUniformLocation (gl3_prog3d.program, "u_mvp");
	gl3_prog3d.u_gamma = glGetUniformLocation (gl3_prog3d.program, "u_gamma");
	gl3_prog3d.u_intensity = glGetUniformLocation (gl3_prog3d.program, "u_intensity");
	gl3_prog3d.u_lm_enabled = glGetUniformLocation (gl3_prog3d.program, "u_lm_enabled");
	gl3_prog3d.u_alpha = glGetUniformLocation (gl3_prog3d.program, "u_alpha");
	gl3_prog3d.u_scroll = glGetUniformLocation (gl3_prog3d.program, "u_scroll");
	glUseProgram (gl3_prog3d.program);
	glUniform1f (gl3_prog3d.u_alpha, 1.0f);
	glUniform1f (gl3_prog3d.u_scroll, 0.0f);
	glUniform1i (glGetUniformLocation (gl3_prog3d.program, "u_tex"), 0);		// diffuse on unit 0
	glUniform1i (glGetUniformLocation (gl3_prog3d.program, "u_lightmap"), 1);	// lightmap on unit 1

	gl3_prog_alias.program = GL3_CompileProgram (vtxAlias, fragAlias);
	gl3_prog_alias.u_mvp = glGetUniformLocation (gl3_prog_alias.program, "u_mvp");
	gl3_prog_alias.u_gamma = glGetUniformLocation (gl3_prog_alias.program, "u_gamma");
	gl3_prog_alias.u_intensity = glGetUniformLocation (gl3_prog_alias.program, "u_intensity");
	gl3_prog_alias.u_alphacut = glGetUniformLocation (gl3_prog_alias.program, "u_alphacut");
	glUseProgram (gl3_prog_alias.program);
	glUniform1f (gl3_prog_alias.u_alphacut, 0.0f);
	glUniform1i (glGetUniformLocation (gl3_prog_alias.program, "u_tex"), 0);

	gl3_prog_part.program = GL3_CompileProgram (vtxPart, fragPart);
	gl3_prog_part.u_mvp = glGetUniformLocation (gl3_prog_part.program, "u_mvp");
	gl3_prog_part.u_gamma = glGetUniformLocation (gl3_prog_part.program, "u_gamma");
	gl3_prog_part.u_psize = glGetUniformLocation (gl3_prog_part.program, "u_psize");

	gl3_prog_warp.program = GL3_CompileProgram (vtxWarp, fragWarp);
	gl3_prog_warp.u_mvp = glGetUniformLocation (gl3_prog_warp.program, "u_mvp");
	gl3_prog_warp.u_time = glGetUniformLocation (gl3_prog_warp.program, "u_time");
	gl3_prog_warp.u_gamma = glGetUniformLocation (gl3_prog_warp.program, "u_gamma");
	gl3_prog_warp.u_intensity = glGetUniformLocation (gl3_prog_warp.program, "u_intensity");
	gl3_prog_warp.u_alpha = glGetUniformLocation (gl3_prog_warp.program, "u_alpha");
	gl3_prog_warp.u_scroll = glGetUniformLocation (gl3_prog_warp.program, "u_scroll");
	glUseProgram (gl3_prog_warp.program);
	glUniform1f (gl3_prog_warp.u_alpha, 1.0f);
	glUniform1f (gl3_prog_warp.u_scroll, 0.0f);
	glUniform1i (glGetUniformLocation (gl3_prog_warp.program, "u_tex"), 0);
}

void GL3_ShutdownShaders (void)
{
	if (gl3_prog2d.program)
		glDeleteProgram (gl3_prog2d.program);
	if (gl3_prog3d.program)
		glDeleteProgram (gl3_prog3d.program);
	if (gl3_prog_alias.program)
		glDeleteProgram (gl3_prog_alias.program);
	if (gl3_prog_part.program)
		glDeleteProgram (gl3_prog_part.program);
	if (gl3_prog_warp.program)
		glDeleteProgram (gl3_prog_warp.program);
	memset (&gl3_prog2d, 0, sizeof(gl3_prog2d));
	memset (&gl3_prog3d, 0, sizeof(gl3_prog3d));
	memset (&gl3_prog_alias, 0, sizeof(gl3_prog_alias));
	memset (&gl3_prog_part, 0, sizeof(gl3_prog_part));
	memset (&gl3_prog_warp, 0, sizeof(gl3_prog_warp));
}
