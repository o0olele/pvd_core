Shader "PVD/Playback Preview"
{
    Properties
    {
        _Color ("Color", Color) = (0.34, 0.68, 0.78, 1)
    }

    CGINCLUDE
    #include "UnityCG.cginc"

    struct Attributes
    {
        float4 position : POSITION;
        float3 normal : NORMAL;
    };

    struct Varyings
    {
        float4 position : SV_POSITION;
        float3 normal : TEXCOORD0;
    };

    half4 _Color;

    Varyings PreviewVertex(Attributes input)
    {
        Varyings output;
        output.position = UnityObjectToClipPos(input.position);
        output.normal = UnityObjectToWorldNormal(input.normal);
        return output;
    }

    half4 PreviewFragment(Varyings input) : SV_Target
    {
        half3 normal = normalize(input.normal);
        half lighting = 0.38 + 0.62 * saturate(dot(normal, half3(0.36, 0.80, 0.48)));
        return half4(_Color.rgb * lighting, 1);
    }
    ENDCG

    SubShader
    {
        Tags { "RenderPipeline" = "UniversalPipeline" "RenderType" = "Opaque" }
        Pass
        {
            Tags { "LightMode" = "SRPDefaultUnlit" }
            CGPROGRAM
            #pragma vertex PreviewVertex
            #pragma fragment PreviewFragment
            ENDCG
        }
    }

    SubShader
    {
        Tags { "RenderType" = "Opaque" }
        Pass
        {
            CGPROGRAM
            #pragma vertex PreviewVertex
            #pragma fragment PreviewFragment
            ENDCG
        }
    }
}
