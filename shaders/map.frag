#version 450
layout(binding=0) uniform sampler2D atlas;
layout(location=0) in vec2 uv;
layout(location=1) flat in int tile;
layout(location=0) out vec4 color;
layout(constant_id=0) const bool CUTOUT=true;
void main(){
    uint id=uint(abs(tile)-1);
    ivec2 pixel;
    if(tile<0){
        pixel=ivec2(id>>4,496u+(id&15u));
    }else{
        ivec2 fp=ivec2(fract(uv)*16.0);
        pixel=ivec2((id&31u)<<4,(id>>5)<<4)+ivec2(fp.x,15-fp.y);
    }
    color=texelFetch(atlas,pixel,0);
    if(CUTOUT&&color.a<0.5)discard;
}
