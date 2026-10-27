//! Safe Rust bindings for Cuervo Engine native plugins (docs/PLUGINS.md).
//!
//! A plugin is a package whose `@native` Pluma declarations are implemented
//! here. Register them in the `cuervo_plugin!` body; argument and result
//! types follow the declaration: `int` is `i32`, `i64` `i64`, `float` `f32`,
//! `f64` `f64`, `bool` `bool`, `str` and `name` `String`, `vec2`..`vec4`
//! `[f32; 2]`..`[f32; 4]`, a class or struct [`Object`]. A Rust panic in a
//! native becomes a script panic.
//!
//! ```ignore
//! cuervo_plugin::cuervo_plugin!(noise, |plugin| {
//!     plugin.function("value", |x: i32, y: i32| hash(x, y) as f32 / u32::MAX as f32);
//! });
//! ```

pub mod sys;

use core::ffi::{c_char, c_void};
use std::ffi::CString;
use std::panic::{catch_unwind, AssertUnwindSafe};

/// One native call: its arguments and its result.
pub struct Call {
    host: &'static sys::cuervo_host,
    raw: *mut sys::cuervo_call,
}

impl Call {
    pub fn len(&self) -> u32 {
        unsafe { (self.host.arg_count.unwrap())(self.raw) }
    }
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
    pub fn int(&self, i: u32) -> i64 {
        unsafe { (self.host.arg_int.unwrap())(self.raw, i) }
    }
    pub fn float(&self, i: u32) -> f64 {
        unsafe { (self.host.arg_float.unwrap())(self.raw, i) }
    }
    pub fn bool(&self, i: u32) -> bool {
        unsafe { (self.host.arg_bool.unwrap())(self.raw, i) != 0 }
    }
    pub fn is_null(&self, i: u32) -> bool {
        unsafe { (self.host.arg_is_null.unwrap())(self.raw, i) != 0 }
    }
    /// A `str` or `name` argument (its UTF-8 text).
    pub fn str(&self, i: u32) -> String {
        let mut n = 0usize;
        unsafe {
            let p = (self.host.arg_str.unwrap())(self.raw, i, &mut n);
            if p.is_null() || n == 0 {
                return String::new();
            }
            let bytes = std::slice::from_raw_parts(p as *const u8, n);
            String::from_utf8_lossy(bytes).into_owned()
        }
    }
    pub fn vec<const N: usize>(&self, i: u32) -> [f32; N] {
        let mut out = [0.0f32; N];
        unsafe { (self.host.arg_vec.unwrap())(self.raw, i, out.as_mut_ptr(), N as u32) };
        out
    }
    /// A class or struct argument; `None` for null.
    pub fn object(&self, i: u32) -> Option<Object> {
        let o = unsafe { (self.host.arg_object.unwrap())(self.raw, i) };
        if o.is_null() {
            None
        } else {
            Some(Object { host: self.host, raw: o })
        }
    }
    /// Ends the call with a script panic.
    pub fn panic(&self, message: &str) {
        let m = CString::new(message.replace('\0', " ")).unwrap_or_default();
        unsafe { (self.host.panic.unwrap())(self.raw, m.as_ptr()) };
    }
}

/// A script object passed to a native. Valid during the call.
#[derive(Clone, Copy)]
pub struct Object {
    host: &'static sys::cuervo_host,
    raw: *mut sys::cuervo_object,
}

fn cfield(name: &str) -> CString {
    CString::new(name).unwrap_or_default()
}

impl Object {
    pub fn type_name(&self) -> String {
        unsafe {
            let p = (self.host.object_type.unwrap())(self.raw);
            if p.is_null() {
                String::new()
            } else {
                std::ffi::CStr::from_ptr(p).to_string_lossy().into_owned()
            }
        }
    }
    pub fn get_float(&self, field: &str) -> Option<f64> {
        let mut v = 0.0f64;
        let f = cfield(field);
        let ok = unsafe { (self.host.object_get_float.unwrap())(self.raw, f.as_ptr(), &mut v) } == 0;
        ok.then_some(v)
    }
    pub fn set_float(&self, field: &str, v: f64) -> bool {
        let f = cfield(field);
        unsafe { (self.host.object_set_float.unwrap())(self.raw, f.as_ptr(), v) == 0 }
    }
    pub fn get_int(&self, field: &str) -> Option<i64> {
        let mut v = 0i64;
        let f = cfield(field);
        let ok = unsafe { (self.host.object_get_int.unwrap())(self.raw, f.as_ptr(), &mut v) } == 0;
        ok.then_some(v)
    }
    pub fn set_int(&self, field: &str, v: i64) -> bool {
        let f = cfield(field);
        unsafe { (self.host.object_set_int.unwrap())(self.raw, f.as_ptr(), v) == 0 }
    }
    pub fn get_vec<const N: usize>(&self, field: &str) -> Option<[f32; N]> {
        let mut out = [0.0f32; N];
        let mut n = 0u32;
        let f = cfield(field);
        let ok =
            unsafe { (self.host.object_get_vec.unwrap())(self.raw, f.as_ptr(), out.as_mut_ptr(), N as u32, &mut n) }
                == 0;
        (ok && n as usize == N).then_some(out)
    }
    pub fn set_vec<const N: usize>(&self, field: &str, v: [f32; N]) -> bool {
        let f = cfield(field);
        unsafe { (self.host.object_set_vec.unwrap())(self.raw, f.as_ptr(), v.as_ptr(), N as u32) == 0 }
    }
    /// Host data set with [`Object::set_native`], if any.
    ///
    /// # Safety
    /// `T` must be the type that was stored.
    pub unsafe fn native<T>(&self) -> Option<&mut T> {
        let p = (self.host.object_native.unwrap())(self.raw) as *mut T;
        p.as_mut()
    }
    /// Attaches `data`; the class's finalizer ([`Registrar::finalizer`]) drops it.
    pub fn set_native<T>(&self, data: Box<T>) {
        unsafe { (self.host.object_set_native.unwrap())(self.raw, Box::into_raw(data) as *mut c_void) };
    }
}

/// An argument type.
pub trait FromArg: Sized {
    fn from_arg(call: &Call, i: u32) -> Self;
}
impl FromArg for i32 {
    fn from_arg(c: &Call, i: u32) -> Self {
        c.int(i) as i32
    }
}
impl FromArg for i64 {
    fn from_arg(c: &Call, i: u32) -> Self {
        c.int(i)
    }
}
impl FromArg for f32 {
    fn from_arg(c: &Call, i: u32) -> Self {
        c.float(i) as f32
    }
}
impl FromArg for f64 {
    fn from_arg(c: &Call, i: u32) -> Self {
        c.float(i)
    }
}
impl FromArg for bool {
    fn from_arg(c: &Call, i: u32) -> Self {
        c.bool(i)
    }
}
impl FromArg for String {
    fn from_arg(c: &Call, i: u32) -> Self {
        c.str(i)
    }
}
impl<const N: usize> FromArg for [f32; N] {
    fn from_arg(c: &Call, i: u32) -> Self {
        c.vec::<N>(i)
    }
}
impl FromArg for Object {
    fn from_arg(c: &Call, i: u32) -> Self {
        match c.object(i) {
            Some(o) => o,
            None => panic!("argument {i} is null"),
        }
    }
}
impl FromArg for Option<Object> {
    fn from_arg(c: &Call, i: u32) -> Self {
        c.object(i)
    }
}

/// A result type.
pub trait IntoRet {
    fn ret(self, call: &Call);
}
impl IntoRet for () {
    fn ret(self, _: &Call) {}
}
impl IntoRet for i32 {
    fn ret(self, c: &Call) {
        unsafe { (c.host.ret_i32.unwrap())(c.raw, self) }
    }
}
impl IntoRet for i64 {
    fn ret(self, c: &Call) {
        unsafe { (c.host.ret_i64.unwrap())(c.raw, self) }
    }
}
impl IntoRet for f32 {
    fn ret(self, c: &Call) {
        unsafe { (c.host.ret_f32.unwrap())(c.raw, self) }
    }
}
impl IntoRet for f64 {
    fn ret(self, c: &Call) {
        unsafe { (c.host.ret_f64.unwrap())(c.raw, self) }
    }
}
impl IntoRet for bool {
    fn ret(self, c: &Call) {
        unsafe { (c.host.ret_bool.unwrap())(c.raw, self as i32) }
    }
}
impl IntoRet for String {
    fn ret(self, c: &Call) {
        self.as_str().ret(c)
    }
}
impl IntoRet for &str {
    fn ret(self, c: &Call) {
        unsafe { (c.host.ret_str.unwrap())(c.raw, self.as_ptr() as *const c_char, self.len()) }
    }
}
impl<const N: usize> IntoRet for [f32; N] {
    fn ret(self, c: &Call) {
        unsafe { (c.host.ret_vec.unwrap())(c.raw, self.as_ptr(), N as u32) }
    }
}
impl IntoRet for Object {
    fn ret(self, c: &Call) {
        unsafe { (c.host.ret_object.unwrap())(c.raw, self.raw) }
    }
}

/// A Rust function usable as a native: `Fn(A, B, ...) -> R` with
/// [`FromArg`] arguments and an [`IntoRet`] result.
pub trait NativeFn<Args>: Send + Sync + 'static {
    fn call(&self, call: &Call);
}

macro_rules! native_fn {
    ($($a:ident $i:tt),*) => {
        impl<F, R, $($a,)*> NativeFn<($($a,)*)> for F
        where
            F: Fn($($a),*) -> R + Send + Sync + 'static,
            R: IntoRet,
            $($a: FromArg,)*
        {
            #[allow(unused_variables)]
            fn call(&self, call: &Call) {
                let want: u32 = 0 $(+ { let _ = $i; 1 })*;
                if call.len() < want {
                    call.panic("native called with too few arguments");
                    return;
                }
                (self)($($a::from_arg(call, $i)),*).ret(call);
            }
        }
    };
}
native_fn!();
native_fn!(A 0);
native_fn!(A 0, B 1);
native_fn!(A 0, B 1, C 2);
native_fn!(A 0, B 1, C 2, D 3);
native_fn!(A 0, B 1, C 2, D 3, E 4);
native_fn!(A 0, B 1, C 2, D 3, E 4, G 5);
native_fn!(A 0, B 1, C 2, D 3, E 4, G 5, H 6);
native_fn!(A 0, B 1, C 2, D 3, E 4, G 5, H 6, I 7);

struct Holder {
    host: &'static sys::cuervo_host,
    f: Box<dyn Fn(&Call) + Send + Sync>,
}

unsafe extern "C" fn trampoline(raw: *mut sys::cuervo_call, user: *mut c_void) {
    let h = &*(user as *const Holder);
    let call = Call { host: h.host, raw };
    if let Err(e) = catch_unwind(AssertUnwindSafe(|| (h.f)(&call))) {
        let msg = if let Some(s) = e.downcast_ref::<&str>() {
            (*s).to_string()
        } else if let Some(s) = e.downcast_ref::<String>() {
            s.clone()
        } else {
            "Rust panic in a plugin native".to_string()
        };
        call.panic(&msg);
    }
}

type ImportFn = dyn Fn(&str, &[u8]) -> Result<Vec<u8>, String> + Send + Sync;

struct ImportHolder {
    host: &'static sys::cuervo_host,
    f: Box<ImportFn>,
}

unsafe extern "C" fn import_trampoline(
    user: *mut c_void,
    path: *const c_char,
    data: *const u8,
    size: usize,
    out: *mut sys::cuervo_buffer,
) -> i32 {
    let h = &*(user as *const ImportHolder);
    let p = std::ffi::CStr::from_ptr(path).to_string_lossy().into_owned();
    let bytes = if size == 0 { &[][..] } else { std::slice::from_raw_parts(data, size) };
    let result = catch_unwind(AssertUnwindSafe(|| (h.f)(&p, bytes)))
        .unwrap_or_else(|_| Err("Rust panic in a plugin importer".to_string()));
    let (code, text) = match result {
        Ok(v) => (0, v),
        Err(m) => (1, m.into_bytes()),
    };
    (h.host.buffer_write.unwrap())(out, text.as_ptr(), text.len());
    code
}

/// What a plugin registers, in the `cuervo_plugin!` body. Names are relative
/// to the plugin's namespace: `"value"` registers `"<plugin>.value"`.
pub struct Registrar {
    host: &'static sys::cuervo_host,
    reg: *mut sys::cuervo_registrar,
    prefix: String,
    failed: bool,
}

impl Registrar {
    #[doc(hidden)]
    pub unsafe fn new(host: *const sys::cuervo_host, reg: *mut sys::cuervo_registrar, name: &str) -> Self {
        Registrar { host: &*host, reg, prefix: format!("{name}."), failed: false }
    }

    /// A native implemented by `f`.
    pub fn function<Args, F: NativeFn<Args>>(&mut self, name: &str, f: F) {
        self.raw(name, move |call: &Call| f.call(call));
    }

    /// A native with raw access to its call.
    pub fn raw(&mut self, name: &str, f: impl Fn(&Call) + Send + Sync + 'static) {
        let holder = Box::into_raw(Box::new(Holder { host: self.host, f: Box::new(f) }));
        let full = cfield(&format!("{}{}", self.prefix, name));
        let code =
            unsafe { (self.host.register_native.unwrap())(self.reg, full.as_ptr(), Some(trampoline), holder as *mut c_void) };
        if code != 0 {
            self.failed = true;
        }
    }

    /// Drops the `Box<T>` attached with [`Object::set_native`] when an
    /// instance of `class` (relative to the namespace) dies.
    pub fn finalizer<T>(&mut self, class: &str) {
        unsafe extern "C" fn drop_box<T>(p: *mut c_void) {
            if !p.is_null() {
                drop(Box::from_raw(p as *mut T));
            }
        }
        let full = cfield(&format!("{}{}", self.prefix, class));
        if unsafe { (self.host.register_finalizer.unwrap())(self.reg, full.as_ptr(), Some(drop_box::<T>)) } != 0 {
            self.failed = true;
        }
    }

    /// Converts `extension` files to `output_extension` (an engine format).
    pub fn importer(
        &mut self,
        extension: &str,
        output_extension: &str,
        f: impl Fn(&str, &[u8]) -> Result<Vec<u8>, String> + Send + Sync + 'static,
    ) {
        let holder = Box::into_raw(Box::new(ImportHolder { host: self.host, f: Box::new(f) }));
        let (e, o) = (cfield(extension), cfield(output_extension));
        let code = unsafe {
            (self.host.register_importer.unwrap())(
                self.reg,
                e.as_ptr(),
                o.as_ptr(),
                Some(import_trampoline),
                holder as *mut c_void,
            )
        };
        if code != 0 {
            self.failed = true;
        }
    }

    pub fn log(&self, message: &str) {
        let m = cfield(message);
        unsafe { (self.host.log.unwrap())(sys::CUERVO_LOG_INFO, m.as_ptr()) };
    }

    #[doc(hidden)]
    pub fn failed(&self) -> bool {
        self.failed
    }
}

#[doc(hidden)]
pub unsafe fn entry(
    host: *const sys::cuervo_host,
    reg: *mut sys::cuervo_registrar,
    name: &str,
    body: impl FnOnce(&mut Registrar),
) -> i32 {
    if host.is_null() || (*host).abi_version != sys::CUERVO_PLUGIN_ABI_VERSION {
        return 1;
    }
    let mut r = Registrar::new(host, reg, name);
    match catch_unwind(AssertUnwindSafe(|| body(&mut r))) {
        Ok(()) if !r.failed() => 0,
        Ok(()) => 3,
        Err(_) => 2,
    }
}

/// Defines the plugin's entry point, `cuervo_plugin_entry_<name>`.
#[macro_export]
macro_rules! cuervo_plugin {
    ($name:ident, |$plugin:ident| $body:block) => {
        #[export_name = concat!("cuervo_plugin_entry_", stringify!($name))]
        pub unsafe extern "C" fn cuervo_plugin_entry(
            host: *const $crate::sys::cuervo_host,
            reg: *mut $crate::sys::cuervo_registrar,
        ) -> i32 {
            $crate::entry(host, reg, stringify!($name), |$plugin: &mut $crate::Registrar| $body)
        }
    };
}
