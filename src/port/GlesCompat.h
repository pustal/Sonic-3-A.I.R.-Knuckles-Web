/**
 * @file GlesCompat.h
 * @brief OpenGL ES 2 names the game's renderer uses that the ES 2 headers lack.
 *
 * Force-included (-include) into the Oxygen engine's sources by CMakeLists.txt.
 * The engine's OpenGL drawer uses the minimum and maximum blend equations,
 * which OpenGL ES 3 has as core and ES 2 as EXT_blend_minmax, with the same
 * values. Every TV this port runs on has an ES 3 capable driver, so the
 * equations work even though the context is created as ES 2.
 */
#ifndef S3W_GLES_COMPAT_H
#define S3W_GLES_COMPAT_H

#ifndef GL_MIN
#define GL_MIN 0x8007 /* GL_MIN_EXT */
#endif
#ifndef GL_MAX
#define GL_MAX 0x8008 /* GL_MAX_EXT */
#endif

#endif // S3W_GLES_COMPAT_H
