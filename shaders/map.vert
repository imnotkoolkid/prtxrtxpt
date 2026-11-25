#version 450
layout(location=0) in vec3 origin;
layout(location=1) in vec4 sizeOffset;
layout(location=2) in uint orientation;
layout(location=3) in int material;
layout(push_constant) uniform Camera {mat4 vp;} camera;
layout(location=0) out vec2 uv;
layout(location=1) flat out int tile;
const vec3 U[8]=vec3[](vec3(-1,0,0),vec3(1,0,0),vec3(0,0,-1),vec3(0,0,1),vec3(-1,0,0),vec3(1,0,0),vec3(1,0,1),vec3(1,0,-1));
void main(){
    uint face=orientation&7u;
    uint corner=uint(gl_VertexIndex)^(orientation>>3);
    vec2 span=vec2(corner&1u,corner>>1u)*sizeOffset.xy;
    vec3 V=face<2u?vec3(0,0,1):vec3(0,1,0);
    gl_Position=camera.vp*vec4(origin+U[face]*span.x+V*span.y,1);
    uv=span+sizeOffset.zw;
    tile=material;
}
