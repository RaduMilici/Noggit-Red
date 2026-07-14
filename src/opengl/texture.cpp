// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <opengl/context.hpp>
#include <opengl/context.inl>
#include <opengl/texture.hpp>

#include <QtGui/QOpenGLContext>

#include <utility>

namespace OpenGL
{
  texture::texture()
    : _id (0)
  {
    
  }

  texture::~texture()
  {
    // teardown-safe: never let a GL call throw from a destructor -> std::terminate (the "crash on
    // return to menu"). At process/scene teardown the owning context is gone OR a DIFFERENT context
    // is current (Qt AA_ShareOpenGLContexts / BLPRenderer's own context), and gl.deleteTextures'
    // internal verify throws on that mismatch. The texture dies with its context, so guard + swallow.
    if (_id > 0 && _id != -1 && QOpenGLContext::currentContext())
    {
      try { gl.deleteTextures (1, &_id); } catch (...) {}
    }
  }

  texture::texture (texture&& other)
    : _id (other._id)
  {
    other._id = -1;
  }

  texture& texture::operator= (texture&& other)
  {
    std::swap (_id, other._id);
    return *this;
  }

  void texture::bind()
  {
    if (_id == 0)
    {
      gl.genTextures (1, &_id);
    }
    gl.bindTexture (GL_TEXTURE_2D, _id);
  }

  void texture::set_active_texture (size_t num)
  {
    gl.activeTexture (static_cast<GLenum>(GL_TEXTURE0 + num));
  }

  void texture::unload()
  {
    // unload() is an explicit call (usually with the context current), but swallow any throw so it
    // can never std::terminate if invoked during teardown.
    if (_id > 0 && _id != -1)
    {
      try { gl.deleteTextures (1, &_id); } catch (...) {}
    }

    _id = 0;
  }
}
