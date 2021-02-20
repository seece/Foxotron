#version 410 core

in vec3 in_pos;
in vec3 in_normal;
in vec3 in_tangent;
in vec3 in_binormal;
in vec2 in_texcoord;

out vec3 out_normal;
out vec3 out_tangent;
out vec3 out_binormal;
out vec2 out_texcoord;
out vec3 out_worldpos;
out vec3 out_viewpos;
out vec3 out_to_camera;

uniform mat4x4 mat_projection;
uniform mat4x4 mat_view;
uniform mat4x4 mat_view_inverse;
uniform mat4x4 mat_world;

void main()
{
  vec4 o = vec4( in_pos.x, in_pos.y, in_pos.z, 1.0 );
  o = mat_world * o;
  out_worldpos = o.xyz;
  o = mat_view * o;
  out_viewpos = o.xyz;

  vec3 to_camera = normalize( -o.xyz );
  out_to_camera = mat3( mat_view_inverse ) * to_camera;

  o = mat_projection * o;
  gl_Position = o;

  out_normal = normalize( mat3( mat_world ) * in_normal );
  out_tangent = normalize( mat3( mat_world ) * in_tangent );
  out_binormal = normalize( mat3( mat_world ) * in_binormal );
  out_texcoord = in_texcoord;
}
