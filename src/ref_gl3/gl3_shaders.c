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

void GL3_InitShaders (void)
{
	gl3_prog2d.program = GL3_CompileProgram (vtx2d, frag2d);
	gl3_prog2d.u_ortho = glGetUniformLocation (gl3_prog2d.program, "u_ortho");
	gl3_prog2d.u_color = glGetUniformLocation (gl3_prog2d.program, "u_color");
	gl3_prog2d.u_gamma = glGetUniformLocation (gl3_prog2d.program, "u_gamma");
	gl3_prog2d.u_intensity = glGetUniformLocation (gl3_prog2d.program, "u_intensity");

	glUseProgram (gl3_prog2d.program);
	glUniform1i (glGetUniformLocation (gl3_prog2d.program, "u_tex"), 0);	// sampler on unit 0
}

void GL3_ShutdownShaders (void)
{
	if (gl3_prog2d.program)
		glDeleteProgram (gl3_prog2d.program);
	memset (&gl3_prog2d, 0, sizeof(gl3_prog2d));
}
