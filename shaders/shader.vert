#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inColor;
layout(location = 3) in vec2 inTexCoord;

layout(push_constant) uniform PushConstants {
    mat4 model;
    mat4 view;
    mat4 proj;
    vec4 cameraPos;   // xyz = eye, w = time
    vec4 carColor;    // rgb = color, a = metallic
    vec4 renderFlags; // x = useTextures, y = wireframePass (0=solid, 1=overlay, 2=wireOnly), z = materialType (0=textured/raw, 1=carPaint, 2=glass, 3=rubber/dark, 4=chrome/rim), w = isMap
} u;

layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec4 fragColor;
layout(location = 3) out vec2 fragTexCoord;
layout(location = 4) out vec3 fragEyePos;
layout(location = 5) out vec4 fragCarColor;
layout(location = 6) out vec4 fragRenderFlags;

void main() {
    if (u.renderFlags.y > 2.5) {
        fragWorldPos = vec3(0.0);
        fragNormal = vec3(0.0, 0.0, 1.0);
        fragColor = inColor;
        fragTexCoord = inTexCoord;
        fragEyePos = vec3(0.0);
        fragCarColor = u.carColor;
        fragRenderFlags = u.renderFlags;
        gl_Position = vec4(inPosition.xy, 0.0, 1.0);
        return;
    }

    vec4 worldPos = u.model * vec4(inPosition, 1.0);
    fragWorldPos = worldPos.xyz;
    fragNormal = normalize(mat3(u.model) * inNormal);
    fragColor = inColor;
    fragTexCoord = inTexCoord;
    fragEyePos = u.cameraPos.xyz;
    fragCarColor = u.carColor;
    fragRenderFlags = u.renderFlags;

    gl_Position = u.proj * u.view * worldPos;
}
