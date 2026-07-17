//! jsbsim.wasm contract-test + validation harness (Phase 1 exit gate).
//!
//! Usage (from wasm/):  cargo run --release --manifest-path harness/Cargo.toml
//! Optional args: --module <path> --pack <path> --ref <csv> --out <csv>
mod abi;
mod wasi_stubs;

use abi::*;
use anyhow::{bail, Context, Result};
use wasi_stubs::HostState;
use wasmtime::{Caller, Config, Engine, Instance, Linker, Module, Store,
               TypedFunc};

struct Jsb {
    store: Store<HostState>,
    memory: wasmtime::Memory,
    f_create: TypedFunc<i32, i32>,
    f_destroy: TypedFunc<i32, i32>,
    f_vfs_add: TypedFunc<(i32, i32, u32, i32, u32), i32>,
    f_load: TypedFunc<(i32, i32, u32), i32>,
    f_init: TypedFunc<(i32, i32), i32>,
    f_step: TypedFunc<(i32, i32, i32, u32), i32>,
    f_prop_id: TypedFunc<(i32, i32, u32), i32>,
    f_prop_get: TypedFunc<(i32, i32, i32), i32>,
    f_last_error: TypedFunc<(i32, i32, u32), i32>,
    f_alloc: TypedFunc<u32, i32>,
    scratch: i32, // 64 KB guest scratch buffer
}

impl Jsb {
    fn new(engine: &Engine, module: &Module) -> Result<Self> {
        let mut linker: Linker<HostState> = Linker::new(engine);
        wasi_stubs::add_to_linker(&mut linker)?;
        linker.func_wrap("env", "kiro_ground_query", ground_query)?;
        let mut store = Store::new(
            engine,
            HostState { ground_elev_m: 0.0, ground_miss: false,
                        ground_calls: 0 },
        );
        let instance: Instance = linker.instantiate(&mut store, module)?;
        let memory = instance
            .get_memory(&mut store, "memory")
            .context("no guest memory export")?;
        if let Some(init) =
            instance.get_typed_func::<(), ()>(&mut store, "_initialize").ok()
        {
            init.call(&mut store, ())?;
        }
        let f_alloc =
            instance.get_typed_func::<u32, i32>(&mut store, "jsb_alloc")?;
        let scratch = f_alloc.call(&mut store, 65536)?;
        Ok(Self {
            memory,
            f_create: instance
                .get_typed_func::<i32, i32>(&mut store, "jsb_create")?,
            f_destroy: instance
                .get_typed_func::<i32, i32>(&mut store, "jsb_destroy")?,
            f_vfs_add: instance.get_typed_func::<(i32, i32, u32, i32, u32),
                i32>(&mut store, "jsb_vfs_add")?,
            f_load: instance.get_typed_func::<(i32, i32, u32), i32>(
                &mut store, "jsb_load_model")?,
            f_init: instance.get_typed_func::<(i32, i32), i32>(
                &mut store, "jsb_init")?,
            f_step: instance.get_typed_func::<(i32, i32, i32, u32), i32>(
                &mut store, "jsb_step_io")?,
            f_prop_id: instance.get_typed_func::<(i32, i32, u32), i32>(
                &mut store, "jsb_prop_id")?,
            f_prop_get: instance.get_typed_func::<(i32, i32, i32), i32>(
                &mut store, "jsb_prop_get")?,
            f_last_error: instance.get_typed_func::<(i32, i32, u32), i32>(
                &mut store, "jsb_last_error")?,
            f_alloc,
            store,
            scratch,
        })
    }

    fn write(&mut self, ptr: i32, bytes: &[u8]) {
        self.memory.write(&mut self.store, ptr as usize, bytes).unwrap();
    }
    fn read(&mut self, ptr: i32, len: usize) -> Vec<u8> {
        let mut v = vec![0u8; len];
        self.memory.read(&self.store, ptr as usize, &mut v).unwrap();
        v
    }
    fn alloc_bytes(&mut self, bytes: &[u8]) -> i32 {
        let p = self.f_alloc.call(&mut self.store, bytes.len() as u32).unwrap();
        self.write(p, bytes);
        p
    }
    fn create(&mut self, dt: f64) -> i32 {
        let cfg = JsbCreateV1 {
            struct_size: std::mem::size_of::<JsbCreateV1>() as u32,
            abi_version: JSB_ABI_VERSION, dt_s: dt, debug_level: 0, _pad: 0,
        };
        let b = as_bytes(&cfg).to_vec();
        let p = self.scratch;
        self.write(p, &b);
        self.f_create.call(&mut self.store, p).unwrap()
    }
    fn vfs_add(&mut self, h: i32, path: &str, data: &[u8]) -> i32 {
        let pp = self.alloc_bytes(path.as_bytes());
        let dp = self.alloc_bytes(data);
        self.f_vfs_add
            .call(&mut self.store,
                  (h, pp, path.len() as u32, dp, data.len() as u32))
            .unwrap()
    }
    fn load(&mut self, h: i32, name: &str) -> i32 {
        let np = self.alloc_bytes(name.as_bytes());
        self.f_load.call(&mut self.store, (h, np, name.len() as u32)).unwrap()
    }
    fn init(&mut self, h: i32, ic: &JsbIcV1) -> i32 {
        let b = as_bytes(ic).to_vec();
        let p = self.scratch;
        self.write(p, &b);
        self.f_init.call(&mut self.store, (h, p)).unwrap()
    }
    fn step(&mut self, h: i32, inp: &JsbInV1, substeps: u32)
            -> (i32, JsbOutV1) {
        let in_p = self.scratch;
        let out_p = self.scratch + 512;
        let b = as_bytes(inp).to_vec();
        self.write(in_p, &b);
        let mut out = JsbOutV1::default();
        out.struct_size = std::mem::size_of::<JsbOutV1>() as u32;
        let ob = as_bytes(&out).to_vec();
        self.write(out_p, &ob);
        let rc =
            self.f_step.call(&mut self.store, (h, in_p, out_p, substeps))
                .unwrap();
        let out: JsbOutV1 =
            from_bytes(&self.read(out_p, std::mem::size_of::<JsbOutV1>()));
        (rc, out)
    }
    fn prop_id(&mut self, h: i32, name: &str) -> i32 {
        let np = self.alloc_bytes(name.as_bytes());
        self.f_prop_id
            .call(&mut self.store, (h, np, name.len() as u32)).unwrap()
    }
    fn prop_get(&mut self, h: i32, id: i32) -> (i32, f64) {
        let p = self.scratch + 2048;
        let rc = self.f_prop_get.call(&mut self.store, (h, id, p)).unwrap();
        let b = self.read(p, 8);
        (rc, f64::from_le_bytes(b.try_into().unwrap()))
    }
    fn last_error(&mut self, h: i32) -> String {
        let p = self.scratch + 4096;
        let n = self.f_last_error.call(&mut self.store, (h, p, 512)).unwrap();
        let b = self.read(p, (n as usize).min(511));
        String::from_utf8_lossy(&b).into_owned()
    }
}

fn ground_query(mut caller: Caller<'_, HostState>, _h: i32, in_ptr: i32,
                out_ptr: i32) -> i32 {
    let mem = caller.get_export("memory").and_then(|e| e.into_memory())
        .expect("memory");
    let mut in_b = vec![0u8; std::mem::size_of::<KiroGroundInV1>()];
    mem.read(&caller, in_ptr as usize, &mut in_b).unwrap();
    let inp: KiroGroundInV1 = from_bytes(&in_b);

    let st = caller.data_mut();
    st.ground_calls += 1;
    let miss = st.ground_miss;
    let elev = st.ground_elev_m;

    let mut out = KiroGroundOutV1::default();
    out.struct_size = std::mem::size_of::<KiroGroundOutV1>() as u32;
    if miss {
        out.status = 1;
    } else {
        out.status = 0;
        out.agl_m = inp.alt_msl_m - elev;
        out.contact_ecef_m = geodetic_to_ecef(inp.lat_deg, inp.lon_deg, elev);
        out.normal_ecef = geodetic_up(inp.lat_deg, inp.lon_deg);
    }
    let ob = as_bytes(&out).to_vec();
    mem.write(&mut caller, out_ptr as usize, &ob).unwrap();
    0
}

fn read_pack(path: &str) -> Result<Vec<(String, Vec<u8>)>> {
    let raw = std::fs::read(path).with_context(|| format!("read {path}"))?;
    if &raw[0..4] != b"JSBP" {
        bail!("bad pack magic");
    }
    let ver = u32::from_le_bytes(raw[4..8].try_into()?);
    if ver != 1 {
        bail!("bad pack version {ver}");
    }
    let count = u32::from_le_bytes(raw[8..12].try_into()?) as usize;
    let mut off = 12usize;
    let mut idx = Vec::new();
    for _ in 0..count {
        let plen = u16::from_le_bytes(raw[off..off + 2].try_into()?) as usize;
        off += 2;
        let path = String::from_utf8(raw[off..off + plen].to_vec())?;
        off += plen;
        let fo = u32::from_le_bytes(raw[off..off + 4].try_into()?) as usize;
        let fl = u32::from_le_bytes(raw[off + 4..off + 8].try_into()?) as usize;
        off += 8;
        idx.push((path, fo, fl));
    }
    let blob0 = off;
    Ok(idx.into_iter()
        .map(|(p, o, l)| (p, raw[blob0 + o..blob0 + o + l].to_vec()))
        .collect())
}

fn scenario_ic() -> JsbIcV1 {
    JsbIcV1 {
        struct_size: std::mem::size_of::<JsbIcV1>() as u32, _pad: 0,
        lat_deg: 37.0, lon_deg: -122.0, alt_msl_m: 1000.0,
        heading_true_deg: 90.0, airspeed_tas_mps: 55.0, gamma_deg: 0.0,
        gear_down: 1, engines_running: 1,
    }
}

fn scenario_in(t: f64) -> JsbInV1 {
    let mut i = JsbInV1 {
        struct_size: std::mem::size_of::<JsbInV1>() as u32, throttle: 0.65,
        gear_down: 1, ..Default::default()
    };
    if (30.0..31.0).contains(&t) {
        i.elevator = -0.1;
    } else if (31.0..32.0).contains(&t) {
        i.elevator = 0.1;
    }
    i
}

fn load_full(j: &mut Jsb, pack: &[(String, Vec<u8>)]) -> Result<i32> {
    let h = j.create(1.0 / 120.0);
    if h <= 0 {
        bail!("create failed: {}", j.last_error(0));
    }
    for (p, d) in pack {
        let rc = j.vfs_add(h, p, d);
        if rc != JSB_OK {
            bail!("vfs_add {p}: {rc}");
        }
    }
    let rc = j.load(h, "c172p");
    if rc != JSB_OK {
        bail!("load_model: {rc} {}", j.last_error(h));
    }
    Ok(h)
}

struct T(&'static str, std::result::Result<(), String>);

fn contract_tests(engine: &Engine, module: &Module,
                  pack: &[(String, Vec<u8>)]) -> Vec<T> {
    let mut r: Vec<T> = Vec::new();
    macro_rules! check {
        ($name:expr, $body:expr) => {
            r.push(T($name, (|| -> std::result::Result<(), String> { $body })()));
        };
    }
    let e = |v: i32, want: i32, what: &str| {
        if v == want { Ok(()) }
        else { Err(format!("{what}: got {v}, want {want}")) }
    };

    let mut j = Jsb::new(engine, module).expect("instantiate");

    check!("bad create struct_size", {
        let mut cfg = JsbCreateV1 {
            struct_size: 4, abi_version: JSB_ABI_VERSION, dt_s: 1.0 / 120.0,
            debug_level: 0, _pad: 0,
        };
        let b = as_bytes(&cfg).to_vec();
        let p = j.scratch;
        j.write(p, &b);
        let rc = j.f_create.call(&mut j.store, p).unwrap();
        cfg.struct_size = std::mem::size_of::<JsbCreateV1>() as u32;
        cfg.abi_version = 99;
        let b = as_bytes(&cfg).to_vec();
        j.write(p, &b);
        let rc2 = j.f_create.call(&mut j.store, p).unwrap();
        e(rc, JSB_ERR_STRUCT_SIZE, "size")?;
        e(rc2, JSB_ERR_STRUCT_SIZE, "abi")
    });
    check!("bad handle rejected", {
        let rc = j.vfs_add(0x7fff_0001u32 as i32, "x", b"y");
        e(rc, JSB_ERR_BAD_HANDLE, "vfs_add")
    });
    check!("destroy is idempotent", {
        let rc = j.f_destroy.call(&mut j.store, 0x7fff_0001u32 as i32).unwrap();
        e(rc, JSB_OK, "stale destroy")
    });
    check!("load without data fails with message", {
        let h = j.create(1.0 / 120.0);
        if h <= 0 {
            return Err(format!("create: {h}"));
        }
        let rc = j.load(h, "c172p");
        let msg = j.last_error(h);
        let _ = j.f_destroy.call(&mut j.store, h).unwrap();
        e(rc, JSB_ERR_LOAD_FAILED, "load")?;
        if msg.is_empty() { Err("empty last_error".into()) } else { Ok(()) }
    });
    check!("full load from pack", {
        let h = load_full(&mut j, pack).map_err(|e| e.to_string())?;
        let _ = j.f_destroy.call(&mut j.store, h);
        Ok(())
    });
    check!("NaN IC rejected", {
        let h = load_full(&mut j, pack).map_err(|e| e.to_string())?;
        let mut ic = scenario_ic();
        ic.alt_msl_m = f64::NAN;
        let rc = j.init(h, &ic);
        let _ = j.f_destroy.call(&mut j.store, h);
        e(rc, JSB_ERR_NOT_FINITE, "init NaN")
    });
    check!("step before init rejected", {
        let h = load_full(&mut j, pack).map_err(|e| e.to_string())?;
        let (rc, _) = j.step(h, &scenario_in(0.0), 1);
        let _ = j.f_destroy.call(&mut j.store, h);
        e(rc, JSB_ERR_NOT_LOADED, "step")
    });
    check!("init + NaN control + wrong out size", {
        let h = load_full(&mut j, pack).map_err(|e| e.to_string())?;
        e(j.init(h, &scenario_ic()), JSB_OK, "init")?;
        let mut bad = scenario_in(0.0);
        bad.elevator = f64::NAN;
        let (rc, _) = j.step(h, &bad, 1);
        e(rc, JSB_ERR_NOT_FINITE, "NaN control")?;
        // wrong out struct size
        let in_p = j.scratch;
        let out_p = j.scratch + 512;
        let inp = scenario_in(0.0);
        let b = as_bytes(&inp).to_vec();
        j.write(in_p, &b);
        j.write(out_p, &8u32.to_le_bytes());
        let rc = j.f_step.call(&mut j.store, (h, in_p, out_p, 1)).unwrap();
        let _ = j.f_destroy.call(&mut j.store, h);
        e(rc, JSB_ERR_STRUCT_SIZE, "out size")
    });
    check!("props: bad name / good name / stale handle", {
        let h = load_full(&mut j, pack).map_err(|e| e.to_string())?;
        e(j.init(h, &scenario_ic()), JSB_OK, "init")?;
        let bad = j.prop_id(h, "no/such/prop");
        e(bad, JSB_ERR_PROP, "bad prop")?;
        let id = j.prop_id(h, "velocities/vc-kts");
        if id <= 0 { return Err(format!("prop_id: {id}")); }
        let (rc, v) = j.prop_get(h, id);
        e(rc, JSB_OK, "prop_get")?;
        if !(60.0..140.0).contains(&v) {
            return Err(format!("vc-kts {v} out of expected range"));
        }
        let _ = j.f_destroy.call(&mut j.store, h);
        let (rc, _) = j.prop_get(h, id);
        e(rc, JSB_ERR_BAD_HANDLE, "stale prop_get")
    });
    check!("<output> element stripped at ingestion", {
        let h = j.create(1.0 / 120.0);
        for (p, d) in pack {
            if p.ends_with("c172p.xml") {
                let s = String::from_utf8_lossy(d);
                let injected = s.replace(
                    "<metrics>",
                    "<output name=\"x\" type=\"SOCKET\" port=\"99\" rate=\"10\">\
                     <property>velocities/vc-kts</property></output><metrics>");
                j.vfs_add(h, p, injected.as_bytes());
            } else {
                j.vfs_add(h, p, d);
            }
        }
        let rc = j.load(h, "c172p");
        let _ = j.f_destroy.call(&mut j.store, h);
        e(rc, JSB_OK, "load with injected <output>")
    });
    check!("two instances step independently", {
        let h1 = load_full(&mut j, pack).map_err(|e| e.to_string())?;
        let h2 = load_full(&mut j, pack).map_err(|e| e.to_string())?;
        e(j.init(h1, &scenario_ic()), JSB_OK, "init1")?;
        e(j.init(h2, &scenario_ic()), JSB_OK, "init2")?;
        let (rc1, o1) = j.step(h1, &scenario_in(0.0), 2);
        let (rc2, o2) = j.step(h2, &scenario_in(0.0), 4);
        let _ = j.f_destroy.call(&mut j.store, h1);
        let _ = j.f_destroy.call(&mut j.store, h2);
        e(rc1, JSB_OK, "step1")?;
        e(rc2, JSB_OK, "step2")?;
        if (o1.sim_time_s - 2.0 / 120.0).abs() > 1e-9
            || (o2.sim_time_s - 4.0 / 120.0).abs() > 1e-9 {
            return Err(format!("sim times {} {}", o1.sim_time_s, o2.sim_time_s));
        }
        Ok(())
    });
    check!("ground miss -> ellipsoid fallback", {
        j.store.data_mut().ground_miss = true;
        let h = load_full(&mut j, pack).map_err(|e| e.to_string())?;
        e(j.init(h, &scenario_ic()), JSB_OK, "init")?;
        let (rc, o) = j.step(h, &scenario_in(0.0), 1);
        let _ = j.f_destroy.call(&mut j.store, h);
        j.store.data_mut().ground_miss = false;
        e(rc, JSB_OK, "step")?;
        if (o.alt_agl_m - 1000.0).abs() > 5.0 {
            return Err(format!("agl {} not ~1000", o.alt_agl_m));
        }
        Ok(())
    });
    check!("ground hit at 100 m -> AGL ~900 + queries counted", {
        j.store.data_mut().ground_elev_m = 100.0;
        let before = j.store.data().ground_calls;
        let h = load_full(&mut j, pack).map_err(|e| e.to_string())?;
        e(j.init(h, &scenario_ic()), JSB_OK, "init")?;
        let (rc, o) = j.step(h, &scenario_in(0.0), 1);
        let after = j.store.data().ground_calls;
        let _ = j.f_destroy.call(&mut j.store, h);
        j.store.data_mut().ground_elev_m = 0.0;
        e(rc, JSB_OK, "step")?;
        if (o.alt_agl_m - 900.0).abs() > 5.0 {
            return Err(format!("agl {} not ~900", o.alt_agl_m));
        }
        if after <= before {
            return Err("no host ground queries".into());
        }
        Ok(())
    });
    r
}

const PROPS: [&str; 11] = [
    "position/lat-geod-deg", "position/long-gc-deg", "position/h-sl-ft",
    "attitude/phi-rad", "attitude/theta-rad", "attitude/psi-rad",
    "velocities/v-north-fps", "velocities/v-east-fps",
    "velocities/v-down-fps", "aero/alpha-rad", "velocities/vc-kts",
];
// Two-regime tolerances (raw JSBSim units), aligned with PROPS.
//
// Pre-doublet (t <= 30 s, straight & level): the wasm module reproduces the
// native MSVC build BIT-IDENTICALLY (measured 2026-07-17: all deltas exactly
// 0.0 for 3600 steps), so this regime asserts near-zero.
//
// Post-doublet: the elevator transient sweeps aero/libm inputs into ranges
// where MSVC CRT and wasi-libm differ by ULPs, and the c172p's marginally
// stable spiral amplifies them into wandering phase differences. Cross-libm
// bit-equality is impossible there; the envelope bounds catch gross FDM
// breakage instead. (Cross-RUNTIME determinism of the wasm module itself —
// wasmtime vs V8 — IS bit-exact; proven in Phase 0.)
const TOL_EXACT: f64 = 1.0e-9; // t <= 30 s
const TOL_ENVELOPE: [f64; 11] = [
    5.0e-4,  // lat deg (~55 m)
    5.0e-4,  // lon deg
    30.0,    // alt ft
    0.15,    // phi rad
    0.15,    // theta rad
    0.25,    // psi rad
    15.0,    // vn fps
    15.0,    // ve fps
    15.0,    // vd fps
    0.1,     // alpha rad
    3.0,     // vcas kts
];

fn trajectory(engine: &Engine, module: &Module,
              pack: &[(String, Vec<u8>)], out_csv: &str) -> Result<()> {
    let mut j = Jsb::new(engine, module)?;
    let h = load_full(&mut j, pack)?;
    if j.init(h, &scenario_ic()) != JSB_OK {
        bail!("init: {}", j.last_error(h));
    }
    let ids: Vec<i32> = PROPS.iter().map(|p| j.prop_id(h, p)).collect();
    if ids.iter().any(|&i| i <= 0) {
        bail!("prop_id failed for a trajectory column");
    }
    let mut csv = String::from(
        "time_s,lat_geod_deg,lon_deg,alt_asl_ft,phi_rad,theta_rad,psi_rad,\
         vn_fps,ve_fps,vd_fps,alpha_rad,vcas_kts\n");
    let mut t_wall = std::time::Duration::ZERO;
    let mut sim_t = 0.0f64;
    for i in 0..=12000u32 {
        if i % 120 == 0 {
            let vals: Vec<f64> =
                ids.iter().map(|&id| j.prop_get(h, id).1).collect();
            csv.push_str(&format!(
                "{:.6},{:.12},{:.12},{:.8},{:.10},{:.10},{:.10},{:.8},{:.8},\
                 {:.8},{:.10},{:.8}\n",
                sim_t, vals[0], vals[1], vals[2], vals[3], vals[4], vals[5],
                vals[6], vals[7], vals[8], vals[9], vals[10]));
        }
        if i == 12000 {
            break;
        }
        let inp = scenario_in(sim_t);
        let t0 = std::time::Instant::now();
        let (rc, out) = j.step(h, &inp, 1);
        t_wall += t0.elapsed();
        if rc != JSB_OK {
            bail!("step {i}: {rc} {}", j.last_error(h));
        }
        sim_t = out.sim_time_s;
    }
    std::fs::write(out_csv, &csv)?;
    println!("trajectory: 12000 steps, {:.1} us/step, wrote {out_csv}",
             t_wall.as_secs_f64() * 1e6 / 12000.0);
    Ok(())
}

fn parse_csv(path: &str) -> Result<Vec<Vec<f64>>> {
    // Data rows only: 12 comma-separated floats (banner/header lines skipped).
    let text = std::fs::read_to_string(path)?;
    Ok(text.lines()
        .filter_map(|l| {
            let vals: Vec<f64> = l.split(',')
                .filter_map(|v| v.trim().parse().ok())
                .collect();
            (vals.len() == 12).then_some(vals)
        })
        .collect())
}

fn compare(ref_csv: &str, wasm_csv: &str) -> Result<bool> {
    let a = parse_csv(ref_csv)?;
    let b = parse_csv(wasm_csv)?;
    if a.len() != b.len() {
        bail!("row count differs: ref {} vs wasm {}", a.len(), b.len());
    }
    let names = ["lat", "lon", "alt_ft", "phi", "theta", "psi", "vn", "ve",
                 "vd", "alpha", "vcas"];
    let mut ok = true;
    println!("{:<8} {:>13} {:>13} {:>12} {:>8}", "column", "pre30_max",
             "post30_max", "envelope", "verdict");
    for c in 0..11 {
        let (mut pre, mut post) = (0.0f64, 0.0f64);
        for (ra, rb) in a.iter().zip(&b) {
            let d = (ra[c + 1] - rb[c + 1]).abs();
            if ra[0] <= 30.0 {
                pre = pre.max(d);
            } else {
                post = post.max(d);
            }
        }
        let pass = pre <= TOL_EXACT && post <= TOL_ENVELOPE[c];
        ok &= pass;
        println!("{:<8} {:>13.3e} {:>13.3e} {:>12.1e} {:>8}", names[c], pre,
                 post, TOL_ENVELOPE[c], if pass { "PASS" } else { "FAIL" });
    }
    println!("(pre-30s bound: {TOL_EXACT:.0e} — bit-identical regime)");
    Ok(ok)
}

fn arg(name: &str, default: &str) -> String {
    let args: Vec<String> = std::env::args().collect();
    args.iter().position(|a| a == name)
        .and_then(|i| args.get(i + 1).cloned())
        .unwrap_or_else(|| default.to_string())
}

fn main() -> Result<()> {
    let module_path = arg("--module", "dist/jsbsim.wasm");
    let pack_path = arg("--pack", "dist/c172.jsbpack");
    let ref_csv = arg("--ref", "dist/ref_trajectory.csv");
    let out_csv = arg("--out", "dist/wasm_trajectory.csv");

    let mut cfg = Config::new();
    cfg.wasm_exceptions(true);
    let engine = Engine::new(&cfg)?;
    let module = Module::from_file(&engine, &module_path)?;

    let pack = read_pack(&pack_path)?;
    println!("pack: {} files", pack.len());

    let results = contract_tests(&engine, &module, &pack);
    let mut failed = 0;
    for T(name, res) in &results {
        match res {
            Ok(()) => println!("PASS  {name}"),
            Err(e) => {
                failed += 1;
                println!("FAIL  {name}: {e}");
            }
        }
    }
    println!("contract tests: {}/{} passed", results.len() - failed,
             results.len());

    trajectory(&engine, &module, &pack, &out_csv)?;

    if std::path::Path::new(&ref_csv).exists() {
        let ok = compare(&ref_csv, &out_csv)?;
        println!("trajectory comparison: {}",
                 if ok { "PASS" } else { "FAIL" });
        if !ok || failed > 0 {
            std::process::exit(1);
        }
    } else {
        println!("(no {ref_csv} — skipping comparison)");
        if failed > 0 {
            std::process::exit(1);
        }
    }
    Ok(())
}
