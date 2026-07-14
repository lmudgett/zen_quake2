// gl3_math.c -- 4x4 column-major matrix helpers (replacing the fixed-function
// glMatrix stack the old renderer used).

#include "gl3_local.h"
#include <math.h>

void GL3_MatIdentity (float *m)
{
	memset (m, 0, sizeof(float) * 16);
	m[0] = m[5] = m[10] = m[15] = 1.0f;
}

// out = a * b (column-major, so applying `out` = apply b then a)
void GL3_MatMul (float *out, const float *a, const float *b)
{
	float	t[16];
	int		i, j, k;

	for (i = 0; i < 4; i++)			// column
		for (j = 0; j < 4; j++)		// row
		{
			float sum = 0;
			for (k = 0; k < 4; k++)
				sum += a[k * 4 + j] * b[i * 4 + k];
			t[i * 4 + j] = sum;
		}
	memcpy (out, t, sizeof(t));
}

void GL3_MatPerspective (float *m, float fovy_deg, float aspect, float znear, float zfar)
{
	float	f = 1.0f / tanf (DEG2RADF (fovy_deg) * 0.5f);	// cot(fovy/2)

	memset (m, 0, sizeof(float) * 16);
	m[0]  = f / aspect;
	m[5]  = f;
	m[10] = (zfar + znear) / (znear - zfar);
	m[11] = -1.0f;
	m[14] = (2.0f * zfar * znear) / (znear - zfar);
}

void GL3_MatTranslate (float *m, float x, float y, float z)
{
	GL3_MatIdentity (m);
	m[12] = x;
	m[13] = y;
	m[14] = z;
}

void GL3_MatRotate (float *m, float deg, float x, float y, float z)
{
	float	rad = DEG2RADF (deg);
	float	c = cosf (rad), s = sinf (rad), t = 1.0f - c;
	float	len = sqrtf (x * x + y * y + z * z);

	GL3_MatIdentity (m);
	if (len == 0.0f)
		return;
	x /= len; y /= len; z /= len;

	// Rodrigues: R = c*I + s*[axis]x + (1-c)*axis*axis^T
	// (column-major storage; source lines below read as matrix rows)
	m[0] = t*x*x + c;	m[4] = t*x*y - s*z;	m[8]  = t*x*z + s*y;
	m[1] = t*x*y + s*z;	m[5] = t*y*y + c;	m[9]  = t*y*z - s*x;
	m[2] = t*x*z - s*y;	m[6] = t*y*z + s*x;	m[10] = t*z*z + c;
}
