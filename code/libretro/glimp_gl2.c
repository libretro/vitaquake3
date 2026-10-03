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
 * The platform side of ioquake3's OpenGL 2 renderer (code/renderergl2), for
 * the libretro core: what code/sdl/sdl_glimp.c does upstream, minus the
 * window. The frontend owns the context - desktop OpenGL or OpenGL ES 2/3 -
 * and hands out its functions through get_proc_address, so this only loads
 * them, reads the version and fills in glConfig.
 *
 * Built instead of the OpenGL 1 renderer (code/renderergl1, whose platform
 * side is in libretro.c) when RENDERER_GL2 is defined, which is what makes
 * the core run on OpenGL ES: renderergl1 is fixed-function OpenGL 1.x and
 * OpenGL ES 2 has none of it.
 */

#ifdef RENDERER_GL2

#include "../renderercommon_gl2/tr_common.h"
#include "libretro.h"

extern struct retro_hw_render_callback hw_render;
extern int scr_width, scr_height;
extern int framerate;

int qglMajorVersion, qglMinorVersion;
int qglesMajorVersion, qglesMinorVersion;

void (APIENTRYP qglActiveTextureARB) (GLenum texture);
void (APIENTRYP qglClientActiveTextureARB) (GLenum texture);
void (APIENTRYP qglMultiTexCoord2fARB) (GLenum target, GLfloat s, GLfloat t);

void (APIENTRYP qglLockArraysEXT) (GLint first, GLsizei count);
void (APIENTRYP qglUnlockArraysEXT) (void);

#define GLE(ret, name, ...) name##proc * qgl##name = NULL;
QGL_1_1_PROCS;
QGL_1_1_FIXED_FUNCTION_PROCS;
QGL_DESKTOP_1_1_PROCS;
QGL_DESKTOP_1_1_FIXED_FUNCTION_PROCS;
QGL_ES_1_1_PROCS;
QGL_ES_1_1_FIXED_FUNCTION_PROCS;
QGL_1_3_PROCS;
QGL_1_5_PROCS;
QGL_2_0_PROCS;
QGL_3_0_PROCS;
QGL_ARB_occlusion_query_PROCS;
QGL_ARB_framebuffer_object_PROCS;
QGL_ARB_vertex_array_object_PROCS;
QGL_EXT_direct_state_access_PROCS;
#undef GLE

void *GLimp_GetProcAddress( const char *name )
{
	return hw_render.get_proc_address ? (void *)hw_render.get_proc_address( name ) : NULL;
}

static void APIENTRY GLimp_GLES_ClearDepth( GLclampd depth ) {
	qglClearDepthf( depth );
}

static void APIENTRY GLimp_GLES_DepthRange( GLclampd near_val, GLclampd far_val ) {
	qglDepthRangef( near_val, far_val );
}

static void APIENTRY GLimp_GLES_DrawBuffer( GLenum mode ) {
	// unsupported
}

static void APIENTRY GLimp_GLES_PolygonMode( GLenum face, GLenum mode ) {
	// unsupported
}

static qboolean GLimp_GetProcAddresses( void )
{
	qboolean success = qtrue;
	const char *version;

#define GLE( ret, name, ... ) qgl##name = (name##proc *) GLimp_GetProcAddress("gl" #name); \
	if ( qgl##name == NULL ) { \
		ri.Printf( PRINT_ALL, "ERROR: Missing OpenGL function %s\n", "gl" #name ); \
		success = qfalse; \
	}

	GLE(const GLubyte *, GetString, GLenum name)

	if ( !qglGetString )
		ri.Error( ERR_FATAL, "glGetString is NULL" );

	version = (const char *)qglGetString( GL_VERSION );
	if ( !version )
		ri.Error( ERR_FATAL, "GL_VERSION is NULL" );

	if ( Q_stricmpn( "OpenGL ES", version, 9 ) == 0 ) {
		char profile[6]; // ES, ES-CM, or ES-CL
		sscanf( version, "OpenGL %5s %d.%d", profile, &qglesMajorVersion, &qglesMinorVersion );
		// common lite profile (no floating point) is not supported
		if ( Q_stricmp( profile, "ES-CL" ) == 0 ) {
			qglesMajorVersion = 0;
			qglesMinorVersion = 0;
		}
	} else {
		sscanf( version, "%d.%d", &qglMajorVersion, &qglMinorVersion );
	}

	if ( QGL_VERSION_ATLEAST( 2, 0 ) ) {
		QGL_1_1_PROCS;
		QGL_DESKTOP_1_1_PROCS;
		QGL_1_3_PROCS;
		QGL_1_5_PROCS;
		QGL_2_0_PROCS;
	} else if ( QGLES_VERSION_ATLEAST( 2, 0 ) ) {
		QGL_1_1_PROCS;
		QGL_ES_1_1_PROCS;
		QGL_1_3_PROCS;
		QGL_1_5_PROCS;
		QGL_2_0_PROCS;

		qglClearDepth = GLimp_GLES_ClearDepth;
		qglDepthRange = GLimp_GLES_DepthRange;
		qglDrawBuffer = GLimp_GLES_DrawBuffer;
		qglPolygonMode = GLimp_GLES_PolygonMode;
	} else {
		ri.Error( ERR_FATAL, "Unsupported OpenGL Version (%s), OpenGL 2.0 or OpenGL ES 2.0 is required", version );
	}

	if ( QGL_VERSION_ATLEAST( 3, 0 ) || QGLES_VERSION_ATLEAST( 3, 0 ) ) {
		QGL_3_0_PROCS;
	}

	// The renderer only loads the framebuffer functions where it uses its own
	// framebuffers (desktop OpenGL 3, tr_extensions.c), and on OpenGL ES draws
	// straight into the window. In a libretro core the window is the
	// frontend's framebuffer, which has to be bound, and these two are core in
	// OpenGL ES 2 and 3.
	if ( QGLES_VERSION_ATLEAST( 2, 0 ) ) {
		GLE(void, BindFramebuffer, GLenum target, GLuint framebuffer)
		GLE(void, BindRenderbuffer, GLenum target, GLuint renderbuffer)
	}

#undef GLE

	return success;
}

static qboolean GLimp_HaveExtension( const char *ext )
{
	const char *list = glConfig.extensions_string;
	const size_t len = strlen( ext );

	while ( ( list = strstr( list, ext ) ) != NULL ) {
		if ( ( list == glConfig.extensions_string || list[-1] == ' ' ) && ( list[len] == ' ' || list[len] == '\0' ) )
			return qtrue;
		list += len;
	}
	return qfalse;
}

// For tr_extensions.c, which upstream asks SDL
int GLimp_ExtensionSupported( const char *extension )
{
	return GLimp_HaveExtension( extension );
}

static void GLimp_InitExtensions( void )
{
	if ( !r_allowExtensions->integer ) {
		ri.Printf( PRINT_ALL, "* IGNORING OPENGL EXTENSIONS *\n" );
		return;
	}

	ri.Printf( PRINT_ALL, "Initializing OpenGL extensions\n" );

	glConfig.textureCompression = TC_NONE;

	// GL_EXT_texture_compression_s3tc
	if ( ( QGLES_VERSION_ATLEAST( 2, 0 ) || GLimp_HaveExtension( "GL_ARB_texture_compression" ) ) &&
	     GLimp_HaveExtension( "GL_EXT_texture_compression_s3tc" ) ) {
		if ( r_ext_compressed_textures->value ) {
			glConfig.textureCompression = TC_S3TC_ARB;
			ri.Printf( PRINT_ALL, "...using GL_EXT_texture_compression_s3tc\n" );
		} else {
			ri.Printf( PRINT_ALL, "...ignoring GL_EXT_texture_compression_s3tc\n" );
		}
	} else {
		ri.Printf( PRINT_ALL, "...GL_EXT_texture_compression_s3tc not found\n" );
	}
}

// Called from retro_run before each frame (see there). Nothing to bind with
// before the renderer has loaded its functions.
void GL_BindNullFramebuffers( void );
void GLimp_BindDefaultFramebuffer( void )
{
	if ( qglBindFramebuffer && qglBindRenderbuffer )
		GL_BindNullFramebuffers();
}

void GLimp_Init( qboolean fixedFunction )
{
	(void)fixedFunction; // renderergl2 never asks for the fixed-function pipeline

	ri.Printf( PRINT_ALL, "Initializing the frontend's OpenGL context\n" );

	glConfig.vidWidth = scr_width;
	glConfig.vidHeight = scr_height;
	glConfig.windowAspect = (float)scr_width / (float)scr_height;
	glConfig.colorBits = 32;
	glConfig.depthBits = 24;
	glConfig.stencilBits = 8;
	glConfig.displayFrequency = framerate;
	glConfig.stereoEnabled = qfalse;
	glConfig.isFullscreen = qtrue;
	glConfig.driverType = GLDRV_ICD;
	glConfig.hardwareType = GLHW_GENERIC;
	glConfig.deviceSupportsGamma = qfalse;
	glConfig.textureEnvAddAvailable = qfalse;

	if ( !GLimp_GetProcAddresses() )
		ri.Error( ERR_FATAL, "GLimp_Init() - the frontend's context lacks OpenGL functions the renderer needs" );

	Q_strncpyz( glConfig.vendor_string, (char *)qglGetString( GL_VENDOR ), sizeof( glConfig.vendor_string ) );
	Q_strncpyz( glConfig.renderer_string, (char *)qglGetString( GL_RENDERER ), sizeof( glConfig.renderer_string ) );
	if ( *glConfig.renderer_string && glConfig.renderer_string[strlen( glConfig.renderer_string ) - 1] == '\n' )
		glConfig.renderer_string[strlen( glConfig.renderer_string ) - 1] = 0;
	Q_strncpyz( glConfig.version_string, (char *)qglGetString( GL_VERSION ), sizeof( glConfig.version_string ) );

	// A core or ES 3 context has no GL_EXTENSIONS string: build the list.
	glConfig.extensions_string[0] = '\0';
	if ( qglGetStringi ) {
		int i, numExtensions = 0, listLength = 0;
		qglGetIntegerv( GL_NUM_EXTENSIONS, &numExtensions );
		for ( i = 0; i < numExtensions; i++ ) {
			const char *extension = (const char *)qglGetStringi( GL_EXTENSIONS, i );
			const int extensionLength = strlen( extension );
			if ( ( listLength + extensionLength + 1 ) >= sizeof( glConfig.extensions_string ) )
				break;
			if ( i > 0 ) {
				Q_strcat( glConfig.extensions_string, sizeof( glConfig.extensions_string ), " " );
				listLength++;
			}
			Q_strcat( glConfig.extensions_string, sizeof( glConfig.extensions_string ), extension );
			listLength += extensionLength;
		}
	} else {
		const char *extensions = (const char *)qglGetString( GL_EXTENSIONS );
		Q_strncpyz( glConfig.extensions_string, extensions ? extensions : "", sizeof( glConfig.extensions_string ) );
	}

	GLimp_InitExtensions();
}

#endif // RENDERER_GL2
