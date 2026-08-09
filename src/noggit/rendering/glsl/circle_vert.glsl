#version 330 core

in vec4 position;
in vec2 local; // unit-space coords for the radial gradient (world_space path)
out vec2 local_pos;

uniform mat4 model_view_projection;
uniform vec3 origin;
uniform float radius;
uniform float inclination;
uniform float orientation;
uniform int world_space; // 1 = position is already world-space (terrain-conforming disc)

void main()
{
    if (world_space == 1)
    {
        gl_Position = model_view_projection * vec4(position.xyz, 1.0);
        local_pos = local;
        return;
    }

    vec4 pos = position;
    float cos_o = cos(orientation);
    float sin_o = sin(orientation);

    pos.y += pos.x * tan(inclination) * radius;

    pos.x = (position.x * cos_o - position.z * sin_o) * radius;
    pos.z = (position.z * cos_o + position.x * sin_o) * radius;

    pos.xyz += origin;
    gl_Position = model_view_projection * pos;

    // Unscaled local XZ coordinates for radial ring gradient.
    local_pos = position.xz;
}
