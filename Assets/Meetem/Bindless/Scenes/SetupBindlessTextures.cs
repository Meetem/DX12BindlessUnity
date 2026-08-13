using System.Runtime.InteropServices;
using UnityEngine;

namespace Meetem.Bindless
{
    [ExecuteInEditMode]
    public class SetupBindlessTextures : MonoBehaviour
    {
        private BindlessTexture[] bindlessTextures;
        private GCHandle pinnedHandle;
        
        [SerializeField]
        protected Texture2D[] testTextures;

        void Awake()
        {
            bindlessTextures = new BindlessTexture[1024];
            pinnedHandle = GCHandle.Alloc(bindlessTextures, GCHandleType.Pinned);
            
            for (int i = 0; i < testTextures.Length; i++)
                bindlessTextures[i] = BindlessTexture.FromTexture2D(testTextures[i]);
            
            bindlessTextures.SetBindlessTextures(0);
        }

        protected void OnDestroy()
        {
            if(pinnedHandle.IsAllocated)
                pinnedHandle.Free();

            bindlessTextures = null;
        }
    }
}