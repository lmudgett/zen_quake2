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
	glBindAttribLocation (prog, 2, "a_alpha");	// decals: no shader uses both
	glBindAttribLocation (prog, 3, "a_color");
	glBindAttribLocation (prog, 4, "a_ppos");	// per-instance particle center
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
	"out vec3 v_wpos;\n"			// surface-space position for per-pixel dlights
	"void main() {\n"
	"    v_uv = a_uv + vec2(u_scroll, 0.0);\n"
	"    v_lmuv = a_lmuv;\n"
	"    v_wpos = a_pos;\n"
	"    gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
	"}\n";

static const char *frag3d =
	"#version 330 core\n"
	"in vec2 v_uv;\n"
	"in vec2 v_lmuv;\n"
	"in vec3 v_wpos;\n"
	"uniform sampler2D u_tex;\n"
	"uniform sampler2D u_lightmap;\n"
	"uniform int u_lm_enabled;\n"
	"uniform float u_intensity;\n"
	"uniform float u_alpha;\n"
	"uniform int u_num_dlights;\n"		// gl_dynamic 2: per-pixel dynamic lights
	"uniform vec4 u_dlights[32];\n"		// xyz = origin (surface space), w = radius
	"uniform vec3 u_dlcolors[32];\n"
	"uniform sampler2D u_normalmap;\n"	// tangent-space normals (bump)
	"uniform int u_bump;\n"
	"uniform vec3 u_tbn_t;\n"			// per-surface texture axes + plane normal
	"uniform vec3 u_tbn_b;\n"
	"uniform vec3 u_tbn_n;\n"
	"uniform int u_lightmode;\n"		// dev: 1 = r_fullbright, 2 = gl_lightmap
	"uniform sampler2D u_scene;\n"		// opaque-scene grab (glass refraction)
	"uniform int u_refract;\n"			// 1 = composite refracted scene (blend off)
	"uniform vec2 u_rscale;\n"			// max refraction offset in scene-texture uv
	"out vec4 frag;\n"
	"void main() {\n"	// gamma applied once in the post pass, like id's ramp
	"    vec4 diff = texture(u_tex, v_uv);\n"
	"    vec3 c = diff.rgb * u_intensity;\n"
	"    if (u_lm_enabled != 0 && u_lightmode != 1) {\n"
	"        vec3 light = texture(u_lightmap, v_lmuv).rgb * 2.0;\n"	// overbright
	"        vec3 n = vec3(0.0);\n"
	"        if (u_bump != 0 && u_num_dlights > 0) {\n"
	"            vec3 nm = texture(u_normalmap, v_uv).rgb * 2.0 - 1.0;\n"
	"            n = normalize(nm.x * u_tbn_t + nm.y * u_tbn_b + nm.z * u_tbn_n);\n"
	"        }\n"
	"        for (int i = 0; i < u_num_dlights; i++) {\n"
	// smooth per-pixel falloff calibrated against id's lightmap formula
	// ((radius - cutoff64 - dist) per 255 with the x2 overbright)
	"            vec3 L = u_dlights[i].xyz - v_wpos;\n"
	"            float d = length(L);\n"
	"            float att = max(u_dlights[i].w - 64.0 - d, 0.0) * (2.0 / 255.0);\n"
	"            if (u_bump != 0)\n"
	"                att *= max(dot(n, L / max(d, 1.0)), 0.0) * 1.5 + 0.15;\n"	// keep a floor so grazing light still reads
	"            light += u_dlcolors[i] * att;\n"
	"        }\n"
	"        c *= light;\n"
	"        if (u_lightmode == 2) c = light * 0.5;\n"	// raw lightmap, no overbright
	"    }\n"
	"    if (u_refract != 0) {\n"
	// glass: bend the grabbed scene by the surface's tangent-space normal
	// map (auto-generated when no _n override exists); a flat normal falls
	// back to a straight composite, identical to plain alpha blending
	"        vec2 nm2 = texture(u_normalmap, v_uv).xy * 2.0 - 1.0;\n"
	"        vec2 suv = gl_FragCoord.xy / vec2(textureSize(u_scene, 0));\n"
	"        vec3 behind = texture(u_scene, suv + nm2 * u_rscale).rgb;\n"
	"        frag = vec4(mix(behind, c, u_alpha), 1.0);\n"
	"    } else\n"
	"        frag = vec4(c, diff.a * u_alpha);\n"
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
	"uniform float u_intensity;\n"
	"uniform float u_alphacut;\n"	// alpha-test replacement (0 = off); sprites use 0.666
	"out vec4 frag;\n"
	"void main() {\n"
	"    vec4 t = texture(u_tex, v_uv);\n"
	"    if (t.a * v_color.a < u_alphacut) discard;\n"
	// id boosts skins by intensity at UPLOAD, clamped to 255 -- clamp the
	// boosted texel before modulating by vertex light like fixed function did
	"    vec3 c = min(t.rgb * u_intensity, 1.0) * v_color.rgb;\n"
	"    frag = vec4(c, t.a * v_color.a);\n"
	"}\n";

gl3progalias_t	gl3_prog_alias;

// ---- water / turb surfaces: raw texcoords warped per-fragment by sin(time) ----
static const char *vtxWarp =
	"#version 330 core\n"
	"in vec3 a_pos;\n"
	"in vec2 a_uv;\n"				// RAW (undivided) surface texcoords
	"uniform mat4 u_mvp;\n"
	"out vec2 v_uv;\n"
	"void main() {\n"
	"    v_uv = a_uv;\n"
	"    gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
	"}\n";

static const char *fragWarp =
	"#version 330 core\n"
	"in vec2 v_uv;\n"
	"uniform sampler2D u_tex;\n"
	"uniform float u_time;\n"
	"uniform float u_intensity;\n"
	"uniform float u_alpha;\n"		// translucent water (TRANS33/66)
	"uniform float u_scroll;\n"		// SURF_FLOWING, raw units
	"uniform sampler2D u_scene;\n"	// opaque-scene grab (refraction)
	"uniform int u_refract;\n"		// 1 = composite refracted scene (blend off)
	"uniform vec2 u_rscale;\n"		// max refraction offset in scene-texture uv
	"out vec4 frag;\n"
	// id's per-vertex turbsin displacement (EmitWaterPolys)
	"vec2 warp_at(vec2 p) {\n"
	"    return vec2(sin(p.y * 0.125 + u_time), sin(p.x * 0.125 + u_time)) * 8.0;\n"
	"}\n"
	"void main() {\n"
	// id warps VERTICES of ~64-unit subdivided patches (GL_SubdivideSurface)
	// and lerps between them; the sine's ~50-unit period is under-sampled by
	// that grid, giving vanilla water its choppy patchwork slosh. Emulate it:
	// sample the displacement at the 64-unit cell corners and bilerp, instead
	// of evaluating the sine per fragment (which looks like smooth smears).
	"    vec2 c0 = floor(v_uv / 64.0) * 64.0;\n"
	"    vec2 fr = (v_uv - c0) / 64.0;\n"
	"    vec2 d = mix(mix(warp_at(c0),                    warp_at(c0 + vec2(64.0, 0.0)),  fr.x),\n"
	"                 mix(warp_at(c0 + vec2(0.0, 64.0)),  warp_at(c0 + vec2(64.0, 64.0)), fr.x), fr.y);\n"
	"    vec2 w = (v_uv + d + vec2(u_scroll, 0.0)) / 64.0;\n"
	"    vec4 t = texture(u_tex, w);\n"
	"    if (u_refract != 0) {\n"
	// the same choppy id displacement that warps the texture also bends the
	// grabbed scene: what's underwater visibly sloshes with the surface
	"        vec2 suv = gl_FragCoord.xy / vec2(textureSize(u_scene, 0));\n"
	"        vec3 behind = texture(u_scene, suv + (d / 8.0) * u_rscale).rgb;\n"
	"        frag = vec4(mix(behind, t.rgb * u_intensity, u_alpha), 1.0);\n"
	"    } else\n"
	"        frag = vec4(t.rgb * u_intensity, t.a * u_alpha);\n"
	"}\n";

gl3progwarp_t	gl3_prog_warp;

// ---- particles: instanced billboard quads, round + soft-depth faded ----
static const char *vtxPart =
	"#version 330 core\n"
	"in vec2 a_pos;\n"			// quad corner in [-1,1] (per vertex)
	"in vec3 a_ppos;\n"			// particle center (per instance)
	"in vec4 a_color;\n"		// particle color (per instance)
	"uniform mat4 u_mvp;\n"
	"uniform vec3 u_right;\n"
	"uniform vec3 u_up;\n"
	"uniform float u_size;\n"	// half-size in world units
	"out vec4 v_color;\n"
	"out vec2 v_corner;\n"
	"void main() {\n"
	"    vec3 w = a_ppos + (u_right * a_pos.x + u_up * a_pos.y) * u_size;\n"
	"    v_color = a_color;\n"
	"    v_corner = a_pos;\n"
	"    gl_Position = u_mvp * vec4(w, 1.0);\n"
	"}\n";

static const char *fragPart =
	"#version 330 core\n"
	"in vec4 v_color;\n"
	"in vec2 v_corner;\n"
	"uniform sampler2D u_depth;\n"		// resolved scene depth (soft particles)
	"uniform vec2 u_invdepthsize;\n"
	"uniform float u_soft;\n"			// 1 / fade range in world units; 0 = off
	"out vec4 frag;\n"
	"void main() {\n"
	"    float a = v_color.a * (1.0 - smoothstep(0.55, 1.0, length(v_corner)));\n"
	"    if (u_soft > 0.0) {\n"
	// fade where the particle nears solid geometry (near 4 / far 4096)
	"        float zs = texture(u_depth, gl_FragCoord.xy * u_invdepthsize).r;\n"
	"        float lin_s = 32768.0 / (4100.0 - (zs * 2.0 - 1.0) * 4092.0);\n"
	"        float lin_f = 32768.0 / (4100.0 - (gl_FragCoord.z * 2.0 - 1.0) * 4092.0);\n"
	"        a *= clamp((lin_s - lin_f) * u_soft, 0.0, 1.0);\n"
	"    }\n"
	"    if (a <= 0.004) discard;\n"
	"    frag = vec4(v_color.rgb, a);\n"
	"}\n";

gl3progpart_t	gl3_prog_part;

// ---- voxel mode: textured cube faces with a baked directional face shade ----
static const char *vtxVoxel =
	"#version 330 core\n"
	"in vec3 a_pos;\n"			// face-vertex world (or model) position
	"in vec2 a_uv;\n"			// texcoord projected from the source surface
	"in float a_color;\n"		// baked per-face shade (axis-dependent)
	"uniform mat4 u_mvp;\n"		// projection * view
	"uniform mat4 u_model;\n"	// identity for the world, entity matrix for models
	"out vec2 v_uv;\n"
	"out float v_shade;\n"
	"void main() {\n"
	"    v_uv = a_uv;\n"
	"    v_shade = a_color;\n"
	"    gl_Position = u_mvp * u_model * vec4(a_pos, 1.0);\n"
	"}\n";

static const char *fragVoxel =
	"#version 330 core\n"
	"in vec2 v_uv;\n"
	"in float v_shade;\n"
	"uniform sampler2D u_tex;\n"
	"uniform float u_intensity;\n"
	"uniform vec3 u_entcolor;\n"		// per-entity light tint; (1,1,1) for the world
	"out vec4 frag;\n"
	"void main() {\n"
	"    vec3 c = texture(u_tex, v_uv).rgb * v_shade * u_intensity * u_entcolor;\n"
	"    frag = vec4(c, 1.0);\n"		// opaque, writes depth (gamma applied in post)
	"}\n";

gl3progvoxel_t	gl3_prog_voxel;

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
	gl3_prog3d.u_num_dlights = glGetUniformLocation (gl3_prog3d.program, "u_num_dlights");
	gl3_prog3d.u_dlights = glGetUniformLocation (gl3_prog3d.program, "u_dlights");
	gl3_prog3d.u_dlcolors = glGetUniformLocation (gl3_prog3d.program, "u_dlcolors");
	gl3_prog3d.u_bump = glGetUniformLocation (gl3_prog3d.program, "u_bump");
	gl3_prog3d.u_tbn_t = glGetUniformLocation (gl3_prog3d.program, "u_tbn_t");
	gl3_prog3d.u_tbn_b = glGetUniformLocation (gl3_prog3d.program, "u_tbn_b");
	gl3_prog3d.u_tbn_n = glGetUniformLocation (gl3_prog3d.program, "u_tbn_n");
	gl3_prog3d.u_lightmode = glGetUniformLocation (gl3_prog3d.program, "u_lightmode");
	gl3_prog3d.u_refract = glGetUniformLocation (gl3_prog3d.program, "u_refract");
	gl3_prog3d.u_rscale = glGetUniformLocation (gl3_prog3d.program, "u_rscale");
	glUseProgram (gl3_prog3d.program);
	glUniform1i (gl3_prog3d.u_lightmode, 0);
	glUniform1f (gl3_prog3d.u_alpha, 1.0f);
	glUniform1f (gl3_prog3d.u_scroll, 0.0f);
	glUniform1i (gl3_prog3d.u_num_dlights, 0);
	glUniform1i (gl3_prog3d.u_bump, 0);
	glUniform1i (gl3_prog3d.u_refract, 0);
	glUniform1i (glGetUniformLocation (gl3_prog3d.program, "u_normalmap"), 2);	// unit 2
	glUniform1i (glGetUniformLocation (gl3_prog3d.program, "u_tex"), 0);		// diffuse on unit 0
	glUniform1i (glGetUniformLocation (gl3_prog3d.program, "u_lightmap"), 1);	// lightmap on unit 1
	glUniform1i (glGetUniformLocation (gl3_prog3d.program, "u_scene"), 3);		// refraction grab on unit 3

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
	gl3_prog_part.u_right = glGetUniformLocation (gl3_prog_part.program, "u_right");
	gl3_prog_part.u_up = glGetUniformLocation (gl3_prog_part.program, "u_up");
	gl3_prog_part.u_size = glGetUniformLocation (gl3_prog_part.program, "u_size");
	gl3_prog_part.u_soft = glGetUniformLocation (gl3_prog_part.program, "u_soft");
	gl3_prog_part.u_invdepthsize = glGetUniformLocation (gl3_prog_part.program, "u_invdepthsize");
	glUseProgram (gl3_prog_part.program);
	glUniform1i (glGetUniformLocation (gl3_prog_part.program, "u_depth"), 0);

	gl3_prog_warp.program = GL3_CompileProgram (vtxWarp, fragWarp);
	gl3_prog_warp.u_mvp = glGetUniformLocation (gl3_prog_warp.program, "u_mvp");
	gl3_prog_warp.u_time = glGetUniformLocation (gl3_prog_warp.program, "u_time");
	gl3_prog_warp.u_gamma = glGetUniformLocation (gl3_prog_warp.program, "u_gamma");
	gl3_prog_warp.u_intensity = glGetUniformLocation (gl3_prog_warp.program, "u_intensity");
	gl3_prog_warp.u_alpha = glGetUniformLocation (gl3_prog_warp.program, "u_alpha");
	gl3_prog_warp.u_scroll = glGetUniformLocation (gl3_prog_warp.program, "u_scroll");
	gl3_prog_warp.u_refract = glGetUniformLocation (gl3_prog_warp.program, "u_refract");
	gl3_prog_warp.u_rscale = glGetUniformLocation (gl3_prog_warp.program, "u_rscale");
	glUseProgram (gl3_prog_warp.program);
	glUniform1f (gl3_prog_warp.u_alpha, 1.0f);
	glUniform1f (gl3_prog_warp.u_scroll, 0.0f);
	glUniform1i (gl3_prog_warp.u_refract, 0);
	glUniform1i (glGetUniformLocation (gl3_prog_warp.program, "u_tex"), 0);
	glUniform1i (glGetUniformLocation (gl3_prog_warp.program, "u_scene"), 1);	// refraction grab on unit 1

	gl3_prog_voxel.program = GL3_CompileProgram (vtxVoxel, fragVoxel);
	gl3_prog_voxel.u_mvp = glGetUniformLocation (gl3_prog_voxel.program, "u_mvp");
	gl3_prog_voxel.u_model = glGetUniformLocation (gl3_prog_voxel.program, "u_model");
	gl3_prog_voxel.u_intensity = glGetUniformLocation (gl3_prog_voxel.program, "u_intensity");
	gl3_prog_voxel.u_entcolor = glGetUniformLocation (gl3_prog_voxel.program, "u_entcolor");
	glUseProgram (gl3_prog_voxel.program);
	glUniform1i (glGetUniformLocation (gl3_prog_voxel.program, "u_tex"), 0);
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
	if (gl3_prog_voxel.program)
		glDeleteProgram (gl3_prog_voxel.program);
	memset (&gl3_prog2d, 0, sizeof(gl3_prog2d));
	memset (&gl3_prog3d, 0, sizeof(gl3_prog3d));
	memset (&gl3_prog_alias, 0, sizeof(gl3_prog_alias));
	memset (&gl3_prog_part, 0, sizeof(gl3_prog_part));
	memset (&gl3_prog_warp, 0, sizeof(gl3_prog_warp));
	memset (&gl3_prog_voxel, 0, sizeof(gl3_prog_voxel));
}
