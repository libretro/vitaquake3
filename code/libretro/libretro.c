#include "../libretro-common/include/libretro.h"
#include "../libretro-common/include/retro_dirent.h"
#include "../libretro-common/include/features/features_cpu.h"
#include "../libretro-common/include/file/file_path.h"
#ifndef RENDERER_GL2
#include "../libretro-common/include/glsym/glsym.h"
#endif
#include "../libretro-common/include/net/net_compat.h"
#include "../libretro-common/include/net/net_socket.h"
#include "../libretro-common/include/streams/file_stream.h"

#include "libretro_core_options.h"
#include "sys_local.h"
#include "sys_loadlib.h"
#include "../qcommon/q_shared.h"
#include "sys_local.h"
#ifdef RENDERER_GL2
#include "../renderercommon_gl2/tr_common.h"
#else
#include "../renderercommon/tr_common.h"
#endif
#include "../qcommon/qcommon.h"
#include "../client/client.h"
#include "../client/snd_local.h"

#ifndef RENDERER_GL2
#include <glsm/glsm.h>
#endif

#define SAMPLE_RATE_DEFAULT	48000
/* One-frame linear output buffer, in stereo sample-pairs. Sized for the
 * worst-case rate/framerate combination the "Sound Samplerate" option allows
 * (96 kHz at a low framerate) plus headroom; at 48 kHz / 60 fps the engine
 * writes 800 pairs here per frame and audio_callback hands them straight to the
 * frontend. No ring, no wrap. */
#define AUDIO_BUFFER_SIZE	16384

/* Output sample rate, resolved from the "Sound Samplerate (Hint)" core option
 * at startup (update_audio_samplerate) before SNDDMA_Init reads it. */
static int audio_sample_rate = SAMPLE_RATE_DEFAULT;

/* Backported from upstream libretro.h: lets "Auto" match the frontend's target
 * output rate. Guarded so a later header update won't conflict. */
#ifndef RETRO_ENVIRONMENT_GET_TARGET_SAMPLE_RATE
#define RETRO_ENVIRONMENT_GET_TARGET_SAMPLE_RATE (81 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#endif

int framerate = 165;
static int invert_y_axis = 1;
static int16_t audio_buffer[AUDIO_BUFFER_SIZE * 2];   /* interleaved L,R int16 */
static float   audio_buffer_f[AUDIO_BUFFER_SIZE * 2]; /* interleaved L,R float */
static unsigned audio_batch_frames_max = AUDIO_BUFFER_SIZE; /* backpressure cap, stereo frames */
static bool libretro_shared_context = false;
static bool widescreen_enabled      = false;

/* libretro timing model: the engine clock is driven entirely by a fixed
 * per-frame quantum (1000/framerate ms) advanced exactly once per retro_run.
 * There is no host wall clock, no timer, and no sleep anywhere in the frame
 * path. retro_run executes one video frame and its audio samples; a given
 * input sequence therefore produces identical output, which is what run-ahead,
 * netplay and deterministic record/replay require. The frame clock is published
 * to com_frameTime, the single engine clock that every consumer of time reads;
 * there is no Sys_Milliseconds and no host wall clock anywhere. */
static uint64_t frame_time_us        = 0;     /* accumulated virtual time, microseconds */
static unsigned frame_ms_remainder   = 0;     /* carry of the 1000 % framerate remainder */

/* One-shot init guards, defined later in this file. Forward-declared here so the
 * teardown path (retro_unload_game/retro_deinit) can reset them, which is what
 * allows the core to be loaded a second time in the same process (relevant for
 * statically linked console frontends and multi-instance hosts). */
extern bool     first_boot;
extern bool     initial_resolution_set;
extern qboolean snd_inited;
extern uint8_t  psp2_inited;
extern uint8_t  inited;

/* Shared teardown helper, defined alongside retro_unload_game. Forward-declared
 * so retro_deinit (which appears earlier in this file) can invoke it. */
static void core_teardown(void);
static bool core_initialized = false;

int scr_width = 960, scr_height = 544;

char *BASEGAME;

/* The OpenGL 1 renderer (code/renderergl1) gets its functions and its
 * immediate-mode batch buffers from here. The OpenGL 2 one (RENDERER_GL2)
 * loads its own, in glimp_gl2.c. */
#ifndef RENDERER_GL2
void ( APIENTRY * qglBlendFunc )(GLenum sfactor, GLenum dfactor);
void ( APIENTRY * qglTexImage2D )(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const GLvoid *pixels);
void ( APIENTRY * qglTexParameteri )(GLenum target, GLenum pname, GLint param);
void ( APIENTRY * qglBindFramebuffer )(GLenum target, GLuint framebuffer);
void ( APIENTRY * qglGenerateMipmap )(GLenum target);
void ( APIENTRY * qglTexSubImage2D )(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid *pixels);
void ( APIENTRY * qglDepthMask )(GLboolean flag);
void ( APIENTRY * qglPushMatrix )(void);
void ( APIENTRY * qglRotatef )(GLfloat angle, GLfloat x, GLfloat y, GLfloat z);
void ( APIENTRY * qglTranslatef )(GLfloat x, GLfloat y, GLfloat z);
void ( APIENTRY * qglDepthRange )(GLclampd zNear, GLclampd zFar);
void ( APIENTRY * qglClear )(GLbitfield mask);
void ( APIENTRY * qglEnable )(GLenum cap);
void ( APIENTRY * qglDisable )(GLenum cap);
void ( APIENTRY * qglPopMatrix )(void);
void ( APIENTRY * qglGetFloatv )(GLenum pname, GLfloat *params);
void ( APIENTRY * qglOrtho )(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble zNear, GLdouble zFar);
void ( APIENTRY * qglFrustum )(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble zNear, GLdouble zFar);
void ( APIENTRY * qglLoadMatrixf )(const GLfloat *m);
void ( APIENTRY * qglLoadIdentity )(void);
void ( APIENTRY * qglMatrixMode )(GLenum mode);
void ( APIENTRY * qglBindTexture )(GLenum target, GLuint texture);
void ( APIENTRY * qglReadPixels )(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid *pixels);
void ( APIENTRY * qglPolygonMode )(GLenum face, GLenum mode);
void ( APIENTRY * qglVertexPointer )(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer);
void ( APIENTRY * qglColorPointer )(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer);
void ( APIENTRY * qglTexCoordPointer )(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer);
void ( APIENTRY * qglDrawElements )(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices);
void ( APIENTRY * qglClearColor )(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha);
void ( APIENTRY * qglCullFace )(GLenum mode);
void ( APIENTRY * qglViewport )(GLint x, GLint y, GLsizei width, GLsizei height);
void ( APIENTRY * qglDeleteTextures )(GLsizei n, const GLuint *textures);
void ( APIENTRY * qglClearStencil )(GLint s);
void ( APIENTRY * qglColor4f )(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
void ( APIENTRY * qglVertex2f )(GLfloat x, GLfloat y);
void ( APIENTRY * qglScissor )(GLint x, GLint y, GLsizei width, GLsizei height);
void ( APIENTRY * qglEnableClientState )(GLenum array);
void ( APIENTRY * qglDisableClientState )(GLenum array);
void ( APIENTRY * qglStencilFunc )(GLenum func, GLint ref, GLuint mask);
void ( APIENTRY * qglStencilOp )(GLenum fail, GLenum zfail, GLenum zpass);
void ( APIENTRY * qglScalef )(GLfloat x, GLfloat y, GLfloat z);
void ( APIENTRY * qglDepthFunc )(GLenum func);
void ( APIENTRY * qglTexEnvi )(GLenum target, GLenum pname, GLint param);
void ( APIENTRY * qglAlphaFunc )(GLenum func,  GLclampf ref);
void ( APIENTRY * qglClearDepth )(GLdouble depth);
void ( APIENTRY * qglBegin )(GLenum mode);
void ( APIENTRY * qglEnd )(void);
void ( APIENTRY * qglFinish )(void);
void ( APIENTRY * qglGenTextures )(GLsizei n,GLuint * textures);
void ( APIENTRY * qglPolygonOffset )(GLfloat factor, GLfloat units);
void ( APIENTRY * qglClipPlane )(GLenum plane, const GLdouble * equation);
void ( APIENTRY * qglColorMask )(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha);
void ( APIENTRY * qglLineWidth )(GLfloat width);
void ( APIENTRY * qglStencilMask )(GLuint mask);

#define GL_FUNCS_NUM 52

/* indices[] (declared in qgl.h, allocated in GLimp_Init) is the index array
 * handed to qglDrawElements() by vglDrawObjects(). It must be at least
 * SHADER_MAX_INDEXES (6000) entries; see the MAX_INDICES fix in qgl.h. */
uint16_t* indices;
float *gVertexBuffer;
uint8_t *gColorBuffer;
uint8_t *gColorBuffer255;
float *gTexCoordBuffer;
float *gVertexBufferPtr;
uint8_t *gColorBufferPtr;
float *gTexCoordBufferPtr;

typedef struct api_entry{
	void *ptr;
} api_entry;

api_entry funcs[GL_FUNCS_NUM];
#endif


char g_rom_dir[1024], g_pak_path[1024], g_save_dir[1024];

extern struct retro_hw_render_callback hw_render;

#ifndef RENDERER_GL2
void vglVertexPointerMapped(const GLvoid *pointer) {
	qglVertexPointer(3, GL_FLOAT, 0, pointer);
}

void vglTexCoordPointerMapped(const GLvoid *pointer) {
	qglTexCoordPointer(2, GL_FLOAT, 0, pointer);
}

void vglColorPointerMapped(GLenum type, const GLvoid *pointer) {
	qglColorPointer(4, type, 0, pointer);
}

void vglDrawObjects(GLenum mode, GLsizei count, GLboolean implicit_wvp) {
	qglDrawElements(mode, count, GL_UNSIGNED_SHORT, indices);
}

void vglTexCoordPointer(GLint size, GLenum type, GLsizei stride, GLuint count, const GLvoid *pointer) {
	vglTexCoordPointerMapped(pointer);
}

void vglVertexPointer(GLint size, GLenum type, GLsizei stride, GLuint count, const GLvoid *pointer) {
	vglVertexPointerMapped(pointer);
}

void vglColorPointer(GLint size, GLenum type, GLsizei stride, GLuint count, const GLvoid *pointer) {
	vglColorPointerMapped(type, pointer);
}

#endif

static retro_log_printf_t log_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
/* Float batch output, negotiated per-game via
 * RETRO_ENVIRONMENT_GET_AUDIO_SAMPLE_BATCH_FLOAT. use_float_output stays 0 (and
 * audio_batch_cb_float NULL) on any frontend that doesn't support it, leaving
 * the int16 path unchanged. */
static retro_audio_sample_batch_float_t audio_batch_cb_float = NULL;
static int use_float_output = 0;
/* Engine-side float-output state (defined in snd_mix.c). */
extern int    s_float_output;
extern float *snd_float_buffer;
retro_environment_t environ_cb;
static retro_input_poll_t poll_cb;
static retro_input_state_t input_cb;
static bool libretro_supports_bitmasks = false;

static void audio_callback(void);

#define MAX_PADS 1
static unsigned quake_devices[1];

// System analog stick range is -0x8000 to 0x8000
#define ANALOG_RANGE 0x8000
// Default deadzone: 15%
static int analog_deadzone = (int)(0.15f * ANALOG_RANGE);

#define GP_MAXBINDS 32


typedef struct {
   struct retro_input_descriptor desc[GP_MAXBINDS];
   struct {
      char *key;
      char *com;
   } bind[GP_MAXBINDS];
} gp_layout_t;

gp_layout_t modern = {
   {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Swim Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Strafe Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Strafe Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Swim Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "Previous Weapon" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "Next Weapon" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "Jump" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "Fire" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,"Show Scores" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Menu" },
      { 0 },
   },
   {
      {"JOY_LEFT",  "+moveleft"},     {"JOY_RIGHT", "+moveright"},
      {"JOY_DOWN",  "+back"},         {"JOY_UP",    "+forward"},
      {"JOY_B",     "+movedown"},     {"JOY_A",     "+moveright"},
      {"JOY_X",     "+moveup"},       {"JOY_Y",     "+moveleft"},
      {"JOY_L",     "impulse 12"},    {"JOY_R",     "impulse 10"},
      {"JOY_L2",    "+jump"},         {"JOY_R2",    "+attack"},
      {"JOY_SELECT","+showscores"},   {"JOY_START", "togglemenu"},
      { 0 },
   },
};
gp_layout_t classic = {

   {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Jump" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Cycle Weapon" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Freelook" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Fire" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "Strafe Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "Strafe Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "Look Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "Look Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,    "Move Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,    "Swim Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,"Toggle Run Mode" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Menu" },
      { 0 },
   },
   {
      {"JOY_LEFT",  "+left"},         {"JOY_RIGHT", "+right"},
      {"JOY_DOWN",  "+back"},         {"JOY_UP",    "+forward"},
      {"JOY_B",     "+jump"} ,        {"JOY_A",     "impulse 10"},
      {"JOY_X",     "+klook"},        {"JOY_Y",     "+attack"},
      {"JOY_L",     "+moveleft"},     {"JOY_R",     "+moveright"},
      {"JOY_L2",    "+lookup"},       {"JOY_R2",    "+lookdown"},
      {"JOY_L3",    "+movedown"},     {"JOY_R3",    "+moveup"},
      {"JOY_SELECT","+togglewalk"},   {"JOY_START", "togglemenu"},
      { 0 },
   },
};
gp_layout_t classic_alt = {

   {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Look Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Look Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Look Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Look Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "Jump" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "Fire" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "Run" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "Next Weapon" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,    "Move Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,    "Previous Weapon" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,"Toggle Run Mode" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Menu" },
      { 0 },
   },
   {
      {"JOY_LEFT",  "+moveleft"},     {"JOY_RIGHT", "+moveright"},
      {"JOY_DOWN",  "+back"},         {"JOY_UP",    "+forward"},
      {"JOY_B",     "+lookdown"},     {"JOY_A",     "+right"},
      {"JOY_X",     "+lookup"},       {"JOY_Y",     "+left"},
      {"JOY_L",     "+jump"},         {"JOY_R",     "+attack"},
      {"JOY_L2",    "+speed"},          {"JOY_R2",    "impulse 10"},
      {"JOY_L3",    "+movedown"},     {"JOY_R3",    "impulse 12"},
      {"JOY_SELECT","+togglewalk"},   {"JOY_START", "togglemenu"},
      { 0 },
   },
};

gp_layout_t *gp_layoutp = NULL;

/* sys.c */
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>

#include "errno.h"

#define RETRO_DEVICE_MODERN  RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 2)
#define RETRO_DEVICE_JOYPAD_ALT  RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 1)

static bool context_needs_reinit = true;

#ifndef RENDERER_GL2
static bool initialize_gl()
{
	funcs[0].ptr  = qglTexImage2D         = (void (*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const GLvoid *))hw_render.get_proc_address ("glTexImage2D");
	funcs[2].ptr  = qglTexParameteri      = (void (*)(GLenum, GLenum, GLint))hw_render.get_proc_address ("glTexParameteri");
	funcs[3].ptr  = qglBindFramebuffer    = (void (*)(GLenum, GLuint))hw_render.get_proc_address ("glBindFramebuffer");
	funcs[4].ptr  = qglGenerateMipmap     = (void (*)(GLenum))hw_render.get_proc_address ("glGenerateMipmap");
	funcs[5].ptr  = qglBlendFunc          = (void (*)(GLenum, GLenum))hw_render.get_proc_address ("glBlendFunc");
	funcs[6].ptr  = qglTexSubImage2D      = (void (*)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const GLvoid *))hw_render.get_proc_address ("glTexSubImage2D");
	funcs[7].ptr  = qglDepthMask          = (void (*)(GLboolean))hw_render.get_proc_address ("glDepthMask");
	funcs[8].ptr  = qglPushMatrix         = (void (*)(void))hw_render.get_proc_address ("glPushMatrix");
	funcs[9].ptr  = qglRotatef            = (void (*)(GLfloat, GLfloat, GLfloat, GLfloat))hw_render.get_proc_address ("glRotatef");
	funcs[10].ptr = qglTranslatef         = (void (*)(GLfloat, GLfloat, GLfloat))hw_render.get_proc_address ("glTranslatef");
	funcs[11].ptr = qglDepthRange         = (void (*)(GLclampd, GLclampd))hw_render.get_proc_address ("glDepthRange");
	funcs[12].ptr = qglClear              = (void (*)(GLbitfield))hw_render.get_proc_address ("glClear");
	funcs[13].ptr = qglCullFace           = (void (*)(GLenum))hw_render.get_proc_address ("glCullFace");
	funcs[14].ptr = qglClearColor         = (void (*)(GLclampf, GLclampf, GLclampf, GLclampf))hw_render.get_proc_address ("glClearColor");
	funcs[15].ptr = qglEnable             = (void (*)(GLenum))hw_render.get_proc_address ("glEnable");
	funcs[16].ptr = qglDisable            = (void (*)(GLenum))hw_render.get_proc_address ("glDisable");
	funcs[17].ptr = qglEnableClientState  = (void (*)(GLenum))hw_render.get_proc_address ("glEnableClientState");
	funcs[18].ptr = qglDisableClientState = (void (*)(GLenum))hw_render.get_proc_address ("glDisableClientState");
	funcs[19].ptr = qglPopMatrix          = (void (*)(void))hw_render.get_proc_address ("glPopMatrix");
	funcs[20].ptr = qglGetFloatv          = (void (*)(GLenum, GLfloat *))hw_render.get_proc_address ("glGetFloatv");
	funcs[21].ptr = qglOrtho              = (void (*)(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble))hw_render.get_proc_address ("glOrtho");
	funcs[22].ptr = qglFrustum            = (void (*)(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble))hw_render.get_proc_address ("glFrustum");
	funcs[23].ptr = qglLoadMatrixf        = (void (*)(const GLfloat *))hw_render.get_proc_address ("glLoadMatrixf");
	funcs[24].ptr = qglLoadIdentity       = (void (*)(void))hw_render.get_proc_address ("glLoadIdentity");
	funcs[25].ptr = qglMatrixMode         = (void (*)(GLenum))hw_render.get_proc_address ("glMatrixMode");
	funcs[26].ptr = qglBindTexture        = (void (*)(GLenum, GLuint))hw_render.get_proc_address ("glBindTexture");
	funcs[27].ptr = qglReadPixels         = (void (*)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, GLvoid *))hw_render.get_proc_address ("glReadPixels");
	funcs[28].ptr = qglPolygonMode        = (void (*)(GLenum, GLenum))hw_render.get_proc_address ("glPolygonMode");
	funcs[29].ptr = qglVertexPointer      = (void (*)(GLint, GLenum, GLsizei, const GLvoid *))hw_render.get_proc_address ("glVertexPointer");
	funcs[30].ptr = qglTexCoordPointer    = (void (*)(GLint, GLenum, GLsizei, const GLvoid *))hw_render.get_proc_address ("glTexCoordPointer");
	funcs[31].ptr = qglColorPointer       = (void (*)(GLint, GLenum, GLsizei, const GLvoid *))hw_render.get_proc_address ("glColorPointer");
	funcs[32].ptr = qglDrawElements       = (void (*)(GLenum, GLsizei, GLenum, const GLvoid *))hw_render.get_proc_address ("glDrawElements");
	funcs[33].ptr = qglViewport           = (void (*)(GLint, GLint, GLsizei, GLsizei))hw_render.get_proc_address ("glViewport");
	funcs[34].ptr = qglDeleteTextures     = (void (*)(GLsizei, const GLuint *))hw_render.get_proc_address ("glDeleteTextures");
	funcs[35].ptr = qglClearStencil       = (void (*)(GLint))hw_render.get_proc_address ("glClearStencil");
	funcs[36].ptr = qglColor4f            = (void (*)(GLfloat, GLfloat, GLfloat, GLfloat))hw_render.get_proc_address ("glColor4f");
	funcs[37].ptr = qglScissor            = (void (*)(GLint, GLint, GLsizei, GLsizei))hw_render.get_proc_address ("glScissor");
	funcs[38].ptr = qglStencilFunc        = (void (*)(GLenum, GLint, GLuint))hw_render.get_proc_address ("glStencilFunc");
	funcs[39].ptr = qglStencilOp          = (void (*)(GLenum, GLenum, GLenum))hw_render.get_proc_address ("glStencilOp");
	funcs[40].ptr = qglScalef             = (void (*)(GLfloat, GLfloat, GLfloat))hw_render.get_proc_address ("glScalef");
	funcs[41].ptr = qglDepthFunc          = (void (*)(GLenum))hw_render.get_proc_address ("glDepthFunc");
	funcs[42].ptr = qglTexEnvi            = (void (*)(GLenum, GLenum, GLint))hw_render.get_proc_address ("glTexEnvi");
	funcs[43].ptr = qglAlphaFunc          = (void (*)(GLenum, GLclampf))hw_render.get_proc_address ("glAlphaFunc");
	funcs[44].ptr = qglClearDepth         = (void (*)(GLdouble))hw_render.get_proc_address ("glClearDepth");
	funcs[45].ptr = qglFinish             = (void (*)(void))hw_render.get_proc_address ("glFinish");
	funcs[46].ptr = qglGenTextures        = (void (*)(GLsizei, GLuint *))hw_render.get_proc_address ("glGenTextures");
	funcs[47].ptr = qglPolygonOffset      = (void (*)(GLfloat, GLfloat))hw_render.get_proc_address ("glPolygonOffset");
	funcs[48].ptr = qglClipPlane          = (void (*)(GLenum, const GLdouble *))hw_render.get_proc_address ("glClipPlane");
	funcs[49].ptr = qglColorMask          = (void (*)(GLboolean, GLboolean, GLboolean, GLboolean))hw_render.get_proc_address ("glColorMask");
	funcs[50].ptr = qglLineWidth          = (void (*)(GLfloat))hw_render.get_proc_address ("glLineWidth");
	funcs[51].ptr = qglStencilMask        = (void (*)(GLuint))hw_render.get_proc_address ("glStencilMask");
	
	if (log_cb) {
		int i;
		for (i = 0; i < GL_FUNCS_NUM; i++) {
			/* Slot 1 is intentionally unused: the assignment list jumps from
			 * funcs[0] to funcs[2]. funcs[] feeds only this diagnostic loop, so
			 * skip it rather than emit a spurious "cannot get GL function #1"
			 * error on every boot. */
			if (i == 1)
				continue;
			if (!funcs[i].ptr) log_cb(RETRO_LOG_ERROR, "vitaQuakeIII: cannot get GL function #%d symbol.\n", i);
		}
	}
	
	return true;
}

#endif

static void context_destroy() 
{
	context_needs_reinit = true;
}

/* Forward declaration for Key_Event (defined later in file) */
void Key_Event(int key, int value, int time);

/* Translate a libretro RETROK_* keycode to a Quake 3 keynum. Most printable
 * ASCII keys share their value with the Quake keynum, so they pass through; the
 * ones that don't (arrows, backspace/delete, modifiers, console) are mapped
 * explicitly. WASD are remapped onto the arrow movement keys so the bundled
 * config's arrow binds drive FPS movement without needing WASD binds (this
 * preserves the device's existing movement behaviour). Returns -1 for keys to
 * ignore. */
static int retrok_to_quake(unsigned keycode)
{
	switch (keycode)
	{
		/* WASD -> arrow movement keys (config binds arrows to forward/back and
		 * strafe), matching the device's existing layout. */
		case RETROK_w:         return K_UPARROW;
		case RETROK_s:         return K_DOWNARROW;
		case RETROK_a:         return K_LEFTARROW;
		case RETROK_d:         return K_RIGHTARROW;
		/* Arrow keys -> same directional codes (movement / menu navigation). */
		case RETROK_UP:        return K_UPARROW;
		case RETROK_DOWN:      return K_DOWNARROW;
		case RETROK_LEFT:      return K_LEFTARROW;
		case RETROK_RIGHT:     return K_RIGHTARROW;
		/* Keys whose RETROK value differs from the Quake keynum. */
		case RETROK_BACKQUOTE: return K_CONSOLE;
		case RETROK_BACKSPACE: return K_BACKSPACE; /* 8 -> 127 */
		case RETROK_DELETE:    return K_DEL;       /* 127 would otherwise alias backspace */
		case RETROK_LSHIFT:
		case RETROK_RSHIFT:    return K_SHIFT;
		case RETROK_LCTRL:
		case RETROK_RCTRL:     return K_CTRL;
		case RETROK_LALT:
		case RETROK_RALT:      return K_ALT;
		default:
			/* Printable ASCII (letters, digits, space, enter, escape, tab,
			 * punctuation) shares the Quake keynum, so pass it through - this is
			 * what makes the full keyboard work in the console and menus. Number
			 * keys reach their "weapon N" binds the same way. Everything else
			 * (function keys, keypad, etc.) is ignored. */
			if (keycode < 128)
				return (int)keycode;
			return -1;
	}
}

static void keyboard_cb(bool down, unsigned keycode, uint32_t character, uint16_t mod)
{
	int qkey;

	/* Event-driven keyboard: the frontend pushes each key transition here, so
	 * there is no per-frame keyboard polling. Gated on the keyboard device so it
	 * matches the controller the user selected. */
	if (quake_devices[0] != RETRO_DEVICE_KEYBOARD)
		return;
	if (keycode == RETROK_UNKNOWN)
		return;

	qkey = retrok_to_quake(keycode);
	if (qkey < 0)
		return; /* unmapped key (the engine also rejects keynum >= MAX_KEYS) */

	Key_Event(qkey, down ? 1 : 0, com_frameTime);
}

static bool first_reset = true;

extern void CL_Vid_Restart_f( void );

#ifdef RENDERER_GL2
/* The OpenGL 2 renderer runs on desktop OpenGL 3.2 core or on OpenGL ES 3/2,
 * whichever the frontend gives, with nothing between it and the context: it
 * keeps its own idea of the GL state, so the frontend is asked for a shared
 * context, where it keeps its state apart from the core's. */
struct retro_hw_render_callback hw_render;

GLuint GLimp_DefaultFramebuffer(void)
{
	return hw_render.get_current_framebuffer ? (GLuint)hw_render.get_current_framebuffer() : 0;
}

static void context_reset() {
	if (!context_needs_reinit)
		return;

	if (!first_reset)
		CL_Vid_Restart_f();
	first_reset = false;
	context_needs_reinit = false;
}

bool initialize_opengl(void)
{
	static const struct { enum retro_hw_context_type type; unsigned major, minor; } contexts[] = {
#ifdef HAVE_OPENGLES
		{ RETRO_HW_CONTEXT_OPENGLES3, 3, 0 },
		{ RETRO_HW_CONTEXT_OPENGLES2, 2, 0 },
#else
		{ RETRO_HW_CONTEXT_OPENGL_CORE, 3, 2 },
		{ RETRO_HW_CONTEXT_OPENGL, 2, 1 },
#endif
	};
	unsigned i;

	for (i = 0; i < sizeof(contexts) / sizeof(contexts[0]); i++)
	{
		memset(&hw_render, 0, sizeof(hw_render));
		hw_render.context_type       = contexts[i].type;
		hw_render.version_major      = contexts[i].major;
		hw_render.version_minor      = contexts[i].minor;
		hw_render.context_reset      = context_reset;
		hw_render.context_destroy    = context_destroy;
		hw_render.depth              = true;
		hw_render.stencil            = true;
		hw_render.bottom_left_origin = true;
		if (environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render))
		{
			log_cb(RETRO_LOG_INFO, "vitaQuakeIII: OpenGL%s %u.%u context requested\n",
#ifdef HAVE_OPENGLES
				" ES",
#else
				contexts[i].type == RETRO_HW_CONTEXT_OPENGL_CORE ? " core" : "",
#endif
				contexts[i].major, contexts[i].minor);
			libretro_shared_context = environ_cb(RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT, NULL);
			return true;
		}
	}

	log_cb(RETRO_LOG_ERROR, "vitaQuakeIII: the frontend gave no OpenGL context.\n");
	return false;
}

void destroy_opengl(void)
{
	libretro_shared_context = false;
}
#else
static void context_reset() { 
	if (!context_needs_reinit)
		return;
	
	glsm_ctl(GLSM_CTL_STATE_CONTEXT_RESET, NULL);

   if (!libretro_shared_context)
      if (!glsm_ctl(GLSM_CTL_STATE_SETUP, NULL))
         return;

	initialize_gl();
	if (!first_reset) {
		CL_Vid_Restart_f();
	}
	first_reset = false;
	context_needs_reinit = false;
}

static bool context_framebuffer_lock(void *data)
{
    return false;
}

bool initialize_opengl(void)
{
   glsm_ctx_params_t params = {0};

   params.context_type     = RETRO_HW_CONTEXT_OPENGL;
   params.context_reset    = context_reset;
   params.context_destroy  = context_destroy;
   params.environ_cb       = environ_cb;
   params.stencil          = true;
   params.framebuffer_lock = context_framebuffer_lock;

   if (!glsm_ctl(GLSM_CTL_STATE_CONTEXT_INIT, &params))
   {
      log_cb(RETRO_LOG_ERROR, "Could not setup glsm.\n");
      return false;
   }

   if (environ_cb(RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT, NULL))
      libretro_shared_context = true;
   else
      libretro_shared_context = false;

   return true;
}

void destroy_opengl(void)
{
   if (!glsm_ctl(GLSM_CTL_STATE_CONTEXT_DESTROY, NULL))
   {
      log_cb(RETRO_LOG_ERROR, "Could not destroy glsm context.\n");
   }

   libretro_shared_context = false;
}

#endif

/* con.c */

/*
==================
CON_Shutdown
==================
*/
void CON_Shutdown(void) {
}

/*
==================
CON_Init
==================
*/
void CON_Init(void) {
}

/*
==================
CON_Input
==================
*/
char *CON_Input(void) {
    return NULL;
}

/*
==================
CON_Print
==================
*/
void CON_Print(const char *msg) {
    /* Route engine output to the libretro log so Com_Printf/Com_DPrintf and,
     * crucially, fatal Sys_Error messages are visible in the frontend log.
     * Previously this was #ifndef RELEASE only, so release cores swallowed all
     * engine diagnostics -- a fatal during Com_Init looked like a silent exit. */
    if (log_cb && msg && *msg)
        log_cb(RETRO_LOG_INFO, "%s", msg);
}

/* glimp_gamma.c */

void GLimp_SetGamma( unsigned char red[256], unsigned char green[256], unsigned char blue[256] )
{
}

/* sys_main.c */
static char binaryPath[MAX_OSPATH] = {0};
static char installPath[MAX_OSPATH] = {0};

/*
=================
Sys_SetBinaryPath
=================
*/
void Sys_SetBinaryPath(const char *path) {
    Q_strncpyz(binaryPath, path, sizeof(binaryPath));
}

/*
=================
Sys_BinaryPath
=================
*/
char *Sys_BinaryPath(void) {
    return binaryPath;
}

/*
=================
Sys_SetDefaultInstallPath
=================
*/
void Sys_SetDefaultInstallPath(const char *path) {
    Q_strncpyz(installPath, path, sizeof(installPath));
}

/*
=================
Sys_DefaultInstallPath
=================
*/
char *Sys_DefaultInstallPath(void) {
    if (*installPath)
        return installPath;
    else
        return Sys_Cwd();
}

/*
=================
Sys_DefaultAppPath
=================
*/
char *Sys_DefaultAppPath(void) {
    return Sys_BinaryPath();
}

/*
=================
Sys_In_Restart_f
Restart the input subsystem
=================
*/
void Sys_In_Restart_f(void) {
    IN_Restart();
}

/*
=================
Sys_ConsoleInput
Handle new console input
=================
*/
char *Sys_ConsoleInput(void) {
    return CON_Input();
}

/*
==================
Sys_GetClipboardData
==================
*/
char *Sys_GetClipboardData(void) {
#ifdef DEDICATED
    return NULL;
#else
    /* No clipboard integration in the libretro core. */
    return NULL;
#endif
}

void Sys_InitPIDFile(const char *gamedir) {}

void Sys_RemovePIDFile(const char *gamedir) {}

/*
=================
Sys_Exit
Single exit point (regular exit or in case of error)
=================
*/
static __attribute__ ((noreturn)) void Sys_Exit(int exitCode) {
    CON_Shutdown();

    NET_Shutdown();

    Sys_PlatformExit();

    exit(exitCode);
}

/*
=================
Sys_Quit
=================
*/
void Sys_Quit(void) {
    Sys_Exit(0);
}

/*
=================
Sys_GetProcessorFeatures
=================
*/
cpuFeatures_t Sys_GetProcessorFeatures(void) {
    return (cpuFeatures_t) 0;
}

/*
=================
Sys_Init
=================
*/
void Sys_Init(void) {
    Cmd_AddCommand("in_restart", Sys_In_Restart_f);
    Cvar_Set("arch", OS_STRING " " ARCH_STRING);
    Cvar_Set("name", Sys_GetCurrentUser());
}

/*
=================
Sys_Print
=================
*/
void Sys_Print(const char *msg) {
    CON_Print(msg);
}

/*
=================
Sys_Error
=================
*/
void Sys_Error(const char *error, ...) {
    va_list argptr;
    char string[1024];

    va_start (argptr, error);
    Q_vsnprintf(string, sizeof(string), error, argptr);
    va_end (argptr);

    /* Make the fatal reason visible in the frontend log instead of exiting
     * silently. */
    if (log_cb)
        log_cb(RETRO_LOG_ERROR, "vitaQuakeIII fatal: %s\n", string);

    Sys_ErrorDialog(string);

    Sys_Exit(3);
}

/*
=================
Sys_UnloadDll
=================
*/
void Sys_UnloadDll( void *dllHandle )
{
}

/*
=================
Sys_LoadDll
First try to load library name from system library path,
from executable path, then fs_basepath.
=================
*/

void *Sys_LoadDll(const char *name, qboolean useSystemLib)
{
	return NULL;
}

/*
=================
Sys_LoadGameDll
Used to load a development dll instead of a virtual machine
=================
*/
void *Sys_LoadGameDll(const char *name,
	intptr_t (QDECL **entryPoint)(int, ...),
	intptr_t (*systemcalls)(intptr_t, ...))
{
	return NULL;
}

/*
=================
Sys_ParseArgs
=================
*/
void Sys_ParseArgs(int argc, char **argv) {
    if (argc == 2) {
        if (!strcmp(argv[1], "--version") ||
            !strcmp(argv[1], "-v")) {
            const char *date = PRODUCT_DATE;
#ifdef DEDICATED
            fprintf( stdout, Q3_VERSION " dedicated server (%s)\n", date );
#else
            fprintf(stdout, Q3_VERSION " client (%s)\n", date);
#endif
            Sys_Exit(0);
        }
    }
}

#ifndef DEFAULT_BASEDIR
#	define DEFAULT_BASEDIR Sys_BinaryPath()
#endif

static void extract_directory(char *buf, const char *path, size_t size)
{
   char *base = NULL;

   strncpy(buf, path, size - 1);
   buf[size - 1] = '\0';

   base = strrchr(buf, '/');
   if (!base)
      base = strrchr(buf, '\\');

   if (base)
      *base = '\0';
   else
    {
       buf[0] = '.';
       buf[1] = '\0';
    }
}

void retro_init(void)
{
   struct retro_log_callback log;

   if(environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      log_cb = log.log;
   else
      log_cb = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL))
      libretro_supports_bitmasks = true;
}

void retro_deinit(void)
{
   /* core_teardown() is idempotent; ensures cleanup even if the frontend calls
    * retro_deinit without a preceding retro_unload_game. */
   core_teardown();
   libretro_supports_bitmasks = false;
}

void gp_layout_set_bind(gp_layout_t gp_layout)
{
   char buf[100];
   unsigned i;
   for (i=0; gp_layout.bind[i].key; ++i)
   {
      snprintf(buf, sizeof(buf), "bind %s \"%s\"\n", gp_layout.bind[i].key,
                                                   gp_layout.bind[i].com);
      Cbuf_AddText(buf);
   }
}

bool initial_resolution_set = false;

/* Snap a host target rate to the nearest value the option advertises. */
static int nearest_supported_rate(unsigned host_rate)
{
	if      (host_rate <= (32000u + 44100u) / 2) return 32000;
	else if (host_rate <= (44100u + 48000u) / 2) return 44100;
	else if (host_rate <= (48000u + 96000u) / 2) return 48000;
	return 96000;
}

/* Resolve the "Sound Samplerate (Hint)" core option to a concrete rate.
 * "auto" asks the frontend for its target rate via
 * RETRO_ENVIRONMENT_GET_TARGET_SAMPLE_RATE and snaps to the nearest advertised
 * value; if the frontend doesn't implement the call it keeps the default
 * (48 kHz), so frontends without the call see no change. An explicit
 * "32000".."96000" is taken verbatim. Resolved before SNDDMA_Init reads it. */
static void update_audio_samplerate(void)
{
	struct retro_variable var;
	int chosen = SAMPLE_RATE_DEFAULT;

	var.key   = "vitaquakeiii_audio_samplerate";
	var.value = NULL;

	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		if (!strcmp(var.value, "auto"))
		{
			unsigned host_rate = 0;
			if (environ_cb(RETRO_ENVIRONMENT_GET_TARGET_SAMPLE_RATE, &host_rate)
					&& host_rate > 0)
				chosen = nearest_supported_rate(host_rate);
			/* else: keep the default */
		}
		else
			chosen = atoi(var.value);  /* "32000".."96000" */
	}

	audio_sample_rate = chosen;
}

static void update_variables(bool startup)
{
	struct retro_variable var;
	
	var.key = "vitaquakeiii_framerate";
	var.value = NULL;
	
	if (startup)
	{
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
		{
			if (!strcmp(var.value, "auto"))
			{
				float target_framerate = 0.0f;
				if (!environ_cb(RETRO_ENVIRONMENT_GET_TARGET_REFRESH_RATE, &target_framerate))
					target_framerate = 60.0f;
				framerate = (unsigned)target_framerate;
			}
			else
				framerate = atoi(var.value);
		}
		else
			framerate    = 60;

		/* Sound output rate is fixed for the session; resolve it once at
		 * startup, before SNDDMA_Init reads audio_sample_rate. */
		update_audio_samplerate();
	}
	
	var.key = "vitaquakeiii_resolution";
	var.value = NULL;
	
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && !initial_resolution_set)
	{
		char *pch;
		char str[100];
		snprintf(str, sizeof(str), "%s", var.value);

		pch = strtok(str, "x");
		if (pch)
			scr_width = strtoul(pch, NULL, 0);
		pch = strtok(NULL, "x");
		if (pch)
			scr_height = strtoul(pch, NULL, 0);

		if (log_cb)
			log_cb(RETRO_LOG_INFO, "Got size: %u x %u.\n", scr_width, scr_height);

		initial_resolution_set = true;
	}
   
	var.key = "vitaquakeiii_invert_y_axis";
	var.value = NULL;

	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		if (strcmp(var.value, "disabled") == 0)
			invert_y_axis = 1;
		else
			invert_y_axis = -1;
	}

	// We need setup sequence to be finished to change Cvar values
	if (!startup) {
      var.key = "vitaquakeiii_overbrights";
		var.value = NULL;

		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			if (strcmp(var.value, "disabled") == 0)
				Cvar_SetValue("r_overbrightBits", 0);
			else
				Cvar_SetValue("r_overbrightBits", 1);
		}

		var.key = "vitaquakeiii_wide";
		var.value = NULL;

		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
         struct retro_system_av_info info;
			if (strcmp(var.value, "disabled") == 0)
         {
				Cvar_SetValue("r_widescreen", 0);
            widescreen_enabled = false;
         }
			else
         {
				Cvar_SetValue("r_widescreen", 1);
            widescreen_enabled = true;
         }
         retro_get_system_av_info(&info);
         environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &info.geometry);
		}
		
		var.key = "vitaquakeiii_pickups";
		var.value = NULL;

		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			if (strcmp(var.value, "disabled") == 0)
				Cvar_SetValue("cg_simpleItems", 0);
			else
				Cvar_SetValue("cg_simpleItems", 1);
		}
		
		var.key = "vitaquakeiii_weapon";
		var.value = NULL;

		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			if (strcmp(var.value, "disabled") == 0)
				Cvar_SetValue("cg_drawgun", 0);
			else
				Cvar_SetValue("cg_drawgun", 1);
		}
		
		var.key = "vitaquakeiii_shadows";
		var.value = NULL;
		
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			if (strcmp(var.value, "disabled") == 0)
				Cvar_SetValue("cg_shadows", 0);
			else if (strcmp(var.value, "low") == 0)
				Cvar_SetValue("cg_shadows", 1);
			else if (strcmp(var.value, "high") == 0)
				Cvar_SetValue("cg_shadows", 3);
		}
		
		var.key = "vitaquakeiii_filter";
		var.value = NULL;
		
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			if (strcmp(var.value, "disabled") == 0)
				Cvar_Set("r_textureMode", "GL_NEAREST");
			else if (strcmp(var.value, "linear") == 0)
				Cvar_Set("r_textureMode", "GL_LINEAR");
			else if (strcmp(var.value, "bilinear") == 0)
				Cvar_Set("r_textureMode", "GL_LINEAR_MIPMAP_NEAREST");
			else
				Cvar_Set("r_textureMode", "GL_LINEAR_MIPMAP_LINEAR");
		}
	}
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
   if (port == 0)
   {
      switch (device)
      {
         case RETRO_DEVICE_JOYPAD:
            quake_devices[port] = RETRO_DEVICE_JOYPAD;
            environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, classic.desc);
            gp_layout_set_bind(classic);
            break;
         case RETRO_DEVICE_JOYPAD_ALT:
            quake_devices[port] = RETRO_DEVICE_JOYPAD;
            environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, classic_alt.desc);
            gp_layout_set_bind(classic_alt);
            break;
         case RETRO_DEVICE_MODERN:
            quake_devices[port] = RETRO_DEVICE_MODERN;
            environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, modern.desc);
            gp_layout_set_bind(modern);
            break;
         case RETRO_DEVICE_KEYBOARD:
            quake_devices[port] = RETRO_DEVICE_KEYBOARD;
            break;
         case RETRO_DEVICE_NONE:
         default:
            quake_devices[port] = RETRO_DEVICE_NONE;
            if (log_cb)
               log_cb(RETRO_LOG_ERROR, "[libretro]: Invalid device.\n");
      }
   }
}

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = "vitaQuakeIII";
   info->library_version  = "v0.8" ;
   info->need_fullpath    = true;
   info->valid_extensions = "pk3";
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   info->timing.fps            = framerate;
   info->timing.sample_rate    = audio_sample_rate;

   info->geometry.base_width   = scr_width;
   info->geometry.base_height  = scr_height;
   info->geometry.max_width    = scr_width;
   info->geometry.max_height   = scr_height;
   if (widescreen_enabled)
      info->geometry.aspect_ratio = (16.0f) / (9.0f);
   else
      info->geometry.aspect_ratio = (scr_width * 1.0f) / (scr_height * 1.0f);
}

void retro_set_environment(retro_environment_t cb)
{
   static const struct retro_controller_description port_1[] = {
      { "Gamepad Classic", RETRO_DEVICE_JOYPAD },
      { "Gamepad Classic Alt", RETRO_DEVICE_JOYPAD_ALT },
      { "Gamepad Modern", RETRO_DEVICE_MODERN },
      { "RetroKeyboard + Mouse", RETRO_DEVICE_KEYBOARD },
   };

   static const struct retro_controller_info ports[] = {
      { port_1, 4 },
      { 0 },
   };

   environ_cb = cb;

   {
      bool categories_supported = false;
      libretro_set_core_options(environ_cb, &categories_supported);
   }
   cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)ports);

   /* Obtain the frontend VFS interface and hand it to libretro-common's
    * filestream layer. All engine file I/O (qcommon/files.c, qcommon/ioapi.c)
    * is routed through filestream_*, so this makes the core use the frontend's
    * virtual filesystem when available, and falls back to libretro-common's
    * own vfs_implementation.c otherwise. Must run before any file access. */
   {
      struct retro_vfs_interface_info vfs_iface_info;
      vfs_iface_info.required_interface_version = FILESTREAM_REQUIRED_VFS_VERSION;
      vfs_iface_info.iface                      = NULL;
      if (cb(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &vfs_iface_info))
         filestream_vfs_init(&vfs_iface_info);
   }
}

/* TODO/FIXME */
void retro_reset(void) { }
void retro_set_rumble_strong(void) { }
void retro_unset_rumble_strong(void) { }

void retro_set_audio_sample(retro_audio_sample_t cb)
{
   audio_cb = cb;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
   audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb)
{
   poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
   input_cb = cb;
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
   video_cb = cb;
}

/* Full teardown shared by retro_unload_game/retro_deinit. Mirrors the engine's
 * clean-quit path (Com_Quit_f) but WITHOUT Sys_Quit()/exit(), frees the buffers
 * this translation unit owns, and resets every one-shot init guard so a
 * subsequent retro_load_game in the same process re-initialises correctly. */
static void core_teardown(void)
{
   if (core_initialized)
   {
      /* Tear the engine down the same way the 'quit' command would. */
      VM_Forced_Unload_Start();
      SV_Shutdown("Server quit");
      CL_Shutdown("Client quit", qtrue, qtrue);
      VM_Forced_Unload_Done();
      Com_Shutdown();
      FS_Shutdown(qtrue);
      core_initialized = false;
   }

#ifndef RENDERER_GL2
   /* Free the persistent immediate-mode batch buffers (allocated in GLimp_Init). */
   if (indices)            { free(indices);            indices            = NULL; }
   if (gVertexBufferPtr)   { free(gVertexBufferPtr);   gVertexBufferPtr   = NULL; }
   if (gColorBufferPtr)    { free(gColorBufferPtr);    gColorBufferPtr    = NULL; }
   if (gTexCoordBufferPtr) { free(gTexCoordBufferPtr); gTexCoordBufferPtr = NULL; }
   if (gColorBuffer255)    { free(gColorBuffer255);    gColorBuffer255    = NULL; }
   gVertexBuffer   = NULL;
   gColorBuffer    = NULL;
   gTexCoordBuffer = NULL;
#endif

   if (BASEGAME) { free(BASEGAME); BASEGAME = NULL; }

   /* Reset one-shot guards so a second load re-runs init from scratch. */
   first_boot             = true;
   first_reset            = true;
   inited                 = 0;
   psp2_inited            = 0;
   snd_inited             = qfalse;
   initial_resolution_set = false;
   context_needs_reinit   = true;
   frame_time_us          = 0;
   frame_ms_remainder     = 0;
}

void retro_unload_game(void)
{
   core_teardown();
}

unsigned retro_get_region(void)
{
   return RETRO_REGION_NTSC;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{
   (void)type;
   (void)info;
   (void)num;
   return false;
}

size_t retro_serialize_size(void)
{
   return 0;
}

bool retro_serialize(void *data_, size_t size)
{
   return false;
}

bool retro_unserialize(const void *data_, size_t size)
{
   return false;
}

void *retro_get_memory_data(unsigned id)
{
   (void)id;
   return NULL;
}

size_t retro_get_memory_size(unsigned id)
{
   (void)id;
   return 0;
}

void retro_cheat_reset(void)
{}

void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
   (void)index;
   (void)enabled;
   (void)code;
}

static void extract_basename(char *buf, const char *path, size_t size)
{
   char *ext        = NULL;
   const char *base = strrchr(path, '/');
   if (!base)
      base = strrchr(path, '\\');
   if (!base)
      base = path;

   if (*base == '\\' || *base == '/')
      base++;

   strncpy(buf, base, size - 1);
   buf[size - 1] = '\0';

   ext = strrchr(buf, '.');
   if (ext)
      *ext = '\0';
}

unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

char full_game_dir[256];

static void audio_process(void)
{
}

static void audio_callback(void)
{
	/* Frame-locked: the engine mixed exactly one video frame's worth of audio
	 * (frame_samps stereo pairs) into the linear output buffer during
	 * Com_Frame -> S_Update -> S_Update_. Hand that buffer straight to the
	 * frontend. No DMA read cursor, no ring, no wrap. */
	unsigned frame_samps            = audio_sample_rate / framerate; /* stereo pairs */
	unsigned audio_frames_remaining = frame_samps;
	uint64_t av_flags               = RETRO_AV_ENABLE_VIDEO | RETRO_AV_ENABLE_AUDIO;
	int      push_audio;

	if (!snd_inited)
		return;

	/* Let the frontend tell us when audio output isn't needed.
	 * HARD_DISABLE_AUDIO (e.g. a run-ahead secondary instance whose audio is
	 * never consumed): the mixer is output-only and never feeds back into game
	 * state, so skip the whole step. AUDIO cleared without HARD_DISABLE (e.g.
	 * fast-forward): still mix so engine state advances correctly, just don't
	 * push the buffer. Older frontends that don't implement env 47 fall through
	 * with both flags set. */
	environ_cb(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE, &av_flags);
	if (av_flags & RETRO_AV_ENABLE_HARD_DISABLE_AUDIO)
		return;
	push_audio = (av_flags & RETRO_AV_ENABLE_AUDIO) != 0;

	if (!push_audio)
		return;

	if (use_float_output)
	{
		float *p_f = audio_buffer_f;
		do
		{
			unsigned to_write =
				(audio_frames_remaining > audio_batch_frames_max) ?
				audio_batch_frames_max : audio_frames_remaining;
			unsigned written = audio_batch_cb_float(p_f, to_write);

			if (written == 0)
				break;	/* frontend can't accept any more this frame */
			if (written < to_write)
				audio_batch_frames_max = written;

			audio_frames_remaining -= written;
			p_f                    += written << 1;
		}
		while (audio_frames_remaining > 0);
		return;
	}

	{
		int16_t *p = audio_buffer;
		do
		{
			unsigned to_write =
				(audio_frames_remaining > audio_batch_frames_max) ?
				audio_batch_frames_max : audio_frames_remaining;
			unsigned written = audio_batch_cb(p, to_write);

			if (written == 0)
				break;
			if (written < to_write)
				audio_batch_frames_max = written;

			audio_frames_remaining -= written;
			p                      += written << 1;
		}
		while (audio_frames_remaining > 0);
	}
}

char is_missionpack = false;
char is_urt = false;
char is_standalone = false;
char is_oa = false;
char is_rally = false;

bool retro_load_game(const struct retro_game_info *info)
{
	enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
	if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
	{
		if (log_cb)
			log_cb(RETRO_LOG_INFO, "XRGB8888 is not supported.\n");
		return false;
	}

	/* Negotiate float audio output once for this game's lifetime. If the
	 * frontend supports it we commit to float; otherwise the int16 path is used
	 * unchanged. (Contract: do this once per load, never mix formats mid-game.)
	 * SNDDMA_Init runs later in this function via the engine init and picks up
	 * use_float_output. */
	use_float_output     = 0;
	audio_batch_cb_float = NULL;
	audio_batch_frames_max = AUDIO_BUFFER_SIZE;
	{
		struct retro_audio_sample_float_callback fcb;
		fcb.batch = NULL;
		if (environ_cb(RETRO_ENVIRONMENT_GET_AUDIO_SAMPLE_BATCH_FLOAT, &fcb)
			&& fcb.batch)
		{
			audio_batch_cb_float = fcb.batch;
			use_float_output     = 1;
		}
	}
	s_float_output = use_float_output;

	if (!initialize_opengl())
	{
		if (log_cb)
			log_cb(RETRO_LOG_ERROR, "vitaQuakeIII: libretro frontend doesn't have OpenGL support.\n");
		return false;
	}
	
	int i;
	char path_lower[256];
#if defined(_WIN32)
	char slash = '\\';
#else
	char slash = '/';
#endif
	bool use_external_savedir = false;
	const char *base_save_dir = NULL;
	struct retro_keyboard_callback cb = { keyboard_cb };

	if (!info)
		return false;

	/* Reset the virtual clock so each load starts from t=0. */
	frame_time_us      = 0;
	frame_ms_remainder = 0;

	/* info->path is frontend-supplied and may exceed sizeof(path_lower);
	 * bound the copy to avoid a stack buffer overflow. */
	snprintf(path_lower, sizeof(path_lower), "%s", info->path);
	
	for (i=0; path_lower[i]; ++i)
		path_lower[i] = tolower(path_lower[i]);
	
	environ_cb(RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK, &cb);
	
	update_variables(true);

	extract_directory(g_rom_dir, info->path, sizeof(g_rom_dir));
	
	snprintf(g_pak_path, sizeof(g_pak_path), "%s", info->path);
	
	if (environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &base_save_dir) && base_save_dir)
	{
		if (strlen(base_save_dir) > 0)
		{
			// Get game 'name' (i.e. subdirectory)
			char game_name[1024];
			extract_basename(game_name, g_rom_dir, sizeof(game_name));
			
			// > Build final save path
			snprintf(g_save_dir, sizeof(g_save_dir), "%s%c%s", base_save_dir, slash, game_name);
			use_external_savedir = true;
			
			// > Create save directory, if required
			if (!path_is_directory(g_save_dir))
			{
				use_external_savedir = path_mkdir(g_save_dir);
			}
		}
	}
	
	// > Error check
	if (!use_external_savedir)
	{
		// > Use ROM directory fallback...
		snprintf(g_save_dir, sizeof(g_save_dir), "%s", g_rom_dir);
	}
	else
	{
		// > Final check: is the save directory the same as the 'rom' directory?
		//   (i.e. ensure logical behaviour if user has set a bizarre save path...)
		use_external_savedir = (strcmp(g_save_dir, g_rom_dir) != 0);
	}
	

	extract_directory(g_rom_dir, g_rom_dir, sizeof(g_rom_dir));
	
	BASEGAME = (char*)malloc(256);
	
	if (strstr(path_lower, "missionpack"))
		is_missionpack = true;
	
	if (strstr(path_lower, "q3ut4"))
		is_urt = true;
	
	if (strstr(path_lower, "baseoa")) {
		is_oa = true;
		is_standalone = true;
		sprintf(BASEGAME, "baseoa");
	} else if (strstr(path_lower, "baseq3r")) {
		is_rally = true;
		is_standalone = true;
		sprintf(BASEGAME, "baseq3r");
	} else sprintf(BASEGAME, "baseq3");
	
	Sys_SetBinaryPath(g_rom_dir);
    Sys_SetDefaultInstallPath(g_rom_dir);

	return true;
}

bool first_boot = true;

void retro_run(void)
{
#ifdef RENDERER_GL2
	/* Draw into the frontend's framebuffer. The renderer binds it itself when
	 * it presents from its own framebuffers, but without those - OpenGL ES
	 * here - it draws into whatever is bound, which in the core's shared
	 * context is that context's own default framebuffer, shown nowhere: the
	 * picture was black. Binding it through the renderer keeps the renderer's
	 * idea of what is bound true. */
	{
		extern void GLimp_BindDefaultFramebuffer(void);
		GLimp_BindDefaultFramebuffer();
	}
#else
   if (!libretro_shared_context)
      glsm_ctl(GLSM_CTL_STATE_BIND, NULL);
	qglBindFramebuffer(RARCH_GL_FRAMEBUFFER, hw_render.get_current_framebuffer());
	qglEnable(GL_TEXTURE_2D);
	qglEnableClientState(GL_VERTEX_ARRAY);
#endif

	/* Advance the frame clock by exactly one frame quantum and publish it to
	 * com_frameTime, the single engine clock. Integer ms/frame with a remainder
	 * carry keeps the long-run rate at precisely 'framerate' fps with no
	 * floating-point drift. com_frameTime is set here, before any engine code
	 * (Com_Frame, input, audio, server) reads the time this frame; nothing in
	 * the engine reads a host wall clock. */
	if (framerate > 0) {
		frame_time_us += 1000000u / (unsigned)framerate;
		frame_ms_remainder += 1000000u % (unsigned)framerate;
		if (frame_ms_remainder >= (unsigned)framerate) {
			frame_ms_remainder -= (unsigned)framerate;
			frame_time_us += 1;
		}
	}
	com_frameTime = (int)(frame_time_us / 1000);

	if (first_boot) {
		char commandLine[MAX_STRING_CHARS] = {0};
		struct retro_variable strict_var;
		
		// Starting input
		IN_Init(NULL);
		
		Sys_PlatformInit();

		CON_Init();
		if (is_missionpack) {
			snprintf(commandLine, sizeof(commandLine), "+set fs_game missionpack");
			printf("Launching Quake III: Team Arena...\n");
		} else if (is_urt) {
			snprintf(commandLine, sizeof(commandLine), "+set fs_game q3ut4");
			printf("Launching Urban Terror...\n");
		} else if (is_rally) {
			snprintf(commandLine, sizeof(commandLine), "+set fs_game baseq3r");
			printf("Launching Q3 Rally...\n");
		}

		/* The pak-integrity gate (FS_CheckPak0) runs inside Com_Init below, so
		 * fs_strictPaks must be set via the startup command line ('+set'), which
		 * Com_Init parses before FS_Startup -- not via update_variables(), which
		 * runs too late. Default is permissive (disabled): partial/CD installs
		 * missing the 1.32 point-release paks boot in a degraded mode instead of
		 * exiting. Pass the explicit value for either choice so the option always
		 * wins over the cvar default. */
		strict_var.key   = "vitaquakeiii_strict_paks";
		strict_var.value = NULL;
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &strict_var) &&
		    strict_var.value && !strcmp(strict_var.value, "enabled"))
		{
			Q_strcat(commandLine, sizeof(commandLine), " +set fs_strictPaks 1");
		}
		else
		{
			Q_strcat(commandLine, sizeof(commandLine), " +set fs_strictPaks 0");
		}

		Com_Init(commandLine);
		NET_Init();
		update_variables(false);
		Cvar_Set("name", Sys_GetCurrentUser());
		first_boot = false;
		core_initialized = true;
	}
	
	bool updated = false;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
		update_variables(false);
	
	Com_Frame();
#ifndef RENDERER_GL2
   if (!libretro_shared_context)
      glsm_ctl(GLSM_CTL_STATE_UNBIND, NULL);
#endif
	
	audio_process();
	audio_callback();
}

/* sys_*.c */

void log2file(const char *format, ...) {
	__gnuc_va_list arg;
	char msg[512];
	size_t len;
	va_start(arg, format);
	/* vsnprintf bounds the format expansion; vsprintf could overflow msg. */
	vsnprintf(msg, sizeof(msg), format, arg);
	va_end(arg);
	/* Append a newline in place rather than sprintf(msg, "%s\n", msg), which
	 * aliases src and dst (undefined behaviour). */
	len = strlen(msg);
	if (len < sizeof(msg) - 1) {
		msg[len]     = '\n';
		msg[len + 1] = '\0';
	}
	CON_Print(msg);
}

#ifndef RELEASE
# define printf log2file
#endif

// Used to determine where to store user-specific files
static char homePath[MAX_OSPATH] = {0};

// Used to store the Steam Quake 3 installation path
static char steamPath[MAX_OSPATH] = {0};

// Used to store the GOG Quake 3 installation path
static char gogPath[MAX_OSPATH] = {0};

#ifndef DEFAULT_BASEDIR
#	define DEFAULT_BASEDIR Sys_BinaryPath()
#endif

/*
==================
Sys_DefaultHomePath
==================
*/
char *Sys_DefaultHomePath(void) {
    return homePath;
}

/*
================
Sys_SteamPath
================
*/
char *Sys_SteamPath(void) {
    return steamPath;
}

/*
================
Sys_GogPath
================
*/
char *Sys_GogPath(void) {
    // GOG also doesn't let you install Quake 3 on Mac/Linux
    return gogPath;
}

/*
==================
Sys_RandomBytes
==================
*/
qboolean Sys_RandomBytes(byte *string, int len) {
    return qfalse;
}

/*
==================
Sys_GetCurrentUser
==================
*/
char nick[128];
char *Sys_GetCurrentUser(void) {
	const char *default_username = NULL;
	sprintf(nick, "UnnamedPlayer");
	
	if (environ_cb(RETRO_ENVIRONMENT_GET_USERNAME, &default_username))
	{
		if (default_username && default_username[0] != '\0')
		{
			sprintf(nick, default_username);
		}
	}
		
    return nick;
}

/*
==================
Sys_LowPhysicalMemory

TODO
==================
*/
qboolean Sys_LowPhysicalMemory(void) {
    return qfalse;
}

/*
==================
Sys_Basename
==================
*/
const char *Sys_Basename(char *path) {
    char *p = strrchr(path, '/');
    return p ? p + 1 : (char *) path;
}

/*
==================
Sys_Dirname
==================
*/
const char *Sys_Dirname(char *path) {
    static const char dot[] = ".";
    char *last_slash;

    /* Find last '/'.  */
    last_slash = path != NULL ? strrchr(path, '/') : NULL;

    if (last_slash != NULL && last_slash != path && last_slash[1] == '\0') {
        /* Determine whether all remaining characters are slashes.  */
        char *runp;

        for (runp = last_slash; runp != path; --runp)
            if (runp[-1] != '/')
                break;

        /* The '/' is the last character, we have to look further.  */
        if (runp != path) {
            //last_slash = strrchr(path, '/', runp - path);ù
			printf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
		}
    }

    if (last_slash != NULL) {
        /* Determine whether all remaining characters are slashes.  */
        char *runp;

        for (runp = last_slash; runp != path; --runp)
            if (runp[-1] != '/')
                break;

        /* Terminate the path.  */
        if (runp == path) {
            /* The last slash is the first character in the string.  We have to
            return "/".  As a special case we have to return "//" if there
            are exactly two slashes at the beginning of the string.  See
            XBD 4.10 Path Name Resolution for more information.  */
            if (last_slash == path + 1)
                ++last_slash;
            else
                last_slash = path + 1;
        } else
            last_slash = runp;

        last_slash[0] = '\0';
    } else
        /* This assignment is ill-designed but the XPG specs require to
        return a string containing "." in any case no directory part is
        found and so a static and constant string is required.  */
        path = (char *) dot;

    return path;
}

/*
==============
Sys_FOpen
==============
*/
Q_FILE *Sys_FOpen(const char *ospath, const char *mode) {
    return Q_fopen(ospath, mode);
}

/*
==================
Sys_Mkdir
==================
*/
qboolean Sys_Mkdir(const char *path) {

    path_mkdir(path);
    return qtrue;
}

/*
==================
Sys_Mkfifo
==================
*/
Q_FILE *Sys_Mkfifo(const char *ospath) {
    return NULL;
}

/*
==================
Sys_Cwd
==================
*/
char *Sys_Cwd(void) {
    return DEFAULT_BASEDIR;
}

/*
==============================================================

DIRECTORY SCANNING

==============================================================
*/

#define MAX_FOUND_FILES 0x1000

/*
==================
Sys_ListFilteredFiles
==================
*/
void Sys_ListFilteredFiles(const char *basedir, char *subdirs, char *filter, char **list, int *numfiles) {
    char search[MAX_OSPATH], newsubdirs[MAX_OSPATH];
    char filename[MAX_OSPATH];
    RDIR	*fdir;

    if (*numfiles >= MAX_FOUND_FILES - 1) {
        return;
    }

    if (strlen(subdirs)) {
        Com_sprintf(search, sizeof(search), "%s/%s", basedir, subdirs);
    } else {
        Com_sprintf(search, sizeof(search), "%s", basedir);
    }

    if ((fdir = retro_opendir(search)) == NULL) {
        return;
    }

    while ((retro_readdir(fdir)) > 0) {
        Com_sprintf(filename, sizeof(filename), "%s/%s", search, retro_dirent_get_name(fdir));

        if (retro_dirent_is_dir(fdir, NULL)) {
            if (Q_stricmp(retro_dirent_get_name(fdir), ".") && Q_stricmp(retro_dirent_get_name(fdir), "..")) {
                if (strlen(subdirs)) {
                    Com_sprintf(newsubdirs, sizeof(newsubdirs), "%s/%s", subdirs, retro_dirent_get_name(fdir));
                } else {
                    Com_sprintf(newsubdirs, sizeof(newsubdirs), "%s", retro_dirent_get_name(fdir));
                }
                Sys_ListFilteredFiles(basedir, newsubdirs, filter, list, numfiles);
            }
        }
        if (*numfiles >= MAX_FOUND_FILES - 1) {
            break;
        }
        Com_sprintf(filename, sizeof(filename), "%s/%s", subdirs, retro_dirent_get_name(fdir));
        if (!Com_FilterPath(filter, filename, qfalse))
            continue;
        list[*numfiles] = CopyString(filename);
        (*numfiles)++;
    }

    retro_closedir(fdir);
}

/*
==================
Sys_ListFiles
==================
*/
char pak_fname[MAX_OSPATH];
char search[MAX_OSPATH];
char *list[MAX_FOUND_FILES];
RDIR	*fdir;
char **Sys_ListFiles(const char *directory, const char *extension, char *filter, int *numfiles, qboolean wantsubs) {
    
    qboolean dironly = wantsubs;
   
    int nfiles;
    char **listCopy;
    int i;

    int extLen;

    if (filter) {

        nfiles = 0;
        Sys_ListFilteredFiles(directory, "", filter, list, &nfiles);

        list[nfiles] = NULL;
        *numfiles = nfiles;

        if (!nfiles)
            return NULL;

        listCopy = Z_Malloc((nfiles + 1) * sizeof(*listCopy));
        for (i = 0; i < nfiles; i++) {
            listCopy[i] = list[i];
        }
        listCopy[i] = NULL;

        return listCopy;
    }

    if (!extension)
        extension = "";

    if (extension[0] == '/' && extension[1] == 0) {
        extension = "";
        dironly = qtrue;
    }

    extLen = strlen(extension);

    // search
    nfiles = 0;

	printf("Scanning %s folder\n", directory);
	
    if ((fdir = retro_opendir(directory)) == NULL) {
		printf("Failed to open %s folder\n", directory);
        *numfiles = 0;
        return NULL;
    }

    while ((retro_readdir(fdir)) > 0) {
		sprintf(pak_fname, retro_dirent_get_name(fdir));
        Com_sprintf(search, sizeof(search), "%s/%s", directory, pak_fname);
		printf("Detected %s file which %s a directory\n", search, retro_dirent_is_dir(fdir, NULL) ? "is" : "isn't");
        if ((dironly && !(retro_dirent_is_dir(fdir, NULL))) ||
            (!dironly && (retro_dirent_is_dir(fdir, NULL))))
            continue;

        if (*extension) {
			/* Extension match must be case-insensitive and anchored at the end
			 * of the name: Quake data ships as PAK0.PK3 on some installs, and a
			 * case-sensitive strstr() (matching ".pk3") rejected every uppercase
			 * ".PK3" pak, leaving the engine with no game data -> fatal exit.
			 * Anchoring also avoids false matches like "foo.pk3.bak". */
			int nameLen = (int)strlen(pak_fname);
			if (nameLen < extLen ||
			    Q_stricmpn(pak_fname + nameLen - extLen, extension, extLen) != 0) {
                continue; // didn't match
            }
        }

        if (nfiles == MAX_FOUND_FILES - 1)
            break;
		
		printf("%s is a good candidate\n", pak_fname);
        list[nfiles] = CopyString(pak_fname);
        nfiles++;
    }
	
	printf("Found %d files in %s folder\n", nfiles, directory);
	
    list[nfiles] = NULL;

    retro_closedir(fdir);

    // return a copy of the list
    *numfiles = nfiles;

    if (!nfiles) {
        return NULL;
    }

    listCopy = Z_Malloc((nfiles + 1) * sizeof(*listCopy));
    for (i = 0; i < nfiles; i++) {
        listCopy[i] = list[i];
    }
    listCopy[i] = NULL;

    return listCopy;
}

/*
==================
Sys_FreeFileList
==================
*/
void Sys_FreeFileList(char **list) {
    int i;

    if (!list) {
        return;
    }

    for (i = 0; list[i]; i++) {
        Z_Free(list[i]);
    }

    Z_Free(list);
}

/*
==================
Sys_Sleep

Block execution for msec or until input is received.
==================
*/
void Sys_Sleep(int msec) {
    if (msec == 0)
        return;
}

/*
==============
Sys_ErrorDialog

Display an error message
==============
*/
void Sys_ErrorDialog(const char *error) {
    const char *homepath = Cvar_VariableString("fs_homepath");
    const char *gamedir = Cvar_VariableString("fs_game");
    const char *fileName = "crashlog.txt";
    char *dirpath = FS_BuildOSPath(homepath, gamedir, "");

    Sys_Print(va("%s\n", error));

    // Make sure the write path for the crashlog exists...

    if (!Sys_Mkdir(homepath)) {
        Com_Printf("ERROR: couldn't create path '%s' for crash log.\n", homepath);
        return;
    }

    if (!Sys_Mkdir(dirpath)) {
        Com_Printf("ERROR: couldn't create path '%s' for crash log.\n", dirpath);
        return;
    }
}

/*
==============
Sys_GLimpSafeInit

Unix specific "safe" GL implementation initialisation
==============
*/
void Sys_GLimpSafeInit(void) {
    // NOP
}

/*
==============
Sys_GLimpInit

Unix specific GL implementation initialisation
==============
*/
void Sys_GLimpInit(void) {
    // NOP
}

void Sys_SetFloatEnv(void) {
    // rounding toward nearest
    // fesetround(FE_TONEAREST);
    // TODO: PSP2 ?
}

/*
==============
Sys_PlatformInit

Unix specific initialisation
==============
*/
void Sys_PlatformInit(void) {
}

/*
==============
Sys_PlatformExit

Unix specific deinitialisation
==============
*/
void Sys_PlatformExit(void) {
}

/*
==============
Sys_SetEnv

set/unset environment variables (empty value removes it)
==============
*/

void Sys_SetEnv(const char *name, const char *value) {
}

/*
=================
Sys_DllExtension

Check if filename should be allowed to be loaded as a DLL.
=================
*/
qboolean Sys_DllExtension(const char *name)
{
    const char *p;
    char c = 0;

    if (COM_CompareExtension(name, DLL_EXT))
        return qtrue;

    // Check for format of filename.so.1.2.3
    p = strstr(name, DLL_EXT ".");

    if (p)
    {
        p += strlen(DLL_EXT);

        // Check if .so is only followed for periods and numbers.
        while (*p)
	{
            c = *p;
            if (!isdigit(c) && c != '.')
                return qfalse;
            p++;
        }

        // Don't allow filename to end in a period. file.so., file.so.0., etc
        if (c != '.')
            return qtrue;
    }

    return qfalse;
}

/* snd_dma.c */

qboolean snd_inited = qfalse;
uint8_t psp2_inited = 0;

/*
===============
SNDDMA_Init
===============
*/
qboolean SNDDMA_Init(void)
{
	if (psp2_inited) return qtrue;
	psp2_inited = 1;
	dma.samplebits = 16;
	dma.speed = audio_sample_rate;
	dma.channels = 2;
	dma.samples = AUDIO_BUFFER_SIZE * 2;        /* mono samples in the buffer */
	dma.buffer = (byte *)audio_buffer;
	/* Stereo pairs the engine mixes per video frame; S_Update_ paints exactly
	 * this many into the linear buffer that audio_callback then pushes. */
	dma.samples_per_frame = audio_sample_rate / framerate;

	/* Wire the engine's float output to our linear float buffer; s_float_output
	 * is (re)asserted from the negotiation result so a reload picks up the
	 * current frontend's capability. */
	snd_float_buffer = audio_buffer_f;
	s_float_output   = use_float_output;
	memset(audio_buffer,   0, sizeof(audio_buffer));
	memset(audio_buffer_f, 0, sizeof(audio_buffer_f));

	snd_inited = qtrue;

	return qtrue;
}

/*
===============
SNDDMA_GetDMAPos

Frame-locked model: there is no DMA read cursor, but the engine's raw-stream
bookkeeping still queries position. Playback position is the monotonic paint
position (sample pairs), so report it in mono samples to match the legacy
contract (s_soundtime = GetDMAPos/channels).
===============
*/
int SNDDMA_GetDMAPos(void)
{
	return 0;
}

/*
===============
SNDDMA_Shutdown
===============
*/
void SNDDMA_Shutdown(void)
{
}

/*
===============
SNDDMA_Submit

Send sound to device if buffer isn't really the dma buffer
===============
*/
void SNDDMA_Submit(void)
{
}

/*
===============
SNDDMA_BeginPainting
===============
*/
void SNDDMA_BeginPainting (void)
{
}

void GLimp_Shutdown( void )
{
	ri.IN_Shutdown();
}

/*
===============
GLimp_Minimize

Minimize the game so that user is back at the desktop
===============
*/
void GLimp_Minimize( void )
{
}


/*
===============
GLimp_LogComment
===============
*/
void GLimp_LogComment( char *comment )
{
}

#define R_MODE_FALLBACK 3 // 960 * 544

/*
===============
GLimp_Init

This routine is responsible for initializing the OS specific portions
of OpenGL
===============
*/
uint8_t inited = 0;

typedef struct vidmode_s
{
	const char *description;
	int width, height;
	float pixelAspect;		// pixel width / height
} vidmode_t;
extern vidmode_t r_vidModes[];

uint32_t cur_width;

#ifdef RENDERER_GL2
/* GLimp_Init for the OpenGL 2 renderer is in glimp_gl2.c. */
void GLimp_EndFrame( void )
{
	video_cb(RETRO_HW_FRAME_BUFFER_VALID, scr_width, scr_height, 0);
}
#else
void GLimp_Init( qboolean coreContext)
{
	if (r_mode->integer < 0) r_mode->integer = 3;
	
	glConfig.vidWidth = scr_width;
	glConfig.vidHeight = scr_height;
	glConfig.colorBits = 32;
	glConfig.depthBits = 32;
	glConfig.stencilBits = 8;
	glConfig.displayFrequency = framerate;
	glConfig.stereoEnabled = qfalse;
	
	glConfig.driverType = GLDRV_ICD;
	glConfig.hardwareType = GLHW_GENERIC;
	glConfig.deviceSupportsGamma = qfalse;
	glConfig.textureCompression = TC_NONE;
	glConfig.textureEnvAddAvailable = qfalse;
	glConfig.windowAspect = ((float)scr_width) / ((float)scr_height);
	glConfig.isFullscreen = qtrue;
	
	if (!inited){
		inited = 1;
		cur_width = glConfig.vidWidth;
	}else if (glConfig.vidWidth != cur_width){ // Changed resolution in game, restarting the game

	}
	/* These batch buffers are persistent for the lifetime of the core. GLimp_Init
	 * is re-entered on every CL_Vid_Restart_f (context reset / resolution change),
	 * so allocate exactly once to avoid leaking ~6.5 MB per vid_restart. */
	if (!indices) {
		int j;
		indices = (uint16_t*)malloc(sizeof(uint16_t)*MAX_INDICES);
		for (j=0;j<MAX_INDICES;j++){
			indices[j] = j;
		}
	}
	qglEnableClientState(GL_VERTEX_ARRAY);
	if (!gVertexBufferPtr)
		gVertexBufferPtr = (float*)malloc(0x400000);
	if (!gColorBufferPtr)
		gColorBufferPtr = (uint8_t*)malloc(0x200000);
	if (!gTexCoordBufferPtr)
		gTexCoordBufferPtr = (float*)malloc(0x200000);
	if (!gColorBuffer255) {
		gColorBuffer255 = (uint8_t*)malloc(0x3000);
		memset(gColorBuffer255, 0xFF, 0x3000);
	}
	gVertexBuffer = gVertexBufferPtr;
	gColorBuffer = gColorBufferPtr;
	gTexCoordBuffer = gTexCoordBufferPtr;
	
	strncpy(glConfig.vendor_string, "unknown", sizeof(glConfig.vendor_string));
	strncpy(glConfig.renderer_string, "unknown", sizeof(glConfig.renderer_string));
	strncpy(glConfig.version_string, "unknown", sizeof(glConfig.version_string));
	strncpy(glConfig.extensions_string, "unknown", sizeof(glConfig.extensions_string));
	
	qglClearColor( 0, 0, 0, 1 );
	qglClear( GL_COLOR_BUFFER_BIT );
	
}


/*
===============
GLimp_EndFrame

Responsible for doing a swapbuffers
===============
*/
void GLimp_EndFrame( void )
{
   if (!libretro_shared_context)
      glsm_ctl(GLSM_CTL_STATE_UNBIND, NULL);
	video_cb(RETRO_HW_FRAME_BUFFER_VALID, scr_width, scr_height, 0);
   if (!libretro_shared_context)
      glsm_ctl(GLSM_CTL_STATE_BIND, NULL);
	gVertexBuffer   = gVertexBufferPtr;
	gColorBuffer    = gColorBufferPtr;
	gTexCoordBuffer = gTexCoordBufferPtr;
}
#endif

/* input.c */

/*
===============
IN_Frame
===============
*/
uint32_t oldkeys, oldanalogs;
void Key_Event(int key, int value, int time){
	Com_QueueEvent(time, SE_KEY, key, value, 0, NULL);
}

int16_t old_ret;
void Sys_SetKeys(int time){
	int port;

	if (!poll_cb)
		return;

	poll_cb();

	if (!input_cb)
		return;

	for (port = 0; port < MAX_PADS; port++)
	{
		if (!input_cb)
			break;

		switch (quake_devices[port])
		{
		case RETRO_DEVICE_JOYPAD:
		case RETRO_DEVICE_JOYPAD_ALT:
		case RETRO_DEVICE_MODERN:
		{
			unsigned i;
			int16_t ret    = 0;
			if (libretro_supports_bitmasks)
				ret = input_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
			else
			{
				for (i=RETRO_DEVICE_ID_JOYPAD_B; i <= RETRO_DEVICE_ID_JOYPAD_R3; ++i)
				{
					if (input_cb(port, RETRO_DEVICE_JOYPAD, 0, i))
						ret |= (1 << i);
				}
			}

			if ((ret & (1 << RETRO_DEVICE_ID_JOYPAD_UP)) && !(old_ret & (1 << RETRO_DEVICE_ID_JOYPAD_UP)))
				Key_Event(K_UPARROW, 1, time);
			else if (!(ret & (1 << RETRO_DEVICE_ID_JOYPAD_UP)) && (old_ret & (1 << RETRO_DEVICE_ID_JOYPAD_UP)))
				Key_Event(K_UPARROW, 0, time);
			if ((ret & (1 << RETRO_DEVICE_ID_JOYPAD_DOWN)) && !(old_ret & (1 << RETRO_DEVICE_ID_JOYPAD_DOWN)))
				Key_Event(K_DOWNARROW, 1, time);
			else if (!(ret & (1 << RETRO_DEVICE_ID_JOYPAD_DOWN)) && (old_ret & (1 << RETRO_DEVICE_ID_JOYPAD_DOWN)))
				Key_Event(K_DOWNARROW, 0, time);
			if ((ret & (1 << RETRO_DEVICE_ID_JOYPAD_LEFT)) && !(old_ret & (1 << RETRO_DEVICE_ID_JOYPAD_LEFT)))
				Key_Event(K_LEFTARROW, 1, time);
			else if (!(ret & (1 << RETRO_DEVICE_ID_JOYPAD_LEFT)) && (old_ret & (1 << RETRO_DEVICE_ID_JOYPAD_LEFT)))
				Key_Event(K_LEFTARROW, 0, time);
			if ((ret & (1 << RETRO_DEVICE_ID_JOYPAD_RIGHT)) && !(old_ret & (1 << RETRO_DEVICE_ID_JOYPAD_RIGHT)))
				Key_Event(K_RIGHTARROW, 1, time);
			else if (!(ret & (1 << RETRO_DEVICE_ID_JOYPAD_RIGHT)) && (old_ret & (1 << RETRO_DEVICE_ID_JOYPAD_RIGHT)))
				Key_Event(K_RIGHTARROW, 0, time);
			if ((ret & (1 << RETRO_DEVICE_ID_JOYPAD_START)) && !(old_ret & (1 << RETRO_DEVICE_ID_JOYPAD_START)))
				Key_Event(K_ESCAPE, 1, time);
			else if (!(ret & (1 << RETRO_DEVICE_ID_JOYPAD_START)) && (old_ret & (1 << RETRO_DEVICE_ID_JOYPAD_START)))
				Key_Event(K_ESCAPE, 0, time);
			if ((ret & (1 << RETRO_DEVICE_ID_JOYPAD_SELECT)) && !(old_ret & (1 << RETRO_DEVICE_ID_JOYPAD_SELECT)))
				Key_Event(K_ENTER, 1, time);
			else if (!(ret & (1 << RETRO_DEVICE_ID_JOYPAD_SELECT)) && (old_ret & (1 << RETRO_DEVICE_ID_JOYPAD_SELECT)))
				Key_Event(K_ENTER, 0, time);
			if ((ret & (1 << RETRO_DEVICE_ID_JOYPAD_Y)) && !(old_ret & (1 << RETRO_DEVICE_ID_JOYPAD_Y)))
				Key_Event(K_AUX3, 1, time);
			else if (!(ret & (1 << RETRO_DEVICE_ID_JOYPAD_Y)) && (old_ret & (1 << RETRO_DEVICE_ID_JOYPAD_Y)))
				Key_Event(K_AUX3, 0, time);
			if ((ret & (1 << RETRO_DEVICE_ID_JOYPAD_X)) && !(old_ret & (1 << RETRO_DEVICE_ID_JOYPAD_X)))
				Key_Event(K_AUX4, 1, time);
			else if (!(ret & (1 << RETRO_DEVICE_ID_JOYPAD_X)) && (old_ret & (1 << RETRO_DEVICE_ID_JOYPAD_X)))
				Key_Event(K_AUX4, 0, time);
			if ((ret & (1 << RETRO_DEVICE_ID_JOYPAD_B)) && !(old_ret & (1 << RETRO_DEVICE_ID_JOYPAD_B)))
				Key_Event(K_AUX1, 1, time);
			else if (!(ret & (1 << RETRO_DEVICE_ID_JOYPAD_B)) && (old_ret & (1 << RETRO_DEVICE_ID_JOYPAD_B)))
				Key_Event(K_AUX1, 0, time);
			if ((ret & (1 << RETRO_DEVICE_ID_JOYPAD_A)) && !(old_ret & (1 << RETRO_DEVICE_ID_JOYPAD_A)))
				Key_Event(K_AUX2, 1, time);
			else if (!(ret & (1 << RETRO_DEVICE_ID_JOYPAD_A)) && (old_ret & (1 << RETRO_DEVICE_ID_JOYPAD_A)))
				Key_Event(K_AUX2, 0, time);
			if ((ret & (1 << RETRO_DEVICE_ID_JOYPAD_L)) && !(old_ret & (1 << RETRO_DEVICE_ID_JOYPAD_L)))
				Key_Event(K_AUX5, 1, time);
			else if (!(ret & (1 << RETRO_DEVICE_ID_JOYPAD_L)) && (old_ret & (1 << RETRO_DEVICE_ID_JOYPAD_L)))
				Key_Event(K_AUX5, 0, time);
			if ((ret & (1 << RETRO_DEVICE_ID_JOYPAD_R)) && !(old_ret & (1 << RETRO_DEVICE_ID_JOYPAD_R)))
				Key_Event(K_AUX6, 1, time);
			else if (!(ret & (1 << RETRO_DEVICE_ID_JOYPAD_R)) && (old_ret & (1 << RETRO_DEVICE_ID_JOYPAD_R)))
				Key_Event(K_AUX6, 0, time);
			
			old_ret = ret;
		}
		break;
		case RETRO_DEVICE_KEYBOARD:
		{
			/* RetroKeyboard + Mouse. The keyboard is handled event-driven by
			 * keyboard_cb (RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK); only the
			 * mouse is polled here. Left/right buttons -> fire / alt-fire; wheel
			 * up/down -> K_MWHEELUP/DOWN, which the config binds to
			 * weapprev/weapnext (cycle weapon). Edge-detected so each press and
			 * release emit one event; the wheel's momentary one-frame signal
			 * cycles once on the down edge. */
			static const struct { unsigned id; int qkey; } mouse_btn[] = {
				{ RETRO_DEVICE_ID_MOUSE_LEFT,      K_MOUSE1     },
				{ RETRO_DEVICE_ID_MOUSE_RIGHT,     K_MOUSE2     },
				{ RETRO_DEVICE_ID_MOUSE_WHEELUP,   K_MWHEELUP   },
				{ RETRO_DEVICE_ID_MOUSE_WHEELDOWN, K_MWHEELDOWN },
			};
			static int mouse_prev[sizeof(mouse_btn) / sizeof(mouse_btn[0])];
			unsigned k;
			for (k = 0; k < sizeof(mouse_btn) / sizeof(mouse_btn[0]); k++)
			{
				int down = input_cb(port, RETRO_DEVICE_MOUSE, 0, mouse_btn[k].id) ? 1 : 0;
				if (down != mouse_prev[k])
				{
					Key_Event(mouse_btn[k].qkey, down, time);
					mouse_prev[k] = down;
				}
			}
		}
		break;
		case RETRO_DEVICE_NONE:
			break;
		}
	}
}

// Left analog virtual values
#define LANALOG_LEFT  0x01
#define LANALOG_RIGHT 0x02
#define LANALOG_UP    0x04
#define LANALOG_DOWN  0x08

int old_x = - 1, old_y;

void IN_Frame( void )
{
	int time = com_frameTime;
	Sys_SetKeys(time);
	int lsx, lsy, rsx, rsy;
	
	uint32_t virt_buttons = 0x00;
	
	if (quake_devices[0] != RETRO_DEVICE_NONE && quake_devices[0] != RETRO_DEVICE_KEYBOARD) {
      // Emulating keys with left analog (TODO: Replace this dirty hack with a serious implementation)
      lsx = input_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
               RETRO_DEVICE_ID_ANALOG_X);
      lsy = input_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
               RETRO_DEVICE_ID_ANALOG_Y);

      if (lsx > analog_deadzone || lsx < -analog_deadzone) {
         if (lsx > analog_deadzone)
            virt_buttons += LANALOG_RIGHT;
         if (lsx < -analog_deadzone)
            virt_buttons += LANALOG_LEFT;
	  }
	  
	  if (lsy > analog_deadzone || lsy < -analog_deadzone) {
         if (lsy > analog_deadzone)
            virt_buttons += LANALOG_UP;
         if (lsy < -analog_deadzone)
            virt_buttons += LANALOG_DOWN;
      }
	
	  if (virt_buttons != oldanalogs){
		if((virt_buttons & LANALOG_LEFT) != (oldanalogs & LANALOG_LEFT))
			Key_Event(K_AUX7, (virt_buttons & LANALOG_LEFT) == LANALOG_LEFT, time);
		if((virt_buttons & LANALOG_RIGHT) != (oldanalogs & LANALOG_RIGHT))
			Key_Event(K_AUX8, (virt_buttons & LANALOG_RIGHT) == LANALOG_RIGHT, time);
		if((virt_buttons & LANALOG_UP) != (oldanalogs & LANALOG_UP))
			Key_Event(K_AUX10, (virt_buttons & LANALOG_UP) == LANALOG_UP, time);
		if((virt_buttons & LANALOG_DOWN) != (oldanalogs & LANALOG_DOWN))
			Key_Event(K_AUX9, (virt_buttons & LANALOG_DOWN) == LANALOG_DOWN, time);
	  }
	  oldanalogs = virt_buttons;
	  
	  int slowdown = 1024 * (framerate / 60.0f);
	  
	  // Right stick Look
      rsx = input_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
               RETRO_DEVICE_ID_ANALOG_X);
      rsy = invert_y_axis * input_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
               RETRO_DEVICE_ID_ANALOG_Y);
			   
	  if (rsx > analog_deadzone || rsx < -analog_deadzone) {
         if (rsx > analog_deadzone)
            rsx = rsx - analog_deadzone;
         if (rsx < -analog_deadzone)
            rsx = rsx + analog_deadzone;
		
      } else rsx = 0;
	  if (rsy > analog_deadzone || rsy < -analog_deadzone) {
         if (rsy > analog_deadzone)
            rsy = rsy - analog_deadzone;
         if (rsy < -analog_deadzone)
            rsy = rsy + analog_deadzone;
      } else rsy = 0;
	  Com_QueueEvent(time, SE_MOUSE, rsx / slowdown, rsy / slowdown, 0, NULL);
	}

	/* RetroKeyboard + Mouse: feed relative mouse motion as look. Deltas are
	 * pixel counts; the engine applies cl_sensitivity, so pass them through
	 * directly (same SE_MOUSE channel the analog stick uses above). */
	if (quake_devices[0] == RETRO_DEVICE_KEYBOARD)
	{
		int mdx = input_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X);
		int mdy = input_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y);
		if (mdx || mdy)
			Com_QueueEvent(time, SE_MOUSE, mdx, mdy, 0, NULL);
	}

}

/*
===============
IN_Init
===============
*/
void IN_Init( void *windowData )
{
}

/*
===============
IN_Shutdown
===============
*/
void IN_Shutdown( void )
{
}

/*
===============
IN_Restart
===============
*/
void IN_Restart( void )
{
}

/* net.c */
#if !defined(SWITCH) && !defined(__APPLE__)
typedef unsigned short	sa_family_t;
#endif

#ifndef SOCKET_ERROR
typedef int SOCKET;
#	define INVALID_SOCKET		-1
#	define SOCKET_ERROR			-1
#endif

#	define closesocket			close
#	define ioctlsocket			ioctl
typedef int	ioctlarg_t;
#ifdef _WIN32
#	define socketError		WSAGetLastError( )
#else
#	define socketError			errno
#endif
static qboolean usingSocks = qfalse;
static int networkingEnabled = 0;

static cvar_t	*net_enabled;

static cvar_t	*net_socksEnabled;
static cvar_t	*net_socksServer;
static cvar_t	*net_socksPort;
static cvar_t	*net_socksUsername;
static cvar_t	*net_socksPassword;

static cvar_t	*net_ip;
static cvar_t	*net_port;

static cvar_t	*net_dropsim;

static struct sockaddr	socksRelayAddr;

static SOCKET	ip_socket = INVALID_SOCKET;
static SOCKET	socks_socket = INVALID_SOCKET;

#ifndef IF_NAMESIZE
#define IF_NAMESIZE 16
#endif

// use an admin local address per default so that network admins can decide on how to handle quake3 traffic.
#define NET_MULTICAST_IP6 "ff04::696f:7175:616b:6533"

#define	MAX_IPS		32

typedef struct
{
	char ifname[IF_NAMESIZE];

	netadrtype_t type;
	sa_family_t family;
	struct sockaddr_storage addr;
	struct sockaddr_storage netmask;
} nip_localaddr_t;

static nip_localaddr_t localIP[MAX_IPS];
static int numIP;


//=============================================================================
#undef Com_Printf
#define Com_Printf printf
/*
====================
NET_ErrorString
====================
*/
char error[256];
char *NET_ErrorString( void ) {
	sprintf(error, "sceNet returned 0x%08X", socketError);
	return error;
}

static void NetadrToSockadr( netadr_t *a, struct sockaddr *s ) {
	if( a->type == NA_BROADCAST ) {
		((struct sockaddr_in *)s)->sin_family = AF_INET;
		((struct sockaddr_in *)s)->sin_port = a->port;
		((struct sockaddr_in *)s)->sin_addr.s_addr = INADDR_BROADCAST;
	}
	else if( a->type == NA_IP ) {
		((struct sockaddr_in *)s)->sin_family = AF_INET;
		((struct sockaddr_in *)s)->sin_addr.s_addr = *(int *)&a->ip;
		((struct sockaddr_in *)s)->sin_port = a->port;
	}
}


static void SockadrToNetadr( struct sockaddr *s, netadr_t *a ) {
	if (s->sa_family == AF_INET) {
		a->type = NA_IP;
		*(int *)&a->ip = ((struct sockaddr_in *)s)->sin_addr.s_addr;
		a->port = ((struct sockaddr_in *)s)->sin_port;
	}
}


static struct addrinfo *SearchAddrInfo(struct addrinfo *hints, sa_family_t family)
{
	while(hints)
	{
		if(hints->ai_family == family)
			return hints;

		hints = hints->ai_next;
	}

	return NULL;
}

/*
=============
Sys_StringToSockaddr
=============
*/
static qboolean Sys_StringToSockaddr(const char *s, struct sockaddr *sadr, int sadr_len, sa_family_t family)
{
	struct addrinfo hints;
	struct addrinfo *res = NULL;
	struct addrinfo *search = NULL;
	struct addrinfo *hintsp;
	int retval;
	
	memset(sadr, '\0', sizeof(*sadr));
	memset(&hints, '\0', sizeof(hints));

	hintsp = &hints;
#ifdef __SWITCH__
	if (family == AF_UNSPEC) family = AF_INET;
	hintsp->ai_flags = AI_ADDRCONFIG;
#endif
	hintsp->ai_family = family;
	hintsp->ai_socktype = SOCK_DGRAM;
#ifdef __SWITCH__
	// Since libnx is a smelling pile of shit, we need this
	retval = getaddrinfo(s, "1234", hintsp, &res);
#else
	retval = getaddrinfo_retro(s, NULL, hintsp, &res);
#endif

	if(!retval)
	{
		if(family == AF_UNSPEC)
		{
			// Decide here and now which protocol family to use
			if(net_enabled->integer & NET_PRIOV6)
			{
				if(net_enabled->integer & NET_ENABLEV6)
					search = SearchAddrInfo(res, AF_INET6);
				
				if(!search && (net_enabled->integer & NET_ENABLEV4))
					search = SearchAddrInfo(res, AF_INET);
			}
			else
			{
				if(net_enabled->integer & NET_ENABLEV4)
					search = SearchAddrInfo(res, AF_INET);
				
				if(!search && (net_enabled->integer & NET_ENABLEV6))
					search = SearchAddrInfo(res, AF_INET6);
			}
		}
		else
			search = SearchAddrInfo(res, family);

		if(search)
		{
			if(search->ai_addrlen > sadr_len)
				search->ai_addrlen = sadr_len;
				
			memcpy(sadr, search->ai_addr, search->ai_addrlen);
			freeaddrinfo_retro(res);
			
			return qtrue;
		}
		else
			Com_Printf("Sys_StringToSockaddr: Error resolving %s: No address of required type found.\n", s);
	}
	else
		Com_Printf("Sys_StringToSockaddr: Error resolving %s: %s (errorcode: %d)\n", s, gai_strerror(retval), retval);
	
	if(res)
		freeaddrinfo_retro(res);
	
	return qfalse;
}

/*
=============
Sys_SockaddrToString
=============
*/
static void Sys_SockaddrToString(char *dest, int destlen, struct sockaddr *input)
{
	socklen_t inputlen;
#ifndef SWITCH
	if (input->sa_family == AF_INET6)
		inputlen = sizeof(struct sockaddr_in6);
	else
#endif
		inputlen = sizeof(struct sockaddr_in);

	if(getnameinfo(input, inputlen, dest, destlen, NULL, 0, NI_NUMERICHOST) && destlen > 0)
		*dest = '\0';
}

/*
=============
Sys_StringToAdr
=============
*/
qboolean Sys_StringToAdr( const char *s, netadr_t *a, netadrtype_t family ) {
	char *p1 = strstr(s, ":");
	char *p2;
	if (p1) {
		p2 = strstr(p1 + 1, ":");
		if (p2) p2[0] = 0;
	}
	struct sockaddr_storage sadr;
	sa_family_t fam;

	switch(family)
	{
		case NA_IP:
			fam = AF_INET;
			break;
		default:
			fam = AF_UNSPEC;
			break;
	}
	if( !Sys_StringToSockaddr(s, (struct sockaddr *) &sadr, sizeof(sadr), fam ) ) {
		return qfalse;
	}

	SockadrToNetadr( (struct sockaddr *) &sadr, a );
	return qtrue;
}

/*
===================
NET_CompareBaseAdrMask
Compare without port, and up to the bit number given in netmask.
===================
*/
qboolean NET_CompareBaseAdrMask(netadr_t a, netadr_t b, int netmask)
{
	byte cmpmask, *addra, *addrb;
	int curbyte;

	if (a.type != b.type)
		return qfalse;

	if (a.type == NA_LOOPBACK)
		return qtrue;

	if(a.type == NA_IP)
	{
		addra = (byte *) &a.ip;
		addrb = (byte *) &b.ip;

		if(netmask < 0 || netmask > 32)
			netmask = 32;
	}
	else
	{
		Com_Printf ("NET_CompareBaseAdr: bad address type\n");
		return qfalse;
	}

	curbyte = netmask >> 3;

	if(curbyte && memcmp(addra, addrb, curbyte))
		return qfalse;

	netmask &= 0x07;
	if(netmask)
	{
		cmpmask = (1 << netmask) - 1;
		cmpmask <<= 8 - netmask;

		if((addra[curbyte] & cmpmask) == (addrb[curbyte] & cmpmask))
			return qtrue;
	}
	else
		return qtrue;

	return qfalse;
}


/*
===================
NET_CompareBaseAdr
Compares without the port
===================
*/
qboolean NET_CompareBaseAdr (netadr_t a, netadr_t b)
{
	return NET_CompareBaseAdrMask(a, b, -1);
}

const char	*NET_AdrToString (netadr_t a)
{
	static	char	s[NET_ADDRSTRMAXLEN];

	if (a.type == NA_LOOPBACK)
		Com_sprintf (s, sizeof(s), "loopback");
	else if (a.type == NA_BOT)
		Com_sprintf (s, sizeof(s), "bot");
	else if (a.type == NA_IP)
	{
		struct sockaddr_storage sadr;

		memset(&sadr, 0, sizeof(sadr));
		NetadrToSockadr(&a, (struct sockaddr *) &sadr);
		Sys_SockaddrToString(s, sizeof(s), (struct sockaddr *) &sadr);
	}

	return s;
}

const char	*NET_AdrToStringwPort (netadr_t a)
{
	static	char	s[NET_ADDRSTRMAXLEN];

	if (a.type == NA_LOOPBACK)
		Com_sprintf (s, sizeof(s), "loopback");
	else if (a.type == NA_BOT)
		Com_sprintf (s, sizeof(s), "bot");
	else if(a.type == NA_IP)
		Com_sprintf(s, sizeof(s), "%s:%hu", NET_AdrToString(a), ntohs(a.port));

	return s;
}


qboolean	NET_CompareAdr (netadr_t a, netadr_t b)
{
	if(!NET_CompareBaseAdr(a, b))
		return qfalse;

	if (a.type == NA_IP)
	{
		if (a.port == b.port)
			return qtrue;
	}
	else
		return qtrue;

	return qfalse;
}


qboolean	NET_IsLocalAddress( netadr_t adr ) {
	return adr.type == NA_LOOPBACK;
}

//=============================================================================

/*
==================
NET_GetPacket
Receive one packet
==================
*/
qboolean NET_GetPacket(netadr_t *net_from, msg_t *net_message, fd_set *fdr)
{
	int 	ret;
	struct sockaddr_storage from;
	socklen_t	fromlen;
	int		err;

	if(ip_socket != INVALID_SOCKET && FD_ISSET(ip_socket, fdr))
	{
		fromlen = sizeof(from);
		ret = recvfrom( ip_socket, (void *)net_message->data, net_message->maxsize, 0, (struct sockaddr *) &from, &fromlen );

		if (ret == SOCKET_ERROR)
		{
			err = socketError;

			if( !isagain(ret) && err != ECONNRESET)
				Com_Printf( "NET_GetPacket: %s\n", NET_ErrorString() );
		}
		else
		{

			memset( ((struct sockaddr_in *)&from)->sin_zero, 0, 8 );

			if ( usingSocks && memcmp( &from, &socksRelayAddr, fromlen ) == 0 ) {
				if ( ret < 10 || net_message->data[0] != 0 || net_message->data[1] != 0 || net_message->data[2] != 0 || net_message->data[3] != 1 ) {
					return qfalse;
				}
				net_from->type = NA_IP;
				net_from->ip[0] = net_message->data[4];
				net_from->ip[1] = net_message->data[5];
				net_from->ip[2] = net_message->data[6];
				net_from->ip[3] = net_message->data[7];
				net_from->port = *(short *)&net_message->data[8];
				net_message->readcount = 10;
			}
			else {
				SockadrToNetadr( (struct sockaddr *) &from, net_from );
				net_message->readcount = 0;
			}

			if( ret >= net_message->maxsize ) {
				Com_Printf( "Oversize packet from %s\n", NET_AdrToString (*net_from) );
				return qfalse;
			}

			net_message->cursize = ret;
			return qtrue;
		}
	}

	return qfalse;
}

//=============================================================================

static char socksBuf[4096];

/*
==================
Sys_SendPacket
==================
*/
void Sys_SendPacket( int length, const void *data, netadr_t to ) {
	int				ret = SOCKET_ERROR;
	struct sockaddr_storage	addr;

	if( to.type != NA_BROADCAST && to.type != NA_IP && to.type != NA_IP6 && to.type != NA_MULTICAST6)
	{
		Com_Error( ERR_FATAL, "Sys_SendPacket: bad address type" );
		return;
	}

	if( (ip_socket == INVALID_SOCKET && to.type == NA_IP) ||
		(ip_socket == INVALID_SOCKET && to.type == NA_BROADCAST) )
		return;

	if(to.type == NA_MULTICAST6 && (net_enabled->integer & NET_DISABLEMCAST))
		return;

	memset(&addr, 0, sizeof(addr));
	NetadrToSockadr( &to, (struct sockaddr *) &addr );

	if( usingSocks && to.type == NA_IP ) {
		socksBuf[0] = 0;	// reserved
		socksBuf[1] = 0;
		socksBuf[2] = 0;	// fragment (not fragmented)
		socksBuf[3] = 1;	// address type: IPV4
		*(int *)&socksBuf[4] = ((struct sockaddr_in *)&addr)->sin_addr.s_addr;
		*(short *)&socksBuf[8] = ((struct sockaddr_in *)&addr)->sin_port;
		memcpy( &socksBuf[10], data, length );
		ret = sendto( ip_socket, socksBuf, length+10, 0, &socksRelayAddr, sizeof(socksRelayAddr) );
	}
	else {
		if(addr.ss_family == AF_INET)
			ret = sendto( ip_socket, data, length, 0, (struct sockaddr *) &addr, sizeof(struct sockaddr_in) );
	}
	if( ret == SOCKET_ERROR ) {
		int err = socketError;

		// wouldblock is silent
		if(isagain(ret)) {
			return;
		}

		// some PPP links do not allow broadcasts and return an error
		if( ( err == EADDRNOTAVAIL ) && ( ( to.type == NA_BROADCAST ) ) ) {
			return;
		}

		Com_Printf( "Sys_SendPacket: %s\n", NET_ErrorString() );
	}
}


//=============================================================================

/*
==================
Sys_IsLANAddress
LAN clients will have their rate var ignored
==================
*/
qboolean Sys_IsLANAddress( netadr_t adr ) {
	int		index, run, addrsize;
	qboolean differed;
	byte *compareadr, *comparemask, *compareip;

	if( adr.type == NA_LOOPBACK ) {
		return qtrue;
	}

	if( adr.type == NA_IP )
	{
		// RFC1918:
		// 10.0.0.0        -   10.255.255.255  (10/8 prefix)
		// 172.16.0.0      -   172.31.255.255  (172.16/12 prefix)
		// 192.168.0.0     -   192.168.255.255 (192.168/16 prefix)
		if(adr.ip[0] == 10)
			return qtrue;
		if(adr.ip[0] == 172 && (adr.ip[1]&0xf0) == 16)
			return qtrue;
		if(adr.ip[0] == 192 && adr.ip[1] == 168)
			return qtrue;

		if(adr.ip[0] == 127)
			return qtrue;
	}

	// Now compare against the networks this computer is member of.
	for(index = 0; index < numIP; index++)
	{
		if(localIP[index].type == adr.type)
		{
			if(adr.type == NA_IP)
			{
				compareip = (byte *) &((struct sockaddr_in *) &localIP[index].addr)->sin_addr.s_addr;
				comparemask = (byte *) &((struct sockaddr_in *) &localIP[index].netmask)->sin_addr.s_addr;
				compareadr = adr.ip;

				addrsize = sizeof(adr.ip);
			}

			differed = qfalse;
			for(run = 0; run < addrsize; run++)
			{
				if((compareip[run] & comparemask[run]) != (compareadr[run] & comparemask[run]))
				{
					differed = qtrue;
					break;
				}
			}

			if(!differed)
				return qtrue;

		}
	}

	return qfalse;
}

/*
==================
Sys_ShowIP
==================
*/
void Sys_ShowIP(void) {
	int i;
	char addrbuf[NET_ADDRSTRMAXLEN];

	for(i = 0; i < numIP; i++)
	{
		Sys_SockaddrToString(addrbuf, sizeof(addrbuf), (struct sockaddr *) &localIP[i].addr);

		if(localIP[i].type == NA_IP)
			Com_Printf( "IP: %s\n", addrbuf);
	}
}


//=============================================================================


/*
====================
NET_IPSocket
====================
*/
SOCKET NET_IPSocket( char *net_interface, int port, int *err ) {

	SOCKET				newsocket;
	struct sockaddr_in	address;
	int					i = 1;

	*err = 0;

	if( net_interface ) {
		Com_Printf( "Opening IP socket: %s:%i\n", net_interface, port );
	}
	else {
		Com_Printf( "Opening IP socket: 0.0.0.0:%i\n", port );
	}

	if( ( newsocket = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP ) ) == INVALID_SOCKET ) {
		*err = socketError;
		Com_Printf( "WARNING: NET_IPSocket: socket: %s\n", NET_ErrorString() );
		return newsocket;
	}
	
	// make it non-blocking
	socket_nonblock(newsocket);

	// make it broadcast capable
	if( setsockopt( newsocket, SOL_SOCKET, SO_BROADCAST, (char *) &i, sizeof(i) ) == SOCKET_ERROR ) {
		Com_Printf( "WARNING: NET_IPSocket: setsockopt SO_BROADCAST: %s\n", NET_ErrorString() );
	}

	if( !net_interface || !net_interface[0]) {
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = INADDR_ANY;
	}
	else
	{
		if(!Sys_StringToSockaddr( net_interface, (struct sockaddr *)&address, sizeof(address), AF_INET))
		{
			closesocket(newsocket);
			return INVALID_SOCKET;
		}
	}

	if( port == PORT_ANY ) {
		address.sin_port = 0;
	}
	else {
		address.sin_port = htons( (short)port );
	}

	if( bind( newsocket, (void *)&address, sizeof(address) ) == SOCKET_ERROR ) {
		Com_Printf( "WARNING: NET_IPSocket: bind: %s\n", NET_ErrorString() );
		*err = socketError;
		closesocket( newsocket );
		return INVALID_SOCKET;
	}

	return newsocket;
}

/*
====================
NET_OpenSocks
====================
*/
void NET_OpenSocks( int port ) {
	struct sockaddr_in	address;
	struct hostent		*h;
	int					len;
	qboolean			rfc1929;
	unsigned char		buf[64];

	usingSocks = qfalse;

	Com_Printf( "Opening connection to SOCKS server.\n" );

	if ( ( socks_socket = socket( AF_INET, SOCK_STREAM, IPPROTO_TCP ) ) == INVALID_SOCKET ) {
		Com_Printf( "WARNING: NET_OpenSocks: socket: %s\n", NET_ErrorString() );
		return;
	}
	
	h = gethostbyname( net_socksServer->string );
	if ( h == NULL ) {
		Com_Printf( "WARNING: NET_OpenSocks: gethostbyname: %s\n", NET_ErrorString() );
		return;
	}
	if ( h->h_addrtype != AF_INET ) {
		Com_Printf( "WARNING: NET_OpenSocks: gethostbyname: address type was not AF_INET\n" );
		return;
	}
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = *(int *)h->h_addr_list[0];
	address.sin_port = htons( (short)net_socksPort->integer );

	if ( connect( socks_socket, (struct sockaddr *)&address, sizeof( address ) ) == SOCKET_ERROR ) {
		Com_Printf( "NET_OpenSocks: connect: %s\n", NET_ErrorString() );
		return;
	}

	// send socks authentication handshake
	if ( *net_socksUsername->string || *net_socksPassword->string ) {
		rfc1929 = qtrue;
	}
	else {
		rfc1929 = qfalse;
	}

	buf[0] = 5;		// SOCKS version
	// method count
	if ( rfc1929 ) {
		buf[1] = 2;
		len = 4;
	}
	else {
		buf[1] = 1;
		len = 3;
	}
	buf[2] = 0;		// method #1 - method id #00: no authentication
	if ( rfc1929 ) {
		buf[2] = 2;		// method #2 - method id #02: username/password
	}
	if ( send( socks_socket, (void *)buf, len, 0 ) == SOCKET_ERROR ) {
		Com_Printf( "NET_OpenSocks: send: %s\n", NET_ErrorString() );
		return;
	}

	// get the response
	len = recv( socks_socket, (void *)buf, 64, 0 );
	if ( len == SOCKET_ERROR ) {
		Com_Printf( "NET_OpenSocks: recv: %s\n", NET_ErrorString() );
		return;
	}
	if ( len != 2 || buf[0] != 5 ) {
		Com_Printf( "NET_OpenSocks: bad response\n" );
		return;
	}
	switch( buf[1] ) {
		case 0:	// no authentication
			break;
		case 2: // username/password authentication
			break;
		default:
			Com_Printf( "NET_OpenSocks: request denied\n" );
			return;
	}

	// do username/password authentication if needed
	if ( buf[1] == 2 ) {
		int		ulen;
		int		plen;

		// build the request
		ulen = strlen( net_socksUsername->string );
		plen = strlen( net_socksPassword->string );

		buf[0] = 1;		// username/password authentication version
		buf[1] = ulen;
		if ( ulen ) {
			memcpy( &buf[2], net_socksUsername->string, ulen );
		}
		buf[2 + ulen] = plen;
		if ( plen ) {
			memcpy( &buf[3 + ulen], net_socksPassword->string, plen );
		}

		// send it
		if ( send( socks_socket, (void *)buf, 3 + ulen + plen, 0 ) == SOCKET_ERROR ) {
			Com_Printf( "NET_OpenSocks: send: %s\n", NET_ErrorString() );
			return;
		}

		// get the response
		len = recv( socks_socket, (void *)buf, 64, 0 );
		if ( len == SOCKET_ERROR ) {
			Com_Printf( "NET_OpenSocks: recv: %s\n", NET_ErrorString() );
			return;
		}
		if ( len != 2 || buf[0] != 1 ) {
			Com_Printf( "NET_OpenSocks: bad response\n" );
			return;
		}
		if ( buf[1] != 0 ) {
			Com_Printf( "NET_OpenSocks: authentication failed\n" );
			return;
		}
	}

	// send the UDP associate request
	buf[0] = 5;		// SOCKS version
	buf[1] = 3;		// command: UDP associate
	buf[2] = 0;		// reserved
	buf[3] = 1;		// address type: IPV4
	*(int *)&buf[4] = INADDR_ANY;
	*(short *)&buf[8] = htons( (short)port );		// port
	if ( send( socks_socket, (void *)buf, 10, 0 ) == SOCKET_ERROR ) {
		Com_Printf( "NET_OpenSocks: send: %s\n", NET_ErrorString() );
		return;
	}

	// get the response
	len = recv( socks_socket, (void *)buf, 64, 0 );
	if( len == SOCKET_ERROR ) {
		Com_Printf( "NET_OpenSocks: recv: %s\n", NET_ErrorString() );
		return;
	}
	if( len < 2 || buf[0] != 5 ) {
		Com_Printf( "NET_OpenSocks: bad response\n" );
		return;
	}
	// check completion code
	if( buf[1] != 0 ) {
		Com_Printf( "NET_OpenSocks: request denied: %i\n", buf[1] );
		return;
	}
	if( buf[3] != 1 ) {
		Com_Printf( "NET_OpenSocks: relay address is not IPV4: %i\n", buf[3] );
		return;
	}
	((struct sockaddr_in *)&socksRelayAddr)->sin_family = AF_INET;
	((struct sockaddr_in *)&socksRelayAddr)->sin_addr.s_addr = *(int *)&buf[4];
	((struct sockaddr_in *)&socksRelayAddr)->sin_port = *(short *)&buf[8];
	memset( ((struct sockaddr_in *)&socksRelayAddr)->sin_zero, 0, 8 );

	usingSocks = qtrue;
}


/*
=====================
NET_AddLocalAddress
=====================
*/
static void NET_AddLocalAddress(char *ifname, struct sockaddr *addr, struct sockaddr *netmask)
{
	int addrlen;
	sa_family_t family;

	// only add addresses that have all required info.
	if(!addr || !netmask || !ifname)
		return;

	family = addr->sa_family;

	if(numIP < MAX_IPS)
	{
		if(family == AF_INET)
		{
			addrlen = sizeof(struct sockaddr_in);
			localIP[numIP].type = NA_IP;
		}
		else
			return;

		Q_strncpyz(localIP[numIP].ifname, ifname, sizeof(localIP[numIP].ifname));

		localIP[numIP].family = family;

		memcpy(&localIP[numIP].addr, addr, addrlen);
		memcpy(&localIP[numIP].netmask, netmask, addrlen);

		numIP++;
	}
}

static unsigned long myAddr;
static void NET_GetLocalAddress( void ) {
	char				hostname[256];
	struct addrinfo	hint;
	struct addrinfo	*res = NULL;

	numIP = 0;

	memset(&hint, 0, sizeof(hint));

	hint.ai_family = AF_UNSPEC;
	hint.ai_socktype = SOCK_DGRAM;

	
	
	if(!getaddrinfo_retro(hostname, NULL, &hint, &res))
	{
		struct sockaddr_in mask4;
		struct sockaddr_in addr;
		addr.sin_family = AF_INET;
		addr.sin_port = 5000;
		addr.sin_addr.s_addr = myAddr;
		
		/* On operating systems where it's more difficult to find out the configured interfaces, we'll just assume a
		 * netmask with all bits set. */

		memset(&mask4, 0, sizeof(mask4));
		mask4.sin_family = AF_INET;
		memset(&mask4.sin_addr.s_addr, 0xFF, sizeof(mask4.sin_addr.s_addr));

		// add all IPs from returned list.
		NET_AddLocalAddress("", (struct sockaddr*)&addr, (struct sockaddr *) &mask4);

		Sys_ShowIP();
	}

}

/*
====================
NET_OpenIP
====================
*/
void NET_OpenIP( void ) {
	int		i;
	int		err;
	int		port;

	port = net_port->integer;

	NET_GetLocalAddress();

	// automatically scan for a valid port, so multiple
	// dedicated servers can be started without requiring
	// a different net_port for each one

	if(net_enabled->integer & NET_ENABLEV4)
	{
		for( i = 0 ; i < 10 ; i++ ) {
			ip_socket = NET_IPSocket( net_ip->string, port + i, &err );
			if (ip_socket != INVALID_SOCKET) {
				Cvar_SetValue( "net_port", port + i );

				if (net_socksEnabled->integer)
					NET_OpenSocks( port + i );

				break;
			}
			else
			{
				if(err == EAFNOSUPPORT)
					break;
			}
		}

		if(ip_socket == INVALID_SOCKET)
			Com_Printf( "WARNING: Couldn't bind to a v4 ip address.\n");
	}
}


//===================================================================


/*
====================
NET_GetCvars
====================
*/
static qboolean NET_GetCvars( void ) {
	int modified;

#ifdef DEDICATED
	// I want server owners to explicitly turn on ipv6 support.
	net_enabled = Cvar_Get( "net_enabled", "1", CVAR_LATCH | CVAR_ARCHIVE );
#else
	/* End users have it enabled so they can connect to ipv6-only hosts, but ipv4 will be
	 * used if available due to ping */
	net_enabled = Cvar_Get( "net_enabled", "3", CVAR_LATCH | CVAR_ARCHIVE );
#endif
	modified = net_enabled->modified;
	net_enabled->modified = qfalse;

	net_ip = Cvar_Get( "net_ip", "0.0.0.0", CVAR_LATCH );
	modified += net_ip->modified;
	net_ip->modified = qfalse;

	net_port = Cvar_Get( "net_port", va( "%i", PORT_SERVER ), CVAR_LATCH );
	modified += net_port->modified;
	net_port->modified = qfalse;

	net_socksEnabled = Cvar_Get( "net_socksEnabled", "0", CVAR_LATCH | CVAR_ARCHIVE );
	modified += net_socksEnabled->modified;
	net_socksEnabled->modified = qfalse;

	net_socksServer = Cvar_Get( "net_socksServer", "", CVAR_LATCH | CVAR_ARCHIVE );
	modified += net_socksServer->modified;
	net_socksServer->modified = qfalse;

	net_socksPort = Cvar_Get( "net_socksPort", "1080", CVAR_LATCH | CVAR_ARCHIVE );
	modified += net_socksPort->modified;
	net_socksPort->modified = qfalse;

	net_socksUsername = Cvar_Get( "net_socksUsername", "", CVAR_LATCH | CVAR_ARCHIVE );
	modified += net_socksUsername->modified;
	net_socksUsername->modified = qfalse;

	net_socksPassword = Cvar_Get( "net_socksPassword", "", CVAR_LATCH | CVAR_ARCHIVE );
	modified += net_socksPassword->modified;
	net_socksPassword->modified = qfalse;

	net_dropsim = Cvar_Get("net_dropsim", "", CVAR_TEMP);

	return modified ? qtrue : qfalse;
}


/*
====================
NET_Config
====================
*/
void NET_Config( qboolean enableNetworking ) {
	qboolean	modified;
	qboolean	stop;
	qboolean	start;

	// get any latched changes to cvars
	modified = NET_GetCvars();

	if( !net_enabled->integer ) {
		enableNetworking = 0;
	}

	// if enable state is the same and no cvars were modified, we have nothing to do
	if( enableNetworking == networkingEnabled && !modified ) {
		return;
	}

	if( enableNetworking == networkingEnabled ) {
		if( enableNetworking ) {
			stop = qtrue;
			start = qtrue;
		}
		else {
			stop = qfalse;
			start = qfalse;
		}
	}
	else {
		if( enableNetworking ) {
			stop = qfalse;
			start = qtrue;
		}
		else {
			stop = qtrue;
			start = qfalse;
		}
		networkingEnabled = enableNetworking;
	}

	if( stop ) {
		if ( ip_socket != INVALID_SOCKET ) {
			closesocket( ip_socket );
			ip_socket = INVALID_SOCKET;
		}

		if ( socks_socket != INVALID_SOCKET ) {
			closesocket( socks_socket );
			socks_socket = INVALID_SOCKET;
		}

	}

	if( start )
	{
		if (net_enabled->integer)
		{
			NET_OpenIP();
		}
	}
}


/*
====================
NET_Init
====================
*/
void NET_Init( void ) {
	
	network_init();
	NET_Config( qtrue );

	Cmd_AddCommand ("net_restart", NET_Restart_f);
}


/*
====================
NET_Shutdown
====================
*/
void NET_Shutdown( void ) {
	if ( !networkingEnabled ) {
		return;
	}
	
	NET_Config( qfalse );
	/* network_deinit() was removed from libretro-common's net_compat; the
	 * updated network_init() is idempotent and self-managing (no matching
	 * teardown), so there is nothing to call here. */
}

/*
====================
NET_Event
Called from NET_Sleep which uses select() to determine which sockets have seen action.
====================
*/

void NET_Event(fd_set *fdr)
{
	byte bufData[MAX_MSGLEN + 1];
	netadr_t from = {0};
	msg_t netmsg;

	while(1)
	{
		MSG_Init(&netmsg, bufData, sizeof(bufData));

		if(NET_GetPacket(&from, &netmsg, fdr))
		{
			if(net_dropsim->value > 0.0f && net_dropsim->value <= 100.0f)
			{
				// com_dropsim->value percent of incoming packets get dropped.
				if(rand() < (int) (((double) RAND_MAX) / 100.0 * (double) net_dropsim->value))
					continue;          // drop this packet
			}

			if(com_sv_running->integer)
				Com_RunAndTimeServerPacket(&from, &netmsg);
			else
				CL_PacketEvent(from, &netmsg);
		}
		else
			break;
	}
}

/*
====================
NET_Sleep
Sleeps msec or until something happens on the network
====================
*/
void NET_Sleep(int msec)
{
	struct timeval timeout;
	fd_set fdr;
	int retval;
	SOCKET highestfd = INVALID_SOCKET;

	/* libretro: never block. retro_run owns frame pacing, so this only ever
	 * polls the sockets non-blocking, regardless of the requested msec. */
	msec = 0;

	FD_ZERO(&fdr);

	if(ip_socket != INVALID_SOCKET)
	{
		FD_SET(ip_socket, &fdr);

		highestfd = ip_socket;
	}

	timeout.tv_sec = msec/1000;
	timeout.tv_usec = (msec%1000)*1000;

	retval = select(highestfd + 1, &fdr, NULL, NULL, &timeout);

	if(retval == SOCKET_ERROR)
		Com_Printf("Warning: select() syscall failed: %s\n", NET_ErrorString());
	else if(retval > 0)
		NET_Event(&fdr);
}

/*
====================
NET_Restart_f
====================
*/
void NET_Restart_f(void)
{
	NET_Config(qtrue);
}

void NET_JoinMulticast6(void) {}
void NET_LeaveMulticast6() {}
