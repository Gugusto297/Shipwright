#pragma once
#if defined(__SWITCH__) && defined(ENABLE_OPENGL)
#include <glad/glad.h>
#include <algorithm>
#include <cstdio>

namespace Fast::SwitchBloom {
namespace detail {
static GLuint program=0, vbo=0, texScene=0, texGlow[2]={0,0}, fbo[2]={0,0};
static int w=0,h=0,sw=0,sh=0;
static bool failed=false;
static GLint locPass=-1,locScene=-1,locGlow=-1,locDirection=-1,locPixel=-1,locStrength=-1;

static const char* vs=R"GLSL(#version 120
attribute vec2 pos;
varying vec2 uv;
void main(){ uv=(pos+vec2(1.0))*0.5; gl_Position=vec4(pos,0.0,1.0); }
)GLSL";
static const char* fs=R"GLSL(#version 120
varying vec2 uv;
uniform sampler2D scene,glow;
uniform vec2 pixel,direction;
uniform int pass;
uniform float strength;
void main(){
  if(pass==0){
    vec3 c=texture2D(scene,uv).rgb;
    vec3 b=max(c-vec3(0.64),vec3(0.0));
    gl_FragColor=vec4(pow(b/0.36,vec3(1.5)),1.0);
  }else if(pass==1){
    vec3 c=texture2D(scene,uv).rgb*0.227027;
    c+=(texture2D(scene,uv+direction*1.384615).rgb+
        texture2D(scene,uv-direction*1.384615).rgb)*0.316216;
    c+=(texture2D(scene,uv+direction*3.230769).rgb+
        texture2D(scene,uv-direction*3.230769).rgb)*0.070270;
    gl_FragColor=vec4(c,1.0);
  }else{
    vec4 c=texture2D(scene,uv);
    vec3 g=texture2D(glow,uv).rgb;
    gl_FragColor=vec4(clamp(c.rgb+g*strength*0.45,0.0,1.0),c.a);
  }
}
)GLSL";
static GLuint compile(GLenum type,const char* src){
  GLuint s=glCreateShader(type);
  glShaderSource(s,1,&src,nullptr); glCompileShader(s);
  GLint ok=0; glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
  if(!ok){char msg[1024]={};glGetShaderInfoLog(s,sizeof(msg),nullptr,msg);
    std::printf("[SoH Bloom] shader failed: %s\n",msg);glDeleteShader(s);return 0;}
  return s;
}
static bool init(){
  auto v=compile(GL_VERTEX_SHADER,vs);
  auto f=compile(GL_FRAGMENT_SHADER,fs);
  if(!v||!f){if(v)glDeleteShader(v);if(f)glDeleteShader(f);return false;}
  program=glCreateProgram(); glAttachShader(program,v);glAttachShader(program,f);
  glBindAttribLocation(program,0,"pos");glLinkProgram(program);
  glDeleteShader(v);glDeleteShader(f);
  GLint ok=0;glGetProgramiv(program,GL_LINK_STATUS,&ok);
  if(!ok){char msg[1024]={};glGetProgramInfoLog(program,sizeof(msg),nullptr,msg);
    std::printf("[SoH Bloom] link failed: %s\n",msg);return false;}
  locPass=glGetUniformLocation(program,"pass");locScene=glGetUniformLocation(program,"scene");
  locGlow=glGetUniformLocation(program,"glow");locDirection=glGetUniformLocation(program,"direction");
  locPixel=glGetUniformLocation(program,"pixel");locStrength=glGetUniformLocation(program,"strength");
  const GLfloat quad[]={-1,-1,1,-1,1,1,-1,-1,1,1,-1,1};
  glGenBuffers(1,&vbo);glBindBuffer(GL_ARRAY_BUFFER,vbo);
  glBufferData(GL_ARRAY_BUFFER,sizeof(quad),quad,GL_STATIC_DRAW);
  return true;
}
static void newTexture(GLuint& tex,int width,int height){
  glGenTextures(1,&tex);glBindTexture(GL_TEXTURE_2D,tex);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,width,height,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
}
static bool resize(int width,int height){
  if(width==w&&height==h)return true;
  if(texScene)glDeleteTextures(1,&texScene);
  glDeleteTextures(2,texGlow);glDeleteFramebuffers(2,fbo);
  texScene=0;texGlow[0]=texGlow[1]=0;fbo[0]=fbo[1]=0;
  w=width;h=height;sw=std::max(1,w/4);sh=std::max(1,h/4);
  newTexture(texScene,w,h);
  glGenFramebuffers(2,fbo);
  for(int i=0;i<2;i++){
    newTexture(texGlow[i],sw,sh);glBindFramebuffer(GL_FRAMEBUFFER,fbo[i]);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texGlow[i],0);
    if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)return false;
  }
  return true;
}
static void tex(int i,GLuint t){glActiveTexture(GL_TEXTURE0+i);glBindTexture(GL_TEXTURE_2D,t);}
static void draw(){glDrawArrays(GL_TRIANGLES,0,6);}
}
inline void Apply(int width,int height) {
  using namespace detail;
  if(failed||width<=0||height<=0)return;
  GLint oldDraw=0,oldRead=0,oldReadBuffer=0,oldProgram=0,oldArray=0,oldActive=0;
  GLint oldTex0=0,oldTex1=0,oldViewport[4]={0};
  GLint attrOn=0,attrSize=0,attrType=0,attrNorm=0,attrStride=0,attrVbo=0;
  void* attrPtr=nullptr;
  GLboolean mask[4]={0};
  GLboolean depth=glIsEnabled(GL_DEPTH_TEST),blend=glIsEnabled(GL_BLEND);
  GLboolean scissor=glIsEnabled(GL_SCISSOR_TEST),stencil=glIsEnabled(GL_STENCIL_TEST);
  GLboolean cull=glIsEnabled(GL_CULL_FACE);
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&oldDraw);
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&oldRead);
  glGetIntegerv(GL_READ_BUFFER,&oldReadBuffer);
  glGetIntegerv(GL_CURRENT_PROGRAM,&oldProgram);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING,&oldArray);
  glGetIntegerv(GL_ACTIVE_TEXTURE,&oldActive);
  glGetIntegerv(GL_VIEWPORT,oldViewport);
  glGetBooleanv(GL_COLOR_WRITEMASK,mask);
  glGetVertexAttribiv(0,GL_VERTEX_ATTRIB_ARRAY_ENABLED,&attrOn);
  glGetVertexAttribiv(0,GL_VERTEX_ATTRIB_ARRAY_SIZE,&attrSize);
  glGetVertexAttribiv(0,GL_VERTEX_ATTRIB_ARRAY_TYPE,&attrType);
  glGetVertexAttribiv(0,GL_VERTEX_ATTRIB_ARRAY_NORMALIZED,&attrNorm);
  glGetVertexAttribiv(0,GL_VERTEX_ATTRIB_ARRAY_STRIDE,&attrStride);
  glGetVertexAttribiv(0,GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING,&attrVbo);
  glGetVertexAttribPointerv(0,GL_VERTEX_ATTRIB_ARRAY_POINTER,&attrPtr);
  glActiveTexture(GL_TEXTURE0);glGetIntegerv(GL_TEXTURE_BINDING_2D,&oldTex0);
  glActiveTexture(GL_TEXTURE1);glGetIntegerv(GL_TEXTURE_BINDING_2D,&oldTex1);

  bool ok=program?true:init();
  if(ok)ok=resize(width,height);
  if(!ok){failed=true;} else {
    glBindFramebuffer(GL_READ_FRAMEBUFFER,0);glReadBuffer(GL_BACK);
    tex(0,texScene);glCopyTexSubImage2D(GL_TEXTURE_2D,0,0,0,0,0,width,height);
    glDisable(GL_DEPTH_TEST);glDisable(GL_BLEND);glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    glUseProgram(program);glBindBuffer(GL_ARRAY_BUFFER,vbo);
    glEnableVertexAttribArray(0);glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,nullptr);
    glUniform1i(locScene,0);glUniform1i(locGlow,1);
    glUniform1f(locStrength,1.2f);
    glUniform2f(locPixel,1.0f/width,1.0f/height);
    glViewport(0,0,sw,sh);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,fbo[0]);tex(0,texScene);
    glUniform1i(locPass,0);draw();
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,fbo[1]);tex(0,texGlow[0]);
    glUniform1i(locPass,1);glUniform2f(locDirection,1.0f/sw,0.0f);draw();
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,fbo[0]);tex(0,texGlow[1]);
    glUniform2f(locDirection,0.0f,1.0f/sh);draw();
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,0);glViewport(0,0,width,height);
    tex(0,texScene);tex(1,texGlow[0]);glUniform1i(locPass,2);draw();
  }
  glBindBuffer(GL_ARRAY_BUFFER,attrVbo);
  if(attrVbo)glVertexAttribPointer(0,attrSize,attrType,attrNorm?GL_TRUE:GL_FALSE,attrStride,attrPtr);
  if(attrOn)glEnableVertexAttribArray(0);else glDisableVertexAttribArray(0);
  glBindBuffer(GL_ARRAY_BUFFER,oldArray);
  tex(0,oldTex0);tex(1,oldTex1);glActiveTexture(oldActive);
  glUseProgram(oldProgram);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER,oldDraw);
  glBindFramebuffer(GL_READ_FRAMEBUFFER,oldRead);glReadBuffer(oldReadBuffer);
  glViewport(oldViewport[0],oldViewport[1],oldViewport[2],oldViewport[3]);
  glColorMask(mask[0],mask[1],mask[2],mask[3]);
  if(depth)glEnable(GL_DEPTH_TEST);else glDisable(GL_DEPTH_TEST);
  if(blend)glEnable(GL_BLEND);else glDisable(GL_BLEND);
  if(scissor)glEnable(GL_SCISSOR_TEST);else glDisable(GL_SCISSOR_TEST);
  if(stencil)glEnable(GL_STENCIL_TEST);else glDisable(GL_STENCIL_TEST);
  if(cull)glEnable(GL_CULL_FACE);else glDisable(GL_CULL_FACE);
}
}
#endif
