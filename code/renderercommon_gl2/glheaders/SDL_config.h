/* Stand-in for SDL_config.h: SDL_opengl.h only needs it to exist. The
 * libretro core has no SDL; these two headers are kept for their OpenGL
 * declarations, which upstream ioquake3 also uses on OpenGL ES platforms. */
#ifndef SDL_config_h_
#define SDL_config_h_
#endif
