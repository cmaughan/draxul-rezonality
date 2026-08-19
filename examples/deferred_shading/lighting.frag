#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
layout(set=1,binding=0) uniform sampler2D Positions;
layout(set=1,binding=1) uniform sampler2D Albedo;
layout(set=1,binding=2) uniform sampler2D Normals;
void main() {
    vec3 p=texture(Positions,uv).xyz;
    vec3 a=texture(Albedo,uv).rgb;
    vec3 n=normalize(texture(Normals,uv).xyz);
    vec3 l=normalize(vec3(.7,.5,1.)-p);
    color=vec4(a*(.15+.85*max(dot(n,l),0.)),1.);
}
