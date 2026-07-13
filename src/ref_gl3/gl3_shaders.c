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
	"    c.rgb = pow(c.rgb * u_intensity, vec3(1.0 / u_gamma));\n"
	"    frag = c;\n"
	"}\n";

// ---- 3D world (diffuse * lightmap) ----
static const char *vtx3d =
	"#version 330 core\n"
	"in vec3 a_pos;\n"
	"in vec2 a_uv;\n"
	"in vec2 a_lmuv;\n"
	"uniform mat4 u_mvp;\n"
	"out vec2 v_uv;\n"
	"out vec2 v_lmuv;\n"
	"void main() {\n"
	"    v_uv = a_uv;\n"
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
	"    c = pow(c, vec3(1.0 / u_gamma));\n"
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
	"out vec4 frag;\n"
	"void main() {\n"
	"    vec4 t = texture(u_tex, v_uv);\n"
	"    vec3 c = t.rgb * v_color.rgb * u_intensity;\n"
	"    c = pow(c, vec3(1.0 / u_gamma));\n"
	"    frag = vec4(c, t.a * v_color.a);\n"
	"}\n";

gl3progalias_t	gl3_prog_alias;

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
	"    gl_PointSize = clamp(u_psize / gl_Position.w, 1.0, 64.0);\n"
	"}\n";

static const char *fragPart =
	"#version 330 core\n"
	"in vec4 v_color;\n"
	"uniform float u_gamma;\n"
	"out vec4 frag;\n"
	"void main() {\n"
	"    vec2 d = gl_PointCoord - vec2(0.5);\n"
	"    if (dot(d, d) > 0.25) discard;\n"			// round particle
	"    vec3 c = pow(v_color.rgb, vec3(1.0 / u_gamma));\n"
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
	glUseProgram (gl3_prog3d.program);
	glUniform1f (gl3_prog3d.u_alpha, 1.0f);
	glUniform1i (glGetUniformLocation (gl3_prog3d.program, "u_tex"), 0);		// diffuse on unit 0
	glUniform1i (glGetUniformLocation (gl3_prog3d.program, "u_lightmap"), 1);	// lightmap on unit 1

	gl3_prog_alias.program = GL3_CompileProgram (vtxAlias, fragAlias);
	gl3_prog_alias.u_mvp = glGetUniformLocation (gl3_prog_alias.program, "u_mvp");
	gl3_prog_alias.u_gamma = glGetUniformLocation (gl3_prog_alias.program, "u_gamma");
	gl3_prog_alias.u_intensity = glGetUniformLocation (gl3_prog_alias.program, "u_intensity");
	glUseProgram (gl3_prog_alias.program);
	glUniform1i (glGetUniformLocation (gl3_prog_alias.program, "u_tex"), 0);

	gl3_prog_part.program = GL3_CompileProgram (vtxPart, fragPart);
	gl3_prog_part.u_mvp = glGetUniformLocation (gl3_prog_part.program, "u_mvp");
	gl3_prog_part.u_gamma = glGetUniformLocation (gl3_prog_part.program, "u_gamma");
	gl3_prog_part.u_psize = glGetUniformLocation (gl3_prog_part.program, "u_psize");
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
	memset (&gl3_prog2d, 0, sizeof(gl3_prog2d));
	memset (&gl3_prog3d, 0, sizeof(gl3_prog3d));
	memset (&gl3_prog_alias, 0, sizeof(gl3_prog_alias));
	memset (&gl3_prog_part, 0, sizeof(gl3_prog_part));
}
