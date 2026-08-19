struct Channel { vec4 resolution; float time; };
layout(set = 0, binding = 0) uniform RezonalityCommonUniforms
{
    float iTime;
    float iGlobalTime;
    float iTimeDelta;
    float iFrame;
    float iFrameRate;
    float iSampleRate;
    uint iSceneFlags;
    uint vertexSize;
    vec4 iResolution;
    vec4 iMouse;
    vec4 iDate;
    vec4 iSpectrumBands[2];
    vec4 iChannelTime;
    vec4 iChannelResolution[4];
    vec4 ifFragCoordOffsetUniform;
    vec4 eye;
    mat4 model;
    mat4 view;
    mat4 projection;
    mat4 modelViewProjection;
    mat4 viewInverse;
    mat4 projectionInverse;
    Channel iChannel[4];
} ubo;
