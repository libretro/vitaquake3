/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
/*
** QGL.H
*/

#ifndef __QGL_H__
#define __QGL_H__

/* indices[] is the array passed to qglDrawElements() by vglDrawObjects(). The
 * renderer batches up to tess.numIndexes entries, bounded by SHADER_MAX_INDEXES
 * (6 * SHADER_MAX_VERTEXES = 6000, see qcommon/qfiles.h). This was 4096, which
 * caused an out-of-bounds read past the indices[] allocation (and garbage
 * geometry) on any surface / fog / dlight batch exceeding 4096 indices. */
#define MAX_INDICES 6000
#define VERTEXARRAYSIZE 18360
extern float *gVertexBuffer;
extern uint8_t *gColorBuffer;
extern uint8_t *gColorBuffer255;
extern float *gTexCoordBuffer;
extern uint16_t *indices;

#ifdef __PSP2__

#include "vitaGL.h"

#define APIENTRY
#define APIENTRYP APIENTRY *
#define GL_RGB8 GL_RGB
#define GL_RGBA8 GL_RGBA
#define GL_UNSIGNED_INT GL_UNSIGNED_BYTE

#define qglGenTextures glGenTextures
#define qglGetIntegerv glGetIntegerv
#define qglGetError glGetError
#define qglClearDepth glClearDepth
#define qglCullFace glCullFace
#define qglColor4f glColor4f
#define qglActiveTextureARB glActiveTextureARB
#define qglDisable glDisable
#define qglEnable glEnable
#define qglDepthFunc glDepthFunc
#define qglEnableClientState glEnableClientState
#define qglPolygonMode glPolygonMode
#define qglDepthMask glDepthMask

#define qglLockArraysEXT glLockArraysEXT
#define qglActiveTextureARB glActiveTextureARB
#define qglColor3f glColor3f
#define qglVertex3fv glVertex3fv
#define qglDepthRange glDepthRange
#define qglColor4ubv glColor4ubv
#define qglMultiTexCoord2fARB glMultiTexCoord2fARB
#define qglTexCoord2fv glTexCoord2fv
#define qglVertex3fv glVertex3fv
#define qglDisableClientState glDisableClientState

#define qglBindTexture glBindTexture
#define qglTexEnvf glTexEnvf
#define qglTexEnvi glTexEnvi
#define qglBlendFunc glBlendFunc
#define qglAlphaFunc glAlphaFunc
#define qglClearColor glClearColor
#define qglClear glClear
#define qglMatrixMode glMatrixMode
#define qglLoadMatrixf glLoadMatrixf
#define qglViewport glViewport
#define qglScissor glScissor
#define qglFinish glFinish
#define qglLoadIdentity glLoadIdentity
#define qglOrtho glOrtho
#define qglTexImage2D glTexImage2D
#define qglTexParameterf glTexParameterf
#define qglTexParameteri glTexParameteri
#define qglTexSubImage2D glTexSubImage2D
#define qglBegin glBegin
#define qglTexCoord2f glTexCoord2f
#define qglVertex2f glVertex2f
#define qglEnd glEnd
#define qglStencilMask glStencilMask
#define qglClearStencil glClearStencil
#define qglStencilFunc glStencilFunc
#define qglStencilOp glStencilOp

//#define qglDrawBuffer glDrawBuffer
#define qglColorMask glColorMask

#define qglPopMatrix glPopMatrix
#define qglPushMatrix glPushMatrix
#define qglDeleteTextures glDeleteTextures
#define qglDrawElements glDrawElements
#define qglVertexPointer glVertexPointer
#define qglColorPointer glColorPointer
#define qglTexCoordPointer glTexCoordPointer
#define qglPolygonOffset glPolygonOffset
#define qglArrayElement glArrayElement
#define qglTranslatef glTranslatef
#define qglVertex3f glVertex3f

#else

#ifdef SWITCH
#include <glsym/switch/nx_gl.h>
#elif defined(RGLGEN_DECL_H__)
/* glsym already included - skip GL headers to avoid macro conflicts */
#elif defined(__LIBRETRO__)
#include <glsym/rglgen_private_headers.h>
#else
#include <GL/gl.h>
#endif

#ifndef APIENTRY
#define APIENTRY
#endif

#if defined(__LIBRETRO__) && defined(__MINGW32__) && defined(__i386__)
#  undef APIENTRY
#  define APIENTRY
#endif

extern void vglVertexAttribPointerMapped(int id, void* ptr);

extern  void ( APIENTRY * qglBlendFunc )(GLenum sfactor, GLenum dfactor);
extern  void ( APIENTRY * qglTexImage2D )(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const GLvoid *pixels);
extern  void ( APIENTRY * qglTexParameteri )(GLenum target, GLenum pname, GLint param);
extern  void ( APIENTRY * qglBindFramebuffer )(GLenum target, GLuint framebuffer);
extern  void ( APIENTRY * qglGenerateMipmap )(GLenum target);
extern  void ( APIENTRY * qglTexSubImage2D )(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid *pixels);
extern  void ( APIENTRY * qglDepthMask )(GLboolean flag);
extern  void ( APIENTRY * qglPushMatrix )(void);
extern  void ( APIENTRY * qglRotatef )(GLfloat angle, GLfloat x, GLfloat y, GLfloat z);
extern  void ( APIENTRY * qglTranslatef )(GLfloat x, GLfloat y, GLfloat z);
extern  void ( APIENTRY * qglDepthRange )(GLclampd zNear, GLclampd zFar);
extern  void ( APIENTRY * qglClear )(GLbitfield mask);
extern  void ( APIENTRY * qglEnable )(GLenum cap);
extern  void ( APIENTRY * qglDisable )(GLenum cap);
extern  void ( APIENTRY * qglPopMatrix )(void);
extern  void ( APIENTRY * qglGetFloatv )(GLenum pname, GLfloat *params);
extern  void ( APIENTRY * qglOrtho )(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble zNear, GLdouble zFar);
extern  void ( APIENTRY * qglFrustum )(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble zNear, GLdouble zFar);
extern  void ( APIENTRY * qglLoadMatrixf )(const GLfloat *m);
extern  void ( APIENTRY * qglLoadIdentity )(void);
extern  void ( APIENTRY * qglMatrixMode )(GLenum mode);
extern  void ( APIENTRY * qglBindTexture )(GLenum target, GLuint texture);
extern  void ( APIENTRY * qglReadPixels )(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid *pixels);
extern  void ( APIENTRY * qglPolygonMode )(GLenum face, GLenum mode);
extern  void ( APIENTRY * qglVertexPointer )(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer);
extern  void ( APIENTRY * qglColorPointer )(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer);
extern  void ( APIENTRY * qglTexCoordPointer )(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer);
extern  void ( APIENTRY * qglDrawElements )(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices);
extern  void ( APIENTRY * qglClearColor )(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha);
extern  void ( APIENTRY * qglCullFace )(GLenum mode);
extern  void ( APIENTRY * qglViewport )(GLint x, GLint y, GLsizei width, GLsizei height);
extern  void ( APIENTRY * qglDeleteTextures )(GLsizei n, const GLuint *textures);
extern  void ( APIENTRY * qglClearStencil )(GLint s);
extern  void ( APIENTRY * qglColor4f )(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
extern  void ( APIENTRY * qglScissor )(GLint x, GLint y, GLsizei width, GLsizei height);
extern  void ( APIENTRY * qglEnableClientState )(GLenum array);
extern  void ( APIENTRY * qglDisableClientState )(GLenum array);
extern  void ( APIENTRY * qglStencilFunc )(GLenum func, GLint ref, GLuint mask);
extern  void ( APIENTRY * qglStencilOp )(GLenum fail, GLenum zfail, GLenum zpass);
extern  void ( APIENTRY * qglScalef )(GLfloat x, GLfloat y, GLfloat z);
extern  void ( APIENTRY * qglDepthFunc )(GLenum func);
extern  void ( APIENTRY * qglTexEnvi )(GLenum target, GLenum pname, GLint param);
extern  void ( APIENTRY * qglAlphaFunc )(GLenum func,  GLclampf ref);
extern  void ( APIENTRY * qglClearDepth )(GLdouble depth);
extern  void ( APIENTRY * qglFinish )(void);
extern  void ( APIENTRY * qglGenTextures )(GLsizei n,GLuint * textures);
extern  void ( APIENTRY * qglPolygonOffset )(GLfloat factor, GLfloat units);
extern  void ( APIENTRY * qglClipPlane )(GLenum plane, const GLdouble * equation);
extern  void ( APIENTRY * qglColorMask )(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha);
extern  void ( APIENTRY * qglLineWidth )(GLfloat width);
extern  void ( APIENTRY * qglStencilMask )(GLuint mask);

#endif // PSP2

/* GL constants that may be missing when using glsym without raw GL headers */
#ifndef GL_VERTEX_ARRAY
#define GL_VERTEX_ARRAY 0x8074
#endif
#ifndef GL_TEXTURE_COORD_ARRAY
#define GL_TEXTURE_COORD_ARRAY 0x8078
#endif
#ifndef GL_COLOR_ARRAY
#define GL_COLOR_ARRAY 0x8076
#endif

/* vgl wrapper function declarations (implemented in libretro.c) */
extern void vglVertexPointerMapped(const GLvoid *pointer);
extern void vglTexCoordPointerMapped(const GLvoid *pointer);
extern void vglColorPointerMapped(GLenum type, const GLvoid *pointer);
extern void vglDrawObjects(GLenum mode, GLsizei count, GLboolean implicit_wvp);
extern void vglTexCoordPointer(GLint size, GLenum type, GLsizei stride, GLuint count, const GLvoid *pointer);
extern void vglVertexPointer(GLint size, GLenum type, GLsizei stride, GLuint count, const GLvoid *pointer);
extern void vglColorPointer(GLint size, GLenum type, GLsizei stride, GLuint count, const GLvoid *pointer);

#endif
