#include <renderer/hardware/opengl_renderer.hpp>
#include <SDL3/SDL_opengl.h>
#include <SDL3/SDL_opengl_glext.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace mh::render::hardware {
namespace {
// Resolve GL through SDL so the executable has no hard dependency on a
// particular graphics vendor's libGL and uses the same context as its HUD.
struct GL {
#define BASE(name) decltype(&gl##name) name = nullptr
  BASE(GetIntegerv); BASE(GetFloatv); BASE(TexParameterf); BASE(PushAttrib); BASE(PopAttrib); BASE(PushClientAttrib); BASE(PopClientAttrib);
  BASE(Enable); BASE(Disable); BASE(DepthMask); BASE(DepthFunc); BASE(BlendFunc); BASE(PolygonOffset);
  BASE(PolygonMode); BASE(Viewport); BASE(Scissor); BASE(ClearColor); BASE(ClearDepth); BASE(Clear);
  BASE(GenTextures); BASE(DeleteTextures); BASE(BindTexture); BASE(TexParameteri); BASE(TexImage2D);
  BASE(CopyTexSubImage2D); BASE(PixelStorei); BASE(VertexPointer); BASE(ColorPointer);
  BASE(TexCoordPointer); BASE(EnableClientState); BASE(DrawArrays); BASE(GetError);
#undef BASE
  PFNGLACTIVETEXTUREPROC ActiveTexture = nullptr;
  PFNGLCLIENTACTIVETEXTUREARBPROC ClientActiveTexture = nullptr;
  PFNGLGENBUFFERSPROC GenBuffers = nullptr;
  PFNGLDELETEBUFFERSPROC DeleteBuffers = nullptr;
  PFNGLBINDBUFFERPROC BindBuffer = nullptr;
  PFNGLBUFFERDATAPROC BufferData = nullptr;
  PFNGLCREATESHADERPROC CreateShader = nullptr;
  PFNGLSHADERSOURCEPROC ShaderSource = nullptr;
  PFNGLCOMPILESHADERPROC CompileShader = nullptr;
  PFNGLGETSHADERIVPROC GetShaderiv = nullptr;
  PFNGLGETSHADERINFOLOGPROC GetShaderInfoLog = nullptr;
  PFNGLDELETESHADERPROC DeleteShader = nullptr;
  PFNGLCREATEPROGRAMPROC CreateProgram = nullptr;
  PFNGLATTACHSHADERPROC AttachShader = nullptr;
  PFNGLLINKPROGRAMPROC LinkProgram = nullptr;
  PFNGLGETPROGRAMIVPROC GetProgramiv = nullptr;
  PFNGLGETPROGRAMINFOLOGPROC GetProgramInfoLog = nullptr;
  PFNGLDELETEPROGRAMPROC DeleteProgram = nullptr;
  PFNGLUSEPROGRAMPROC UseProgram = nullptr;
  PFNGLGETUNIFORMLOCATIONPROC GetUniformLocation = nullptr;
  PFNGLUNIFORM1IPROC Uniform1i = nullptr;
  PFNGLUNIFORM4FVPROC Uniform4fv = nullptr;
  PFNGLGENFRAMEBUFFERSEXTPROC GenFramebuffers = nullptr;
  PFNGLDELETEFRAMEBUFFERSEXTPROC DeleteFramebuffers = nullptr;
  PFNGLBINDFRAMEBUFFEREXTPROC BindFramebuffer = nullptr;
  PFNGLFRAMEBUFFERTEXTURE2DEXTPROC FramebufferTexture2D = nullptr;
  PFNGLCHECKFRAMEBUFFERSTATUSEXTPROC CheckFramebufferStatus = nullptr;
  void load() {
#define LOAD(name) name = reinterpret_cast<decltype(name)>(SDL_GL_GetProcAddress("gl" #name)); if (!name) throw std::runtime_error("Missing OpenGL function gl" #name)
    LOAD(GetIntegerv); LOAD(GetFloatv); LOAD(TexParameterf); LOAD(PushAttrib); LOAD(PopAttrib); LOAD(PushClientAttrib); LOAD(PopClientAttrib);
    LOAD(Enable); LOAD(Disable); LOAD(DepthMask); LOAD(DepthFunc); LOAD(BlendFunc); LOAD(PolygonOffset);
    LOAD(PolygonMode); LOAD(Viewport); LOAD(Scissor); LOAD(ClearColor); LOAD(ClearDepth); LOAD(Clear);
    LOAD(GenTextures); LOAD(DeleteTextures); LOAD(BindTexture); LOAD(TexParameteri); LOAD(TexImage2D);
    LOAD(CopyTexSubImage2D); LOAD(PixelStorei); LOAD(VertexPointer); LOAD(ColorPointer);
    LOAD(TexCoordPointer); LOAD(EnableClientState); LOAD(DrawArrays); LOAD(GetError);
    LOAD(ActiveTexture); LOAD(ClientActiveTexture); LOAD(GenBuffers); LOAD(DeleteBuffers);
    LOAD(BindBuffer); LOAD(BufferData); LOAD(CreateShader); LOAD(ShaderSource); LOAD(CompileShader);
    LOAD(GetShaderiv); LOAD(GetShaderInfoLog); LOAD(DeleteShader); LOAD(CreateProgram); LOAD(AttachShader);
    LOAD(LinkProgram); LOAD(GetProgramiv); LOAD(GetProgramInfoLog); LOAD(DeleteProgram); LOAD(UseProgram);
    LOAD(GetUniformLocation); LOAD(Uniform1i); LOAD(Uniform4fv);
#undef LOAD
#define EXT(name) name = reinterpret_cast<decltype(name)>(SDL_GL_GetProcAddress("gl" #name "EXT")); if (!name) throw std::runtime_error("Missing framebuffer support")
    EXT(GenFramebuffers); EXT(DeleteFramebuffers); EXT(BindFramebuffer); EXT(FramebufferTexture2D); EXT(CheckFramebufferStatus);
#undef EXT
  }
};

struct Guard {
  GL &g;
  GLint program = 0, framebuffer = 0, buffer = 0, active = 0, client = 0;
  explicit Guard(GL &gl) : g(gl) {
    g.GetIntegerv(GL_CURRENT_PROGRAM, &program);
    g.GetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &framebuffer);
    g.GetIntegerv(GL_ARRAY_BUFFER_BINDING, &buffer);
    g.GetIntegerv(GL_ACTIVE_TEXTURE, &active);
    g.GetIntegerv(GL_CLIENT_ACTIVE_TEXTURE, &client);
    g.PushAttrib(GL_ALL_ATTRIB_BITS);
    g.PushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);
  }
  ~Guard() {
    g.PopClientAttrib();
    // Draw/read buffer attributes belong to the saved framebuffer. Restoring
    // GL_BACK while our offscreen FBO is bound is an INVALID_OPERATION.
    g.BindFramebuffer(GL_FRAMEBUFFER_EXT, static_cast<GLuint>(framebuffer));
    g.PopAttrib();
    g.BindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(buffer));
    g.ActiveTexture(static_cast<GLenum>(active));
    g.ClientActiveTexture(static_cast<GLenum>(client));
    g.UseProgram(static_cast<GLuint>(program));
  }
};

constexpr const char *vertex_shader = R"GLSL(#version 120
uniform vec4 output_size;
varying vec4 color;
varying vec2 uv;
varying float view_depth;
void main() {
  float d = max(gl_Vertex.z, output_size.z);
  vec2 n = gl_Vertex.xy * 2.0 / output_size.xy - 1.0;
  // FBO row zero is deliberately the authored image's top row, matching SDL
  // texture uploads. This avoids a full-screen readback or GPU flip pass.
  gl_Position = vec4(n*d, d - 2.0*output_size.z, d);
  color = gl_Color; uv = gl_MultiTexCoord0.xy; view_depth = d;
}
)GLSL";
constexpr const char *fragment_shader = R"GLSL(#version 120
uniform sampler2D image;
uniform vec4 fog_color;
uniform vec4 fog_range;
uniform int mode;
varying vec4 color;
varying vec2 uv;
varying float view_depth;
void main() {
  if (mode == 3) { gl_FragColor = vec4(0.0,0.0,0.5,1.0); return; }
  if (mode == 4) { gl_FragColor = vec4(0.44,0.58,1.0,1.0); return; }
  vec4 t = texture2D(image, uv) * color;
  if (t.a < (mode == 0 ? 0.5 : 0.00001)) discard;
  float f = 0.0;
  if (fog_range.z > 0.5 && view_depth > fog_range.x) {
    float cue = clamp((view_depth-fog_range.x)/max(0.0001,fog_range.y-fog_range.x),0.0,1.0);
    f = 1.0 - (1.0-cue)*(1.0-cue);
  }
  t.rgb = mode == 2 ? t.rgb*(1.0-f) : mix(t.rgb, fog_color.rgb, f);
  gl_FragColor = vec4(t.rgb, mode == 0 ? 1.0 : t.a);
}
)GLSL";
constexpr const char *projection_vertex = R"GLSL(#version 120
uniform vec4 output_size;
void main() { gl_Position = vec4(gl_Vertex.xy*2.0/output_size.xy-1.0,0.0,1.0); }
)GLSL";
constexpr const char *projection_fragment = R"GLSL(#version 120
uniform sampler2D scene_depth;
uniform vec4 output_size, view_position, view_right, view_up, view_forward;
uniform vec4 world_bounds, cue, cue_color, vehicle_position;
uniform vec4 body0, body1, body2, source0, direction0, source1, direction1;
uniform vec4 cell_xz[18], cell_y[9];
uniform int mode;
float cross2(vec2 a, vec2 b) { return a.x*b.y-a.y*b.x; }
bool triangle(vec2 a, vec2 b, vec2 c, vec2 p) {
  float e0=cross2(b-a,p-a), e1=cross2(c-b,p-b), e2=cross2(a-c,p-c);
  return (e0 >= -0.0001 && e1 >= -0.0001 && e2 >= -0.0001) ||
         (e0 <= 0.0001 && e1 <= 0.0001 && e2 <= 0.0001);
}
void main() {
  vec2 pixel = gl_FragCoord.xy;
  float hd = texture2D(scene_depth,pixel/output_size.xy).r;
  if (hd >= 0.9999999) discard;
  float d = output_size.w / max(0.0000001,1.0-hd);
  vec2 xy = vec2((pixel.x-output_size.z)*d/cue.w,
                (view_position.w-pixel.y)*d/cue.w);
  vec3 world=view_position.xyz+view_right.xyz*xy.x+view_up.xyz*xy.y+view_forward.xyz*d;
  float normalized_cue = cue.z > 0.5 ? clamp((d-cue.y)/max(0.0001,cue.x-cue.y),0.0,1.0) : 0.0;
  float clear_fraction = 1.0-normalized_cue;
  float fade = 1.0-clear_fraction*clear_fraction*clear_fraction;
  if (mode == 0) {
    if (world.x < world_bounds.x || world.x > world_bounds.y ||
        world.z < world_bounds.z || world.z > world_bounds.w) discard;
    bool accepted=false;
    for (int c=0;c<9;c++) {
      if (cell_y[c].z<0.5 || world.y<cell_y[c].x || world.y>cell_y[c].y) continue;
      vec4 a=cell_xz[c*2], b=cell_xz[c*2+1];
      if (triangle(a.xy,a.zw,b.xy,world.xz) || triangle(a.xy,b.xy,b.zw,world.xz)) accepted=true;
    }
    if (!accepted) discard;
    gl_FragColor=vec4(mix(vec3(10.0/255.0),cue_color.rgb,fade),1.0);
  } else {
    vec3 delta=world-vehicle_position.xyz;
    vec3 local=vec3(dot(delta,body0.xyz),dot(delta,body1.xyz),dot(delta,body2.xyz));
    float dz=(direction0.z+direction1.z)*0.5;
    if (abs(dz)<0.0000001) discard;
    float distance=(local.z-(source0.z+source1.z)*0.5)/dz;
    if (distance<0.35 || distance>20.0) discard;
    float left=source0.x+direction0.x*distance, right=source1.x+direction1.x*distance;
    if (abs(right-left)<0.0000001) discard;
    float lateral=((local.x-left)/(right-left)+0.5)*0.5;
    if (lateral<0.0 || lateral>1.0) discard;
    float edge=clamp(1.0-abs(lateral*2.0-1.0),0.0,1.0);
    float remaining=clamp(1.0-distance/20.0,0.0,1.0);
    float intensity=clamp((distance-0.35)/1.65,0.0,1.0)*remaining*remaining*edge*edge*(3.0-2.0*edge)*0.46*(1.0-fade);
    if (intensity<=0.0) discard;
    gl_FragColor=vec4(vec3(150.0/255.0)*intensity,1.0);
  }
}
)GLSL";

struct Vertex { float x,y,z,r,g,b,a,u,v; };
struct Batch {
  GLint first = 0;
  GLsizei count = 0;
  const std::vector<mh::content::PamRgbaImage> *texture = nullptr;
  SceneBlendMode blend = SceneBlendMode::opaque;
  bool trilinear = false, bias = false, depth = true;
  std::optional<std::size_t> shadow;
};
}

struct OpenGLRenderer::Impl {
  GL g;
  bool ready = false;
  bool anisotropy_supported = false;
  GLfloat texture_anisotropy = 1.0F;
  GLuint scene_program = 0, projection_program = 0, vbo = 0, quad_vbo = 0, fbo = 0;
  GLuint color = 0, depth = 0, depth_copy = 0, white = 0;
  SDL_Texture *wrapped = nullptr;
  int width = 0, height = 0;
  double render_ms = 0;
  std::unordered_map<const std::vector<mh::content::PamRgbaImage> *, GLuint> textures;
  std::vector<Vertex> vertices;
  std::vector<Batch> batches;

  ~Impl() {
    if (!ready) return;
    if (wrapped) SDL_DestroyTexture(wrapped);
    for (auto &entry : textures) g.DeleteTextures(1,&entry.second);
    for (auto id : {color,depth,depth_copy,white}) if (id) g.DeleteTextures(1,&id);
    if (fbo) g.DeleteFramebuffers(1,&fbo);
    if (vbo) g.DeleteBuffers(1,&vbo);
    if (quad_vbo) g.DeleteBuffers(1,&quad_vbo);
    if (scene_program) g.DeleteProgram(scene_program);
    if (projection_program) g.DeleteProgram(projection_program);
  }
  GLuint shader(GLenum type, const char *text) {
    const auto id=g.CreateShader(type);
    g.ShaderSource(id,1,&text,nullptr); g.CompileShader(id);
    GLint ok=0; g.GetShaderiv(id,GL_COMPILE_STATUS,&ok);
    if (!ok) {
      std::array<char,4096> log{}; g.GetShaderInfoLog(id,static_cast<GLsizei>(log.size()),nullptr,log.data());
      g.DeleteShader(id); throw std::runtime_error(std::string("OpenGL shader: ")+log.data());
    }
    return id;
  }
  GLuint program(const char *vs, const char *fs) {
    const auto v=shader(GL_VERTEX_SHADER,vs), f=shader(GL_FRAGMENT_SHADER,fs);
    const auto p=g.CreateProgram(); g.AttachShader(p,v); g.AttachShader(p,f); g.LinkProgram(p);
    g.DeleteShader(v); g.DeleteShader(f);
    GLint ok=0; g.GetProgramiv(p,GL_LINK_STATUS,&ok);
    if (!ok) {
      std::array<char,4096> log{}; g.GetProgramInfoLog(p,static_cast<GLsizei>(log.size()),nullptr,log.data());
      g.DeleteProgram(p); throw std::runtime_error(std::string("OpenGL program: ")+log.data());
    }
    return p;
  }
  void vec(GLuint p, const char *name, std::array<float,4> v) {
    g.Uniform4fv(g.GetUniformLocation(p,name),1,v.data());
  }
  template<class V> static std::array<float,4> vector4(const V &v, float w=0) {
    return {static_cast<float>(v[0]),static_cast<float>(v[1]),static_cast<float>(v[2]),w};
  }
  void integer(GLuint p,const char *name,int value) { g.Uniform1i(g.GetUniformLocation(p,name),value); }
  void checked(const char *stage) {
    const auto error=g.GetError();
    if (error!=GL_NO_ERROR) throw std::runtime_error(std::string("OpenGL ")+stage+" error "+std::to_string(error));
  }
  void texture_parameters(bool mipmaps) {
    g.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    g.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,mipmaps ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    g.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT);
    g.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT);
    // Isotropic mip selection blurs the short axis of the road's oblique
    // footprint. Preserve mipmaps and perspective derivatives while allowing
    // the driver to filter that footprint along its long axis. Bilinear mode
    // and render targets retain their existing single-level sampling policy.
    if (anisotropy_supported)
      g.TexParameterf(GL_TEXTURE_2D,GL_TEXTURE_MAX_ANISOTROPY_EXT,
                      mipmaps ? texture_anisotropy : 1.0F);
  }
  void targets(SDL_Renderer *renderer,int w,int h) {
    if (wrapped) { SDL_DestroyTexture(wrapped); wrapped=nullptr; }
    for (auto id : {color,depth,depth_copy}) if (id) g.DeleteTextures(1,&id);
    width=w; height=h;
    g.ActiveTexture(GL_TEXTURE0);
    g.GenTextures(1,&color); g.BindTexture(GL_TEXTURE_2D,color);
    texture_parameters(false);
    g.TexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,width,height,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
    for (auto *id : {&depth,&depth_copy}) {
      g.GenTextures(1,id); g.BindTexture(GL_TEXTURE_2D,*id);
      g.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
      g.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
      g.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
      g.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
      g.TexImage2D(GL_TEXTURE_2D,0,GL_DEPTH_COMPONENT24,width,height,0,GL_DEPTH_COMPONENT,GL_UNSIGNED_INT,nullptr);
    }
    if (!fbo) g.GenFramebuffers(1,&fbo);
    g.BindFramebuffer(GL_FRAMEBUFFER_EXT,fbo);
    g.FramebufferTexture2D(GL_FRAMEBUFFER_EXT,GL_COLOR_ATTACHMENT0_EXT,GL_TEXTURE_2D,color,0);
    g.FramebufferTexture2D(GL_FRAMEBUFFER_EXT,GL_DEPTH_ATTACHMENT_EXT,GL_TEXTURE_2D,depth,0);
    if (g.CheckFramebufferStatus(GL_FRAMEBUFFER_EXT)!=GL_FRAMEBUFFER_COMPLETE_EXT)
      throw std::runtime_error("Native OpenGL framebuffer is incomplete");
    const auto props=SDL_CreateProperties();
    SDL_SetNumberProperty(props,SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER,SDL_PIXELFORMAT_RGBA32);
    SDL_SetNumberProperty(props,SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER,SDL_TEXTUREACCESS_STATIC);
    SDL_SetNumberProperty(props,SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER,width);
    SDL_SetNumberProperty(props,SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER,height);
    SDL_SetNumberProperty(props,SDL_PROP_TEXTURE_CREATE_OPENGL_TEXTURE_NUMBER,color);
    wrapped=SDL_CreateTextureWithProperties(renderer,props); SDL_DestroyProperties(props);
    if (!wrapped) throw std::runtime_error(std::string("Wrap native GPU scene: ")+SDL_GetError());
    SDL_SetTextureBlendMode(wrapped,SDL_BLENDMODE_BLEND);
  }
  GLuint texture(const std::vector<mh::content::PamRgbaImage> *levels) {
    if (!levels || levels->empty()) return white;
    auto found=textures.find(levels);
    if (found!=textures.end()) return found->second;
    GLuint id=0; g.GenTextures(1,&id); g.BindTexture(GL_TEXTURE_2D,id);
    g.PixelStorei(GL_UNPACK_ALIGNMENT,1);
    g.PixelStorei(GL_UNPACK_ROW_LENGTH,0);
    g.PixelStorei(GL_UNPACK_SKIP_PIXELS,0);
    g.PixelStorei(GL_UNPACK_SKIP_ROWS,0);
    GLint level=0;
    for (const auto &image : *levels) {
      g.TexImage2D(GL_TEXTURE_2D,level++,GL_RGBA8,image.width,image.height,0,GL_RGBA,GL_UNSIGNED_BYTE,image.rgba.data());
    }
    g.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAX_LEVEL,level-1);
    texture_parameters(false);
    textures.emplace(levels,id);
    return id;
  }
  void build(const SceneFrameView &frame) {
    vertices.clear(); batches.clear();
    for (const auto &c : *frame.commands) {
      if (c.shadow_projection_index) { Batch b; b.shadow=c.shadow_projection_index; batches.push_back(b); continue; }
      if (c.vertex_count<3 || c.vertex_count>4) continue;
      const auto first=static_cast<GLint>(vertices.size());
      constexpr std::array<unsigned,6> indices{0,1,2,0,2,3};
      const auto count=c.vertex_count==4 ? 6 : 3;
      for (int j=0;j<count;j++) {
        const auto i=indices[static_cast<std::size_t>(j)]; const auto &s=c.vertices[i];
        vertices.push_back({s.position.x,s.position.y,static_cast<float>(c.view_depths[i]),
          s.color.r,s.color.g,s.color.b,s.color.a,s.tex_coord.x,s.tex_coord.y});
      }
      if (!batches.empty()) {
        auto &b=batches.back();
        if (!b.shadow && b.first+b.count==first && b.texture==c.texture_levels && b.blend==c.blend_mode &&
            b.trilinear==c.trilinear_filtering && b.bias==c.coplanar_depth_bias && b.depth==c.depth_test) {
          b.count+=count; continue;
        }
      }
      batches.push_back({first,count,c.texture_levels,c.blend_mode,c.trilinear_filtering,c.coplanar_depth_bias,c.depth_test,{}});
    }
  }
  void vertex_arrays(GLuint buffer=0) {
    g.BindBuffer(GL_ARRAY_BUFFER,buffer ? buffer : vbo);
    g.EnableClientState(GL_VERTEX_ARRAY); g.VertexPointer(3,GL_FLOAT,sizeof(Vertex),nullptr);
    g.EnableClientState(GL_COLOR_ARRAY); g.ColorPointer(4,GL_FLOAT,sizeof(Vertex),reinterpret_cast<void *>(offsetof(Vertex,r)));
    g.ClientActiveTexture(GL_TEXTURE0);
    g.EnableClientState(GL_TEXTURE_COORD_ARRAY); g.TexCoordPointer(2,GL_FLOAT,sizeof(Vertex),reinterpret_cast<void *>(offsetof(Vertex,u)));
  }
  void upload() {
    g.BindBuffer(GL_ARRAY_BUFFER,vbo);
    g.BufferData(GL_ARRAY_BUFFER,static_cast<GLsizeiptr>(vertices.size()*sizeof(Vertex)),vertices.data(),GL_STREAM_DRAW);
    vertex_arrays();
  }
  void scene_uniforms(const SceneFrameView &frame) {
    g.UseProgram(scene_program);
    vec(scene_program,"output_size",{static_cast<float>(width),static_cast<float>(height),static_cast<float>(frame.near_plane),0});
    vec(scene_program,"fog_color",{frame.fog_color[0]/255,frame.fog_color[1]/255,frame.fog_color[2]/255,1});
    vec(scene_program,"fog_range",{static_cast<float>(frame.fog_start),static_cast<float>(frame.fog_end),frame.fog_enabled ? 1.0F : 0.0F,0});
    integer(scene_program,"image",0);
  }
  void batch(const Batch &b,bool tron,bool line=false) {
    g.UseProgram(scene_program);
    if (b.depth) g.Enable(GL_DEPTH_TEST); else g.Disable(GL_DEPTH_TEST);
    g.DepthFunc(GL_LEQUAL); g.DepthMask(b.depth && (b.blend==SceneBlendMode::opaque || tron) && !line);
    if (!tron && b.blend!=SceneBlendMode::opaque) {
      g.Enable(GL_BLEND); g.BlendFunc(GL_SRC_ALPHA,b.blend==SceneBlendMode::additive ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
    } else g.Disable(GL_BLEND);
    if (b.bias || line) { g.Enable(GL_POLYGON_OFFSET_FILL); g.PolygonOffset(0,-1); }
    else g.Disable(GL_POLYGON_OFFSET_FILL);
    g.PolygonMode(GL_FRONT_AND_BACK,line ? GL_LINE : GL_FILL);
    integer(scene_program,"mode",tron ? (line ? 4 : 3) : static_cast<int>(b.blend));
    g.ActiveTexture(GL_TEXTURE0); g.BindTexture(GL_TEXTURE_2D,texture(b.texture));
    texture_parameters(b.trilinear && b.texture && b.texture->size()>1);
    g.DrawArrays(GL_TRIANGLES,b.first,b.count);
  }
  void projection(const PerspectiveView &view,double distance,double start,bool fog,const std::array<float,3> &color_value) {
    g.ActiveTexture(GL_TEXTURE1); g.BindTexture(GL_TEXTURE_2D,depth_copy);
    g.CopyTexSubImage2D(GL_TEXTURE_2D,0,0,0,0,0,width,height);
    checked("depth snapshot");
    g.UseProgram(projection_program); integer(projection_program,"scene_depth",1);
    vec(projection_program,"output_size",{static_cast<float>(width),static_cast<float>(height),view.center_x,static_cast<float>(view.near_plane)});
    vec(projection_program,"view_position",vector4(view.position,view.center_y));
    vec(projection_program,"view_right",vector4(view.right)); vec(projection_program,"view_up",vector4(view.up));
    vec(projection_program,"view_forward",vector4(view.forward));
    vec(projection_program,"cue",{static_cast<float>(distance),static_cast<float>(start),fog?1.0F:0.0F,view.focal_length});
    vec(projection_program,"cue_color",vector4(color_value));
    g.Disable(GL_DEPTH_TEST); g.DepthMask(false); g.Disable(GL_POLYGON_OFFSET_FILL); g.PolygonMode(GL_FRONT_AND_BACK,GL_FILL);
  }
  void quad(int left,int right,int top,int bottom) {
    const float l=static_cast<float>(left), r=static_cast<float>(right+1), t=static_cast<float>(top), b=static_cast<float>(bottom+1);
    const std::array<Vertex,6> q{{{l,t,1,0,0,0,0,0,0},{r,t,1,0,0,0,0,0,0},{r,b,1,0,0,0,0,0,0},
                               {l,t,1,0,0,0,0,0,0},{r,b,1,0,0,0,0,0,0},{l,b,1,0,0,0,0,0,0}}};
    g.BindBuffer(GL_ARRAY_BUFFER,quad_vbo);
    g.BufferData(GL_ARRAY_BUFFER,sizeof(q),q.data(),GL_STREAM_DRAW);
    vertex_arrays(quad_vbo);
    g.DrawArrays(GL_TRIANGLES,0,6);
    vertex_arrays();
  }
  void shadow(const VehicleShadowDepthProjection &p) {
    projection(p.view,p.render_distance,p.cue_start,p.cue_enabled,{p.cue_color[0]/255,p.cue_color[1]/255,p.cue_color[2]/255});
    integer(projection_program,"mode",0); g.Disable(GL_BLEND);
    vec(projection_program,"world_bounds",{static_cast<float>(p.minimum_world_x),static_cast<float>(p.maximum_world_x),
      static_cast<float>(p.minimum_world_z),static_cast<float>(p.maximum_world_z)});
    std::array<std::array<float,4>,18> xz{}; std::array<std::array<float,4>,9> y{};
    for (std::size_t i=0;i<p.cells.size();++i) {
      const auto &c=p.cells[i];
      xz[i*2]={static_cast<float>(c.world_xz[0][0]),static_cast<float>(c.world_xz[0][1]),static_cast<float>(c.world_xz[1][0]),static_cast<float>(c.world_xz[1][1])};
      xz[i*2+1]={static_cast<float>(c.world_xz[2][0]),static_cast<float>(c.world_xz[2][1]),static_cast<float>(c.world_xz[3][0]),static_cast<float>(c.world_xz[3][1])};
      y[i]={static_cast<float>(c.minimum_receiver_y),static_cast<float>(c.maximum_receiver_y),c.valid?1.0F:0.0F,0};
    }
    g.Uniform4fv(g.GetUniformLocation(projection_program,"cell_xz[0]"),18,xz[0].data());
    g.Uniform4fv(g.GetUniformLocation(projection_program,"cell_y[0]"),9,y[0].data());
    quad(p.minimum_x,p.maximum_x,p.minimum_y,p.maximum_y);
  }
  void headlight(const HeadlightDepthProjection &p) {
    projection(p.view,p.render_distance,p.cue_start,p.cue_enabled,{0,0,0});
    integer(projection_program,"mode",1); g.Enable(GL_BLEND); g.BlendFunc(GL_ONE,GL_ONE);
    vec(projection_program,"vehicle_position",vector4(p.vehicle.world_position));
    vec(projection_program,"body0",vector4(p.vehicle.body_basis[0]));
    vec(projection_program,"body1",vector4(p.vehicle.body_basis[1]));
    vec(projection_program,"body2",vector4(p.vehicle.body_basis[2]));
    vec(projection_program,"source0",vector4(p.sources[0].center)); vec(projection_program,"direction0",vector4(p.sources[0].direction));
    vec(projection_program,"source1",vector4(p.sources[1].center)); vec(projection_program,"direction1",vector4(p.sources[1].direction));
    checked("headlight uniforms");
    quad(p.minimum_x,p.maximum_x,p.minimum_y,p.maximum_y);
    checked("headlight draw");
  }
  SDL_Texture *render(SDL_Renderer *renderer,const SceneFrameView &frame) {
    if (!SDL_FlushRenderer(renderer)) throw std::runtime_error(SDL_GetError());
    while (g.GetError()!=GL_NO_ERROR) {}
    Guard guard(g);
    if (width!=frame.width || height!=frame.height || !wrapped) targets(renderer,frame.width,frame.height);
    checked("target setup");
    const auto started=std::chrono::steady_clock::now();
    g.BindFramebuffer(GL_FRAMEBUFFER_EXT,fbo); g.Viewport(0,0,width,height);
    g.Disable(GL_SCISSOR_TEST); g.Disable(GL_CULL_FACE); g.Disable(GL_ALPHA_TEST); g.Disable(GL_STENCIL_TEST);
    g.Disable(GL_FRAMEBUFFER_SRGB); g.DepthMask(true); g.ClearColor(0,0,0,0); g.ClearDepth(1);
    g.Clear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    build(frame); upload(); scene_uniforms(frame);
    checked("geometry setup");
    for (const auto &b : batches) {
      if (b.shadow) {
        if (!frame.tron_hidden_line && *b.shadow<frame.shadow_projections->size()) shadow(frame.shadow_projections->at(*b.shadow));
        scene_uniforms(frame);
      } else batch(b,frame.tron_hidden_line);
    }
    if (frame.tron_hidden_line) {
      for (const auto &b : batches) if (!b.shadow) batch(b,true,true);
    } else for (const auto &p : *frame.headlight_projections) headlight(p);
    const auto error=g.GetError();
    if (error!=GL_NO_ERROR) throw std::runtime_error("Native OpenGL scene error "+std::to_string(error));
    render_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
    return wrapped;
  }
};

OpenGLRenderer::OpenGLRenderer() : impl_(std::make_unique<Impl>()) {}
OpenGLRenderer::~OpenGLRenderer() = default;
bool OpenGLRenderer::initialize(SDL_Renderer *renderer) {
  if (impl_->ready) return true;
  if (!renderer || std::strcmp(SDL_GetRendererName(renderer),"opengl")!=0) return false;
  if (!SDL_FlushRenderer(renderer)) return false;
  impl_->g.load(); impl_->ready=true;
  Guard guard(impl_->g);
  impl_->anisotropy_supported =
      SDL_GL_ExtensionSupported("GL_EXT_texture_filter_anisotropic") ||
      SDL_GL_ExtensionSupported("GL_ARB_texture_filter_anisotropic");
  if (impl_->anisotropy_supported) {
    GLfloat maximum = 1.0F;
    impl_->g.GetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT,&maximum);
    if (std::isfinite(maximum))
      impl_->texture_anisotropy = std::clamp(maximum,1.0F,16.0F);
  }
  impl_->scene_program=impl_->program(vertex_shader,fragment_shader);
  impl_->projection_program=impl_->program(projection_vertex,projection_fragment);
  impl_->g.GenBuffers(1,&impl_->vbo);
  impl_->g.GenBuffers(1,&impl_->quad_vbo);
  impl_->g.ActiveTexture(GL_TEXTURE0);
  impl_->g.GenTextures(1,&impl_->white); impl_->g.BindTexture(GL_TEXTURE_2D,impl_->white);
  const std::array<unsigned char,4> pixel{255,255,255,255};
  impl_->g.TexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,pixel.data());
  impl_->texture_parameters(false);
  return true;
}
bool OpenGLRenderer::available() const noexcept { return impl_->ready; }
SDL_Texture *OpenGLRenderer::render(SDL_Renderer *renderer,const SceneFrameView &frame) { return impl_->render(renderer,frame); }
double OpenGLRenderer::last_render_ms() const noexcept { return impl_->render_ms; }
}
