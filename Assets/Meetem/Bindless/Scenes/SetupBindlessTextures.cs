using UnityEngine;

namespace Meetem.Bindless
{
    [ExecuteInEditMode]
    public class SetupBindlessTextures : MonoBehaviour
    {
        private BindlessTexture[] bindlessTextures;

        [SerializeField]
        protected Texture2D[] testTextures;

        void Awake()
        {
            bindlessTextures = new BindlessTexture[1024];
            for (int i = 0; i < testTextures.Length; i++)
                bindlessTextures[i] = BindlessTexture.FromTexture2D(testTextures[i]);
            
            bindlessTextures.SetBindlessTextures(0);
        }

        void Update(){
            
        }
    }
}