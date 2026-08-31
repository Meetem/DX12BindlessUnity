using UnityEngine;
using UnityEngine.Experimental.Rendering;
using UnityEngine.Rendering;
using UnityEngine.UI;

namespace Meetem.Bindless
{
    public class TestBindlessCompute : MonoBehaviour
    {
        [SerializeField]
        protected RawImage preview;
    
        public ComputeShader shader;
        private CommandBuffer cmdBuffer;
        private RenderTexture rt;
        public bool renderToImage = false;
        
        private int currentArgs = 0;
        private int currentArgsTextures = 0;
        
        void Awake()
        {
            currentArgs = 0;
            
            cmdBuffer = new CommandBuffer();
            rt = new RenderTexture(512, 512, GraphicsFormat.R8G8B8A8_UNorm, GraphicsFormat.None, 1);
            rt.enableRandomWrite = true;
            rt.filterMode = FilterMode.Point;
            rt.Create();
            preview.texture = rt;
        }
    
        protected void OnDisable()
        {
            rt.Release();
            cmdBuffer.Dispose();
        }
    
        private bool hooked = false;
        protected void Update()
        {
            if (!hooked)
            {
                cmdBuffer.Clear();
                Graphics.ExecuteCommandBuffer(cmdBuffer);
                hooked = true;
                return;
            }
            
            cmdBuffer.Clear();
            
            // we must set the texture here,
            // otherwise Unity will complain.
            cmdBuffer.SetComputeTextureParam(shader, 0, "Texture2DTable", Texture2D.blackTexture);
            
            cmdBuffer.SetComputeTextureParam(shader, 0, "Result", rt);
            cmdBuffer.DispatchCompute(shader, 0, 16, 16, 1);
            //cmdBuffer.DispatchCompute(shader, 0, 16, 16, 1);
    
            if (renderToImage && Time.frameCount >= 5)
            {
                Graphics.ExecuteCommandBuffer(cmdBuffer);
            }
        }
    }
}
