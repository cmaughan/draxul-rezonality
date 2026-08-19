#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 position;
layout(location=1) out vec4 albedo;
layout(location=2) out vec4 normal;
void main() {
    position=vec4((uv-.5)*2.,.2,1.);
    albedo=vec4(.15+.7*uv.x,.18+.55*uv.y,.42,1.);
    normal=vec4(normalize(vec3(uv-.5,1.)),1.);
}
