#version 330 core

uniform vec4 color;
uniform int use_texture;      // 1 = sample the client's UnitSelectTexture ring
uniform sampler2DArray tex;
uniform float tex_index;
uniform float uv_rotation;    // orients the texture's bright edge toward the camera
in vec2 local_pos;
out vec4 out_color;

void main()
{
    float r = length(local_pos);
    if (r > 1.0)
    {
        discard;
    }

    if (use_texture == 1)
    {
        // The 1.12 client's selection circle: Textures\UnitSelectTexture.blp -- white RGB, the ring
        // + interior glow live in the ALPHA channel; tinted per reaction at draw time.
        float c = cos(uv_rotation);
        float s = sin(uv_rotation);
        vec2 rotated = vec2(local_pos.x * c - local_pos.y * s,
                            local_pos.x * s + local_pos.y * c);
        vec2 uv = rotated * 0.5 + 0.5;
        float a = texture(tex, vec3(uv, tex_index)).a;
        out_color = vec4(color.rgb, a * color.a);
        return;
    }

    // Fallback: procedural filled blob with a soft rim feather.
    float alpha = color.a * 0.85 * (1.0 - smoothstep(0.78, 1.0, r));
    out_color = vec4(color.rgb, alpha);
}
