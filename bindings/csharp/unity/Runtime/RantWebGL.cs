// The tunnel a WebGL player reaches the mesh through, since a page cannot open UDP sockets: Rant
// compiled into the player runs on the sockets a Rant bridge lends (Rant's spec/tunnel.md).
#if UNITY_5_3_OR_NEWER
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using UnityEngine;

namespace Rant
{
    /// <summary>A WebGL player's tunnel to a Rant bridge, whose sockets its nodes open on. Every
    /// other player has sockets of its own and needs none.</summary>
    public static class RantWebGL
    {
        /// <summary>True in a WebGL player, where a node opens only once the tunnel is up.</summary>
        public static bool Needed => Application.platform == RuntimePlatform.WebGLPlayer;

#if UNITY_WEBGL && !UNITY_EDITOR
        private delegate void Done(int id, IntPtr error);
        [DllImport("__Internal")] private static extern void RantWebGLConnect(string url, string passphrase, string name, int id, Done done);
        [DllImport("__Internal")] private static extern void RantWebGLDisconnect();

        private static readonly Dictionary<int, Action<string>> s_pending = new Dictionary<int, Action<string>>();
        private static int s_next;

        [AOT.MonoPInvokeCallback(typeof(Done))]
        private static void OnDone(int id, IntPtr error)
        {
            if (!s_pending.TryGetValue(id, out Action<string> done)) return;
            s_pending.Remove(id);
            try { done(error == IntPtr.Zero ? null : Marshal.PtrToStringUTF8(error)); }
            catch (Exception e) { Debug.LogException(e); }
        }
#endif

        /// <summary>Opens the tunnel to the bridge at url, such as ws://192.168.1.20:7480, then calls
        /// done with null, or with why it failed. name is the session's name in the bridge's logs.</summary>
        public static void Connect(string url, string passphrase, string name, Action<string> done)
        {
#if UNITY_WEBGL && !UNITY_EDITOR
            int id = ++s_next;
            s_pending[id] = done;
            RantWebGLConnect(url, passphrase ?? "", name ?? "", id, OnDone);
#else
            done("a tunnel is for a WebGL player, a node here has sockets of its own");
#endif
        }

        /// <summary>Ends the tunnel, once its nodes closed.</summary>
        public static void Disconnect()
        {
#if UNITY_WEBGL && !UNITY_EDITOR
            RantWebGLDisconnect();
#endif
        }
    }
}
#endif
