//! Minimal WASI preview1 stubs for the jsbsim.wasm reactor — the exact set
//! from dist/imports.txt. The VFS supplies every file the FDM needs, so all
//! filesystem imports return errors; fd_write routes stdout/stderr to the
//! host log. This module is the working model for Kiro's production
//! flight/jsbsim/wasi_stubs.rs.
use wasmtime::{Caller, Linker};

const ERRNO_SUCCESS: i32 = 0;
const ERRNO_BADF: i32 = 8;
const ERRNO_NOENT: i32 = 44;
const ERRNO_NOSYS: i32 = 52;

pub struct HostState {
    pub ground_elev_m: f64,
    pub ground_miss: bool,
    pub ground_calls: u64,
}

pub fn add_to_linker(linker: &mut Linker<HostState>) -> anyhow::Result<()> {
    let m = "wasi_snapshot_preview1";

    linker.func_wrap(m, "fd_write",
        |mut caller: Caller<'_, HostState>, fd: i32, iovs: i32, iovs_len: i32,
         nwritten: i32| -> i32 {
            let mem = caller.get_export("memory").and_then(|e| e.into_memory())
                .expect("memory");
            let data = mem.data(&caller).to_vec();
            let mut total = 0usize;
            let mut out = Vec::new();
            for i in 0..iovs_len as usize {
                let base = iovs as usize + i * 8;
                let ptr = u32::from_le_bytes(
                    data[base..base + 4].try_into().unwrap()) as usize;
                let len = u32::from_le_bytes(
                    data[base + 4..base + 8].try_into().unwrap()) as usize;
                out.extend_from_slice(&data[ptr..ptr + len]);
                total += len;
            }
            if fd == 1 || fd == 2 {
                print!("{}", String::from_utf8_lossy(&out));
            }
            let _ = mem.write(&mut caller, nwritten as usize,
                              &(total as u32).to_le_bytes());
            ERRNO_SUCCESS
        })?;

    linker.func_wrap(m, "clock_time_get",
        |mut caller: Caller<'_, HostState>, _id: i32, _prec: i64,
         out_ptr: i32| -> i32 {
            let now = std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap_or_default()
                .as_nanos() as u64;
            let mem = caller.get_export("memory").and_then(|e| e.into_memory())
                .expect("memory");
            let _ = mem.write(&mut caller, out_ptr as usize,
                              &now.to_le_bytes());
            ERRNO_SUCCESS
        })?;

    linker.func_wrap(m, "environ_sizes_get",
        |mut caller: Caller<'_, HostState>, count_ptr: i32, size_ptr: i32|
         -> i32 {
            let mem = caller.get_export("memory").and_then(|e| e.into_memory())
                .expect("memory");
            let _ = mem.write(&mut caller, count_ptr as usize, &0u32.to_le_bytes());
            let _ = mem.write(&mut caller, size_ptr as usize, &0u32.to_le_bytes());
            ERRNO_SUCCESS
        })?;
    linker.func_wrap(m, "environ_get",
        |_: Caller<'_, HostState>, _: i32, _: i32| -> i32 { ERRNO_SUCCESS })?;

    linker.func_wrap(m, "proc_exit",
        |_: Caller<'_, HostState>, code: i32| -> Result<(), wasmtime::Error> {
            Err(wasmtime::Error::msg(format!("guest proc_exit({code})")))
        })?;

    // Filesystem: nothing is preopened; the MemVFS answers everything the
    // FDM needs, so these legitimately fail.
    linker.func_wrap(m, "fd_close",
        |_: Caller<'_, HostState>, _: i32| -> i32 { ERRNO_BADF })?;
    linker.func_wrap(m, "fd_fdstat_get",
        |_: Caller<'_, HostState>, _: i32, _: i32| -> i32 { ERRNO_BADF })?;
    linker.func_wrap(m, "fd_fdstat_set_flags",
        |_: Caller<'_, HostState>, _: i32, _: i32| -> i32 { ERRNO_BADF })?;
    linker.func_wrap(m, "fd_prestat_get",
        |_: Caller<'_, HostState>, _: i32, _: i32| -> i32 { ERRNO_BADF })?;
    linker.func_wrap(m, "fd_prestat_dir_name",
        |_: Caller<'_, HostState>, _: i32, _: i32, _: i32| -> i32 {
            ERRNO_BADF
        })?;
    linker.func_wrap(m, "fd_read",
        |_: Caller<'_, HostState>, _: i32, _: i32, _: i32, _: i32| -> i32 {
            ERRNO_BADF
        })?;
    linker.func_wrap(m, "fd_seek",
        |_: Caller<'_, HostState>, _: i32, _: i64, _: i32, _: i32| -> i32 {
            ERRNO_BADF
        })?;
    linker.func_wrap(m, "path_filestat_get",
        |_: Caller<'_, HostState>, _: i32, _: i32, _: i32, _: i32, _: i32|
         -> i32 { ERRNO_NOENT })?;
    linker.func_wrap(m, "path_open",
        |_: Caller<'_, HostState>, _: i32, _: i32, _: i32, _: i32, _: i32,
         _: i64, _: i64, _: i32, _: i32| -> i32 { ERRNO_NOENT })?;

    // Not in the current import table but harmless to provide.
    let _ = linker.func_wrap(m, "random_get",
        |_: Caller<'_, HostState>, _: i32, _: i32| -> i32 { ERRNO_NOSYS });

    Ok(())
}
