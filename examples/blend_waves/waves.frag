#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
void main() {
    float a=.5+.5*sin(uv.x*35.+sin(uv.y*8.)*3.);
    float b=.5+.5*cos(uv.y*29.+uv.x*7.);
    color=vec4(.05+a*.1,b*.55,.35+a*.55,1.);
}
