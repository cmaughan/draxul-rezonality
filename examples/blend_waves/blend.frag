#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
layout(set=1,binding=0) uniform sampler2D Waves;
void main() {
    vec3 c=texture(Waves,uv).rgb;
    color=vec4(c*c+vec3(.02,.01,.04),1.);
}
