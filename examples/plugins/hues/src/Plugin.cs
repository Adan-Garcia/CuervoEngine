// The hues plugin's natives (scripts/hues.plm), over the C# bindings.

using System;
using System.Numerics;
using System.Runtime.InteropServices;
using Cuervo.Plugin;
using Cuervo.Plugin.Abi;

namespace Hues;

public static class Plugin
{
    // A hue wheel: t in turns -> RGB in 0..1 (piecewise linear, no trig).
    private static Vector3 Hue(float t)
    {
        float h = (t - MathF.Floor(t)) * 6.0f;
        float Channel(float offset)
        {
            float k = (h + offset) % 6.0f;
            return Math.Clamp(Math.Min(k, 4.0f - k), 0.0f, 1.0f);
        }
        return new Vector3(Channel(5.0f), Channel(3.0f), Channel(1.0f));
    }

    [UnmanagedCallersOnly]
    public static unsafe int Entry(cuervo_host* host, IntPtr reg) =>
        Cuervo.Plugin.Entry.Run(host, reg, "hues", plugin =>
        {
            plugin.Function("describe", () => "hues 1.0.0 (C#)");
            plugin.Function("hue", (float t) => Hue(t));
            // Pulse.scale(): 1 +- 0.35 over the node's period, from its fields.
            plugin.Function("Pulse.scale", (ScriptObject self) =>
            {
                float t = (float)(self.GetFloat("t") ?? 0.0);
                float period = (float)(self.GetFloat("period") ?? 1.0);
                float phase = t / period - MathF.Floor(t / period);
                float tri = phase < 0.5f ? phase * 2.0f : 2.0f - phase * 2.0f;
                return 0.65f + 0.7f * tri;
            });
        });
}
