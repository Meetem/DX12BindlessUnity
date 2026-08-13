Shader "Unlit/DrawWithBindless"
{
    Properties
    {
        _MainTex ("Texture", 2D) = "white" {}
        numTextures("Num Textures", Float) = 2
        baseTexture("Base Texture", Float) = 0
    }
    SubShader
    {
        Tags { "RenderType"="Transparent" "Queue"="Transparent" }
        LOD 100
        Blend SrcAlpha OneMinusSrcAlpha
        
        Pass
        {
            CGPROGRAM
            // Only DX12 for bindless (Unity doesn't make a difference between DX11/DX12 here tho)
            // to also support other renderers you can use another Subshader probably.
            #pragma only_renderers d3d11 
            #pragma vertex vert
            #pragma fragment frag
            #pragma use_dxc 
            
            #include "UnityCG.cginc"
            
            struct appdata
            {
                float4 vertex : POSITION;
                float2 uv : TEXCOORD0;
            };

            struct v2f
            {
                float2 uv : TEXCOORD0;
                float4 vertex : SV_POSITION;
            };

            // register t31 is registered to be the "bindless" slot
            // so you declare your bindless tables like that,
            // and descriptor patching will do the job.
            Texture2D TextureTable[2048] : register(t31, space0);

            // you can't get real samplers from textures,
            // so we must define our own.
            SamplerState my_linear_clamp_sampler;
            
            uint numTextures;
            int baseTexture;
            
            v2f vert (appdata v)
            {
                v2f o;
                float3 pos = v.vertex;
                
                o.vertex = UnityObjectToClipPos(pos);
                o.uv = v.uv;
                return o;
            }

            float4 frag (v2f i) : SV_Target
            {
                float2 textureIds = i.uv.xy * numTextures;
                int texIdFlat = int(int(textureIds.x) + int(textureIds.y) * numTextures) + baseTexture;
                texIdFlat = max(texIdFlat, 0);

                float4 v = TextureTable[NonUniformResourceIndex(texIdFlat)].Sample(my_linear_clamp_sampler, frac(i.uv.xy * numTextures));
                return v;
            }
            
            ENDCG
        }
    }
}
