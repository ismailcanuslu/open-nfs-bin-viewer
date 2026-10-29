#version 450

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec4 fragColor;
layout(location = 3) in vec2 fragTexCoord;
layout(location = 4) in vec3 fragEyePos;
layout(location = 5) in vec4 fragCarColor;
layout(location = 6) in vec4 fragRenderFlags;

layout(binding = 0) uniform sampler2D texSampler;

layout(location = 0) out vec4 outColor;

// ReShade Photorealistic Preset: 3197K Kelvin Warm Light Color
const vec3 KELVIN_3197 = vec3(1.0, 0.776, 0.545);

float getLuminance(vec3 color) {
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

vec3 applyLightroomCurve(vec3 col) {
    float luma = getLuminance(col);
    if (luma <= 0.0) return col;

    float curvedLuma = luma;
    if (luma < 0.35) {
        curvedLuma = pow(luma / 0.35, 1.20) * 0.35;
    } else {
        float t = (luma - 0.35) / 0.65;
        curvedLuma = 0.35 + pow(t, 0.88) * 0.65;
    }

    return col * (curvedLuma / max(luma, 0.001));
}

vec3 applyVibrance(vec3 col, float amount) {
    float luma = getLuminance(col);
    float maxC = max(col.r, max(col.g, col.b));
    float minC = min(col.r, min(col.g, col.b));
    float sat = (maxC - minC) / (maxC + 0.001);

    float boost = amount * (1.0 - sat * 0.5);
    return mix(vec3(luma), col, 1.0 + boost);
}

void main() {
    float useTextures = fragRenderFlags.x;
    float wirePass    = fragRenderFlags.y; // 0 = solid, 1 = wireframe overlay, 2 = wireframe only
    float matType     = fragRenderFlags.z; // 0 = raw texture, 1 = car paint, 2 = glass, 3 = rubber/dark, 4 = alloy/chrome
    float isMap       = fragRenderFlags.w;

    vec3 N = normalize(fragNormal);
    if (!gl_FrontFacing) N = -N;
    vec3 V = normalize(fragEyePos - fragWorldPos);

    // 2D UI Sidebar / TabBar Pass
    if (wirePass > 2.5) {
        outColor = texture(texSampler, fragTexCoord);
        return;
    }

    vec4 tex = (useTextures > 0.5) ? texture(texSampler, fragTexCoord) : vec4(1.0);

    // Texture Pack / TrackMap Showcase Pass (matType == 5.0)
    if (matType > 4.5 && wirePass < 0.5) {
        vec3 col = (useTextures > 0.5) ? tex.rgb : vec3(0.25, 0.27, 0.30);
        if (useTextures > 0.5 && tex.a < 0.99) {
            vec2 grid = floor(fragTexCoord * 20.0);
            float checker = mod(grid.x + grid.y, 2.0);
            vec3 bg = mix(vec3(0.11, 0.12, 0.15), vec3(0.18, 0.20, 0.24), checker);
            col = mix(bg, tex.rgb, max(tex.a, max(tex.r, max(tex.g, tex.b)) * 0.85));
        }
        outColor = vec4(col, 1.0);
        return;
    }

    // Wireframe Pass Rendering
    if (wirePass > 0.5) {
        float edgeShade = 0.55 + 0.45 * max(dot(N, V), 0.0);
        if (wirePass < 1.5) {
            // Mode 1: Crisp cyan-white wireframe overlay on top of solid + textured model
            vec3 overlayColor = mix(vec3(0.0, 0.90, 1.0), vec3(0.85, 0.98, 1.0), edgeShade);
            outColor = vec4(overlayColor, 1.0);
            return;
        } else {
            // Mode 2: Wireframe Only (uses texture/material color if textures enabled, or neon cyan wireframe)
            vec3 wireBase = vec3(0.0, 0.88, 1.0);
            if (useTextures > 0.5) {
                if (matType > 0.5 && matType < 1.5) {
                    wireBase = mix(fragCarColor.rgb * tex.rgb, vec3(0.2, 0.9, 1.0), 0.35);
                } else {
                    wireBase = mix(tex.rgb, vec3(0.0, 0.88, 1.0), 0.30);
                }
            } else if (matType > 0.5 && matType < 1.5) {
                wireBase = mix(fragCarColor.rgb, vec3(0.0, 0.88, 1.0), 0.40);
            }
            outColor = vec4(wireBase * (0.65 + 0.55 * edgeShade), 1.0);
            return;
        }
    }

    // Three-point Underground style cinematic lighting
    vec3 lightDir1 = normalize(vec3(0.5, 1.2, 0.8));
    vec3 lightColor1 = KELVIN_3197 * 1.35;

    vec3 lightDir2 = normalize(vec3(-0.8, 0.3, -0.6));
    vec3 lightColor2 = vec3(0.25, 0.70, 1.0) * 0.80;

    vec3 lightDir3 = normalize(vec3(0.0, -1.0, 0.2));
    vec3 lightColor3 = vec3(0.85, 0.15, 0.55) * 0.40;

    vec3 ambient = vec3(0.14, 0.15, 0.18);

    float diff1 = max(dot(N, lightDir1), 0.0);
    float diff2 = max(dot(N, lightDir2), 0.0);
    float diff3 = max(dot(N, lightDir3), 0.0);

    vec3 H1 = normalize(lightDir1 + V);
    float spec1_sharp = pow(max(dot(N, H1), 0.0), 64.0);
    float spec1_broad = pow(max(dot(N, H1), 0.0), 16.0);

    vec3 H2 = normalize(lightDir2 + V);
    float spec2 = pow(max(dot(N, H2), 0.0), 24.0);

    float NdotV = max(dot(N, V), 0.0);
    float fresnel = 0.04 + (1.0 - 0.04) * pow(1.0 - NdotV, 4.0);

    vec3 baseColor = vec3(0.8);
    float specScale = 1.0;
    float reflScale = 1.0;

    if (isMap > 0.5) {
        // Bayview Map District
        vec3 vCol = clamp(fragColor.rgb, 0.25, 1.0);
        baseColor = (useTextures > 0.5) ? (tex.rgb * mix(vec3(1.0), vCol, 0.35)) : vCol;
        specScale = 0.25;
        reflScale = 0.15;
    } else if (matType > 0.5 && matType < 1.5) {
        // Material 1: Car Paint (Body, Hood, Bumpers, Side Skirts, Spoiler, Mirrors)
        baseColor = fragCarColor.rgb * tex.rgb;
        float vertBrightness = (fragColor.r + fragColor.g + fragColor.b) / 3.0;
        if (vertBrightness < 0.55) {
            baseColor *= (0.25 + 0.75 * (vertBrightness / 0.55));
        }
        specScale = 0.6 + 0.8 * fragCarColor.a;
        reflScale = 1.35;
    } else if (matType > 1.5 && matType < 2.5) {
        // Material 2: Glass / Windows / Windshield
        baseColor = (useTextures > 0.5) ? (tex.rgb * vec3(0.18, 0.24, 0.30)) : vec3(0.08, 0.12, 0.16);
        specScale = 1.6;
        reflScale = 1.8;
    } else if (matType > 2.5 && matType < 3.5) {
        // Material 3: Rubber Tire / Undercarriage / Black Trim
        baseColor = (useTextures > 0.5) ? tex.rgb : vec3(0.14, 0.14, 0.16);
        specScale = 0.18;
        reflScale = 0.08;
    } else if (matType > 3.5) {
        // Material 4: Alloy Wheel Rim / Brake Disc / Exhaust Chrome
        baseColor = (useTextures > 0.5) ? (tex.rgb * vec3(0.88, 0.90, 0.94)) : vec3(0.72, 0.75, 0.80);
        specScale = 1.3;
        reflScale = 1.1;
    } else {
        // Material 0: Direct Textured Car Part (Headlights, Taillights, Badges, Grille, Interior)
        baseColor = (useTextures > 0.5) ? tex.rgb : clamp(fragColor.rgb, 0.2, 0.9);
        specScale = 0.55;
        reflScale = 0.45;
    }

    vec3 diffuse = (diff1 * lightColor1 + diff2 * lightColor2 + diff3 * lightColor3) * baseColor;
    vec3 specular = ((spec1_sharp * 0.8 + spec1_broad * 0.4) * lightColor1 + (spec2 * 0.5) * lightColor2) * specScale;
    vec3 reflection = mix(baseColor * 0.4, vec3(0.85, 0.95, 1.0), 0.75) * fresnel * reflScale;

    vec3 hdrColor = ambient * baseColor + diffuse + specular + reflection;

    // Bloom
    float luma = getLuminance(hdrColor);
    if (luma > 0.28) {
        float bloomFactor = (luma - 0.28) * 0.45;
        hdrColor += KELVIN_3197 * bloomFactor * 0.5;
    }

    // Filmic ACES Tone Mapping
    vec3 a = hdrColor * (hdrColor * 2.51 + vec3(0.03));
    vec3 b = hdrColor * (hdrColor * 2.43 + vec3(0.59)) + vec3(0.14);
    vec3 mapped = clamp(a / b, 0.0, 1.0);

    mapped = applyLightroomCurve(mapped);
    mapped = applyVibrance(mapped, 0.15);
    mapped = pow(mapped, vec3(1.0 / 2.2));

    outColor = vec4(mapped, 1.0);
}
