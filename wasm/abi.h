/* kiro-wasm facade ABI — single source of truth.
 *
 * Mirrored by Rust #[repr(C)] structs in the Kiro engine (flight/jsbsim/
 * backend.rs) with size assertions against jsb_abi_version().
 *
 * Conventions:
 *  - All struct pointers are guest-linear-memory offsets, validated by both
 *    sides. Every struct begins with u32 struct_size (compat check).
 *  - All angles in degrees at the ABI unless suffixed _rad; distances/speeds
 *    SI (m, m/s); JSBSim-internal imperial conversion happens in the facade.
 *  - Latitude is WGS84 geodetic; longitude ±180, east-positive.
 *  - Exceptions NEVER cross the ABI: every export catches and returns a
 *    negative status; message retrievable via jsb_last_error.
 *  - Handles are generation-tagged (gen<<16 | slot); stale handles rejected.
 */
#ifndef JSBSIM_KIRO_WASM_ABI_H
#define JSBSIM_KIRO_WASM_ABI_H

#include <stdint.h>

#define JSB_ABI_VERSION 1u

/* Status codes (negative = error). */
#define JSB_OK               0
#define JSB_ERR_BAD_HANDLE  -1
#define JSB_ERR_BAD_ARG     -2
#define JSB_ERR_EXCEPTION   -3   /* C++ exception caught at the boundary */
#define JSB_ERR_NOT_LOADED  -4   /* operation requires a loaded model */
#define JSB_ERR_VFS_LIMIT   -5   /* file count/size/path-length cap hit */
#define JSB_ERR_STRUCT_SIZE -6   /* struct_size mismatch (ABI drift) */
#define JSB_ERR_LOAD_FAILED -7   /* model load returned failure */
#define JSB_ERR_INIT_FAILED -8   /* RunIC failed */
#define JSB_ERR_PROP        -9   /* property not found / invalid id */
#define JSB_ERR_NOT_FINITE -10   /* NaN/Inf rejected */
#define JSB_ERR_SLOTS      -11   /* instance table full */

/* VFS limits (enforced in memvfs.cpp; violations -> JSB_ERR_VFS_LIMIT). */
#define JSB_VFS_MAX_FILES      512u
#define JSB_VFS_MAX_FILE_BYTES (8u * 1024u * 1024u)
#define JSB_VFS_MAX_TOTAL      (64u * 1024u * 1024u)
#define JSB_VFS_MAX_PATH       256u

#define JSB_MAX_ENGINES 4u

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JsbCreateV1 {
  uint32_t struct_size;    /* = sizeof(JsbCreateV1) */
  uint32_t abi_version;    /* must equal JSB_ABI_VERSION */
  double   dt_s;           /* fixed FDM step, e.g. 1.0/120 */
  uint32_t debug_level;    /* 0 quiet .. 2 chatty (JSBSim debug_lvl) */
  uint32_t _pad;
} JsbCreateV1;

typedef struct JsbIcV1 {
  uint32_t struct_size;
  uint32_t _pad;
  double lat_deg;          /* WGS84 geodetic */
  double lon_deg;          /* ±180, east+ */
  double alt_msl_m;
  double heading_true_deg;
  double airspeed_tas_mps; /* true airspeed */
  double gamma_deg;        /* flight-path angle */
  uint32_t gear_down;      /* bool */
  uint32_t engines_running;/* bool: start engines at IC */
} JsbIcV1;

typedef struct JsbInV1 {
  uint32_t struct_size;
  uint32_t _pad;
  double aileron;          /* [-1,1] */
  double elevator;         /* [-1,1] */
  double rudder;           /* [-1,1] */
  double throttle;         /* [0,1] applied to all engines */
  double flaps;            /* [0,1] */
  double brake_left;       /* [0,1] */
  double brake_right;      /* [0,1] */
  double pitch_trim;       /* [-1,1] */
  uint32_t gear_down;      /* bool */
  uint32_t _pad2;
} JsbInV1;

typedef struct JsbOutV1 {
  uint32_t struct_size;
  uint32_t status_flags;   /* bit0 on_ground (any WOW) */
  double sim_time_s;
  /* Position */
  double lat_deg, lon_deg, alt_msl_m, alt_agl_m;
  /* Attitude: euler NED->body (rad) + quaternion body->NED (w,x,y,z) */
  double roll_rad, pitch_rad, yaw_rad;
  double q_w, q_x, q_y, q_z;
  /* Velocities */
  double vn_mps, ve_mps, vd_mps;      /* NED */
  double p_rps, q_rps, r_rps;         /* body rates */
  double ax_mps2, ay_mps2, az_mps2;   /* body-frame accelerations (UVWdot) */
  double alpha_rad, beta_rad;
  double vtas_mps, vcas_mps, vground_mps;
  /* Propulsion (first JSB_MAX_ENGINES engines) */
  uint32_t engine_count; uint32_t _pad3;
  double thrust_n[JSB_MAX_ENGINES];
  double engine_rpm[JSB_MAX_ENGINES];
  double fuel_kg;                     /* total */
  /* Surfaces (normalized positions) */
  double surf_aileron, surf_elevator, surf_rudder, surf_flaps;
  double gear_pos;                    /* 0 up .. 1 down */
  /* Diagnostics */
  uint32_t ground_queries;            /* host ground calls this step batch */
  uint32_t trapped;                   /* reserved, 0 */
} JsbOutV1;

/* Host ground-query import contract (env.pree_ground_query).
 * Input position is geodetic; outputs are ECEF meters (WGS84).
 * status: 0 hit, 1 miss (facade falls back to the reference ellipsoid). */
typedef struct KiroGroundInV1 {
  uint32_t struct_size; uint32_t _pad;
  double lat_deg, lon_deg, alt_msl_m;
} KiroGroundInV1;

typedef struct KiroGroundOutV1 {
  uint32_t struct_size;
  uint32_t status;                    /* 0 hit, 1 miss */
  double agl_m;
  double contact_ecef_m[3];
  double normal_ecef[3];              /* unit */
  double vel_ecef_mps[3];             /* terrain velocity (0 = static) */
  double angvel_ecef_rps[3];
} KiroGroundOutV1;

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* JSBSIM_KIRO_WASM_ABI_H */
