#version 450
// the lower-left half of the target: (-1,-1), (1,-1), (-1,1)
const vec2 pos[3] = vec2[](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0));
void main() { gl_Position = vec4(pos[gl_VertexIndex], 0.0, 1.0); }
