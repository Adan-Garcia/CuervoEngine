// C# bindings for Cuervo Engine native plugins (docs/PLUGINS.md). The engine
// hosts .NET on desktop and calls the plugin's entry method, which has the
// C entry point's signature:
//
//     public static class Plugin {
//         [UnmanagedCallersOnly]
//         public static unsafe int Entry(cuervo_host* host, IntPtr reg) =>
//             Cuervo.Plugin.Entry.Run(host, reg, "hues", plugin => {
//                 plugin.Function("describe", () => "hues 1.0.0 (C#)");
//             });
//     }
//
// Types follow the Pluma declaration: `int` is int, `i64` long, `float`
// float, `f64` double, `bool` bool, `str` and `name` string, `vec2`..`vec4`
// System.Numerics.Vector2..Vector4, a class or struct ScriptObject. An
// exception in a native becomes a script panic.

using System;
using System.Collections.Generic;
using System.Numerics;
using System.Runtime.InteropServices;
using System.Text;
using Cuervo.Plugin.Abi;

namespace Cuervo.Plugin;

// One native call: its arguments and its result.
public readonly unsafe struct Call
{
    private readonly cuervo_host* _host;
    private readonly IntPtr _raw;

    internal Call(cuervo_host* host, IntPtr raw)
    {
        _host = host;
        _raw = raw;
    }

    public uint Count => _host->arg_count(_raw);
    public long Int(uint i) => _host->arg_int(_raw, i);
    public double Float(uint i) => _host->arg_float(_raw, i);
    public bool Bool(uint i) => _host->arg_bool(_raw, i) != 0;
    public bool IsNull(uint i) => _host->arg_is_null(_raw, i) != 0;

    public string Str(uint i)
    {
        nuint n = 0;
        byte* p = _host->arg_str(_raw, i, &n);
        return p == null ? "" : Encoding.UTF8.GetString(p, checked((int)n));
    }

    public float[] Vec(uint i, int size)
    {
        var v = new float[size];
        fixed (float* p = v) _host->arg_vec(_raw, i, p, (uint)size);
        return v;
    }

    public ScriptObject? Object(uint i)
    {
        IntPtr o = _host->arg_object(_raw, i);
        return o == IntPtr.Zero ? null : new ScriptObject(_host, o);
    }

    public void Ret(bool v) => _host->ret_bool(_raw, v ? 1 : 0);
    public void Ret(int v) => _host->ret_i32(_raw, v);
    public void Ret(long v) => _host->ret_i64(_raw, v);
    public void Ret(float v) => _host->ret_f32(_raw, v);
    public void Ret(double v) => _host->ret_f64(_raw, v);

    public void Ret(string v)
    {
        byte[] b = Encoding.UTF8.GetBytes(v);
        fixed (byte* p = b) _host->ret_str(_raw, p, (nuint)b.Length);
    }

    public void Ret(ReadOnlySpan<float> v)
    {
        fixed (float* p = v) _host->ret_vec(_raw, p, (uint)v.Length);
    }

    public void Ret(ScriptObject o) => _host->ret_object(_raw, o.Raw);

    public void Panic(string message)
    {
        byte[] b = Encoding.UTF8.GetBytes(message + "\0");
        fixed (byte* p = b) _host->panic(_raw, p);
    }
}

// A script object passed to a native. Valid during the call.
public readonly unsafe struct ScriptObject
{
    private readonly cuervo_host* _host;
    internal IntPtr Raw { get; }

    internal ScriptObject(cuervo_host* host, IntPtr raw)
    {
        _host = host;
        Raw = raw;
    }

    private static byte[] C(string s) => Encoding.UTF8.GetBytes(s + "\0");

    public string TypeName => Marshal.PtrToStringUTF8((IntPtr)_host->object_type(Raw)) ?? "";

    public double? GetFloat(string field)
    {
        double v = 0;
        fixed (byte* f = C(field)) return _host->object_get_float(Raw, f, &v) == 0 ? v : null;
    }

    public bool SetFloat(string field, double v)
    {
        fixed (byte* f = C(field)) return _host->object_set_float(Raw, f, v) == 0;
    }

    public long? GetInt(string field)
    {
        long v = 0;
        fixed (byte* f = C(field)) return _host->object_get_int(Raw, f, &v) == 0 ? v : null;
    }

    public bool SetInt(string field, long v)
    {
        fixed (byte* f = C(field)) return _host->object_set_int(Raw, f, v) == 0;
    }

    public float[]? GetVec(string field, int size)
    {
        var v = new float[size];
        uint n = 0;
        fixed (byte* f = C(field))
        fixed (float* p = v)
        {
            return _host->object_get_vec(Raw, f, p, (uint)size, &n) == 0 && n == size ? v : null;
        }
    }

    public bool SetVec(string field, ReadOnlySpan<float> v)
    {
        fixed (byte* f = C(field))
        fixed (float* p = v)
        {
            return _host->object_set_vec(Raw, f, p, (uint)v.Length) == 0;
        }
    }
}

// What a plugin registers, in Entry.Run's body. Names are relative to the
// plugin's namespace: "value" registers "<plugin>.value".
public sealed unsafe class Registrar
{
    private readonly cuervo_host* _host;
    private readonly IntPtr _reg;
    private readonly string _prefix;

    // Registered delegates live as long as the process (.NET is not unloaded).
    private static readonly List<GCHandle> Handles = new();

    internal bool Failed { get; private set; }

    internal Registrar(cuervo_host* host, IntPtr reg, string name)
    {
        _host = host;
        _reg = reg;
        _prefix = name + ".";
    }

    [UnmanagedCallersOnly]
    private static void Trampoline(IntPtr call, void* user)
    {
        var entry = (Native)GCHandle.FromIntPtr((IntPtr)user).Target!;
        var c = new Call(entry.Host, call);
        try
        {
            entry.F(c);
        }
        catch (Exception e)
        {
            c.Panic($"{e.GetType().Name}: {e.Message}");
        }
    }

    private sealed class Native
    {
        public required cuervo_host* Host;
        public required Action<Call> F;
    }

    // A native with raw access to its call.
    public void Raw(string name, Action<Call> f)
    {
        var handle = GCHandle.Alloc(new Native { Host = _host, F = f });
        Handles.Add(handle);
        byte[] full = Encoding.UTF8.GetBytes(_prefix + name + "\0");
        delegate* unmanaged<IntPtr, void*, void> tramp = &Trampoline;
        fixed (byte* p = full)
        {
            if (_host->register_native(_reg, p, (IntPtr)tramp, (void*)GCHandle.ToIntPtr(handle)) != 0) Failed = true;
        }
    }

    private static T Arg<T>(Call c, uint i)
    {
        object v = typeof(T) switch
        {
            var t when t == typeof(int) => (int)c.Int(i),
            var t when t == typeof(long) => c.Int(i),
            var t when t == typeof(float) => (float)c.Float(i),
            var t when t == typeof(double) => c.Float(i),
            var t when t == typeof(bool) => c.Bool(i),
            var t when t == typeof(string) => c.Str(i),
            var t when t == typeof(Vector2) => ToVector2(c.Vec(i, 2)),
            var t when t == typeof(Vector3) => ToVector3(c.Vec(i, 3)),
            var t when t == typeof(Vector4) => ToVector4(c.Vec(i, 4)),
            var t when t == typeof(ScriptObject) => c.Object(i) ?? throw new ArgumentNullException($"argument {i}"),
            _ => throw new NotSupportedException($"unsupported native argument type {typeof(T).Name}"),
        };
        return (T)v;
    }

    private static Vector2 ToVector2(float[] v) => new(v[0], v[1]);
    private static Vector3 ToVector3(float[] v) => new(v[0], v[1], v[2]);
    private static Vector4 ToVector4(float[] v) => new(v[0], v[1], v[2], v[3]);

    private static void Ret<R>(Call c, R r)
    {
        switch (r)
        {
            case int x: c.Ret(x); break;
            case long x: c.Ret(x); break;
            case float x: c.Ret(x); break;
            case double x: c.Ret(x); break;
            case bool x: c.Ret(x); break;
            case string x: c.Ret(x); break;
            case Vector2 x: c.Ret(stackalloc float[] { x.X, x.Y }); break;
            case Vector3 x: c.Ret(stackalloc float[] { x.X, x.Y, x.Z }); break;
            case Vector4 x: c.Ret(stackalloc float[] { x.X, x.Y, x.Z, x.W }); break;
            case ScriptObject x: c.Ret(x); break;
            default: throw new NotSupportedException($"unsupported native result type {typeof(R).Name}");
        }
    }

    private static void Need(Call c, uint n)
    {
        if (c.Count < n) throw new ArgumentException("native called with too few arguments");
    }

    public void Function(string name, Action f) => Raw(name, c => f());
    public void Function<R>(string name, Func<R> f) => Raw(name, c => Ret(c, f()));
    public void Function<A, R>(string name, Func<A, R> f) =>
        Raw(name, c => { Need(c, 1); Ret(c, f(Arg<A>(c, 0))); });
    public void Function<A, B, R>(string name, Func<A, B, R> f) =>
        Raw(name, c => { Need(c, 2); Ret(c, f(Arg<A>(c, 0), Arg<B>(c, 1))); });
    public void Function<A, B, C, R>(string name, Func<A, B, C, R> f) =>
        Raw(name, c => { Need(c, 3); Ret(c, f(Arg<A>(c, 0), Arg<B>(c, 1), Arg<C>(c, 2))); });
    public void Function<A, B, C, D, R>(string name, Func<A, B, C, D, R> f) =>
        Raw(name, c => { Need(c, 4); Ret(c, f(Arg<A>(c, 0), Arg<B>(c, 1), Arg<C>(c, 2), Arg<D>(c, 3))); });

    public void Log(string message)
    {
        byte[] b = Encoding.UTF8.GetBytes(message + "\0");
        fixed (byte* p = b) _host->log(Constants.CUERVO_LOG_INFO, p);
    }
}

public static unsafe class Entry
{
    // The body of a plugin's [UnmanagedCallersOnly] entry method.
    public static int Run(cuervo_host* host, IntPtr reg, string name, Action<Registrar> body)
    {
        if (host == null || host->abi_version != Constants.CUERVO_PLUGIN_ABI_VERSION) return 1;
        var r = new Registrar(host, reg, name);
        try
        {
            body(r);
        }
        catch (Exception)
        {
            return 2;
        }
        return r.Failed ? 3 : 0;
    }
}
