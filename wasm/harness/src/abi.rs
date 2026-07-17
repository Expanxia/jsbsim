//! Rust mirrors of wasm/abi.h (v1). Field order/types MUST match exactly;
//! sizes are asserted against the module's jsb_abi_version at startup.
#![allow(dead_code)]

pub const JSB_ABI_VERSION: u32 = 1;

pub const JSB_OK: i32 = 0;
pub const JSB_ERR_BAD_HANDLE: i32 = -1;
pub const JSB_ERR_BAD_ARG: i32 = -2;
pub const JSB_ERR_EXCEPTION: i32 = -3;
pub const JSB_ERR_NOT_LOADED: i32 = -4;
pub const JSB_ERR_VFS_LIMIT: i32 = -5;
pub const JSB_ERR_STRUCT_SIZE: i32 = -6;
pub const JSB_ERR_LOAD_FAILED: i32 = -7;
pub const JSB_ERR_INIT_FAILED: i32 = -8;
pub const JSB_ERR_PROP: i32 = -9;
pub const JSB_ERR_NOT_FINITE: i32 = -10;

pub const JSB_MAX_ENGINES: usize = 4;

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct JsbCreateV1 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub dt_s: f64,
    pub debug_level: u32,
    pub _pad: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct JsbIcV1 {
    pub struct_size: u32,
    pub _pad: u32,
    pub lat_deg: f64,
    pub lon_deg: f64,
    pub alt_msl_m: f64,
    pub heading_true_deg: f64,
    pub airspeed_tas_mps: f64,
    pub gamma_deg: f64,
    pub gear_down: u32,
    pub engines_running: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct JsbInV1 {
    pub struct_size: u32,
    pub _pad: u32,
    pub aileron: f64,
    pub elevator: f64,
    pub rudder: f64,
    pub throttle: f64,
    pub flaps: f64,
    pub brake_left: f64,
    pub brake_right: f64,
    pub pitch_trim: f64,
    pub gear_down: u32,
    pub _pad2: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct JsbOutV1 {
    pub struct_size: u32,
    pub status_flags: u32,
    pub sim_time_s: f64,
    pub lat_deg: f64,
    pub lon_deg: f64,
    pub alt_msl_m: f64,
    pub alt_agl_m: f64,
    pub roll_rad: f64,
    pub pitch_rad: f64,
    pub yaw_rad: f64,
    pub q_w: f64,
    pub q_x: f64,
    pub q_y: f64,
    pub q_z: f64,
    pub vn_mps: f64,
    pub ve_mps: f64,
    pub vd_mps: f64,
    pub p_rps: f64,
    pub q_rps: f64,
    pub r_rps: f64,
    pub ax_mps2: f64,
    pub ay_mps2: f64,
    pub az_mps2: f64,
    pub alpha_rad: f64,
    pub beta_rad: f64,
    pub vtas_mps: f64,
    pub vcas_mps: f64,
    pub vground_mps: f64,
    pub engine_count: u32,
    pub _pad3: u32,
    pub thrust_n: [f64; JSB_MAX_ENGINES],
    pub engine_rpm: [f64; JSB_MAX_ENGINES],
    pub fuel_kg: f64,
    pub surf_aileron: f64,
    pub surf_elevator: f64,
    pub surf_rudder: f64,
    pub surf_flaps: f64,
    pub gear_pos: f64,
    pub ground_queries: u32,
    pub trapped: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct KiroGroundInV1 {
    pub struct_size: u32,
    pub _pad: u32,
    pub lat_deg: f64,
    pub lon_deg: f64,
    pub alt_msl_m: f64,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct KiroGroundOutV1 {
    pub struct_size: u32,
    pub status: u32,
    pub agl_m: f64,
    pub contact_ecef_m: [f64; 3],
    pub normal_ecef: [f64; 3],
    pub vel_ecef_mps: [f64; 3],
    pub angvel_ecef_rps: [f64; 3],
}

pub fn as_bytes<T: Copy>(v: &T) -> &[u8] {
    unsafe {
        std::slice::from_raw_parts(v as *const T as *const u8,
                                   std::mem::size_of::<T>())
    }
}

pub fn from_bytes<T: Copy + Default>(b: &[u8]) -> T {
    assert!(b.len() >= std::mem::size_of::<T>());
    let mut v = T::default();
    unsafe {
        std::ptr::copy_nonoverlapping(b.as_ptr(), &mut v as *mut T as *mut u8,
                                      std::mem::size_of::<T>());
    }
    v
}

/// WGS84 geodetic -> ECEF meters.
pub fn geodetic_to_ecef(lat_deg: f64, lon_deg: f64, h_m: f64) -> [f64; 3] {
    const A: f64 = 6378137.0;
    const E2: f64 = 6.694379990141316e-3;
    let lat = lat_deg.to_radians();
    let lon = lon_deg.to_radians();
    let n = A / (1.0 - E2 * lat.sin() * lat.sin()).sqrt();
    [
        (n + h_m) * lat.cos() * lon.cos(),
        (n + h_m) * lat.cos() * lon.sin(),
        (n * (1.0 - E2) + h_m) * lat.sin(),
    ]
}

/// Geodetic up unit vector in ECEF.
pub fn geodetic_up(lat_deg: f64, lon_deg: f64) -> [f64; 3] {
    let lat = lat_deg.to_radians();
    let lon = lon_deg.to_radians();
    [lat.cos() * lon.cos(), lat.cos() * lon.sin(), lat.sin()]
}
