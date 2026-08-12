// pree-wasm facade: the flat C ABI over FGFDMExec (contract in abi.h).
// Exceptions never escape an export; instances are slot+generation handles.
#include "abi.h"
#include "memvfs.h"
#include "ground_host.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include <FGFDMExec.h>
#include <initialization/FGInitialCondition.h>
#include <models/FGAccelerations.h>
#include <models/FGAuxiliary.h>
#include <models/FGFCS.h>
#include <models/FGGroundReactions.h>
#include <models/FGInertial.h>
#include <models/FGPropagate.h>
#include <models/FGPropulsion.h>
#include <models/propulsion/FGEngine.h>
#include <models/propulsion/FGTank.h>
#include <models/propulsion/FGThruster.h>

#define JSB_EXPORT(name) \
  __attribute__((export_name(#name), used)) extern "C"

namespace {

constexpr double kFtToM = 0.3048;
constexpr double kMToFt = 1.0 / 0.3048;
constexpr double kMpsToKts = 1.0 / 0.514444;
constexpr double kKtsToMps = 0.514444;
constexpr double kLbfToN = 4.4482216152605;
constexpr double kLbsToKg = 0.45359237;

struct Instance {
  std::unique_ptr<JSBSim::FGFDMExec> fdm;
  pree::MemVfs vfs;
  pree::HostGroundCallback* ground = nullptr;  // owned by FGInertial
  std::vector<SGPropertyNode_ptr> props;       // prop-id cache (1-based ids)
  std::string last_error;
  bool loaded = false;
  bool initialized = false;
};

constexpr uint32_t kMaxSlots = 8;
struct Slot {
  uint16_t gen = 1;
  std::unique_ptr<Instance> inst;
};
Slot g_slots[kMaxSlots];
std::string g_global_error;

int32_t make_handle(uint32_t idx) {
  return (int32_t)(((uint32_t)g_slots[idx].gen << 16) | (idx + 1));
}

Instance* resolve(int32_t h) {
  uint32_t idx = ((uint32_t)h & 0xffffu);
  uint16_t gen = (uint16_t)(((uint32_t)h >> 16) & 0xffffu);
  if (idx == 0 || idx > kMaxSlots) return nullptr;
  Slot& s = g_slots[idx - 1];
  if (!s.inst || s.gen != gen) return nullptr;
  return s.inst.get();
}

void set_error(Instance* inst, const std::string& msg) {
  if (inst)
    inst->last_error = msg;
  else
    g_global_error = msg;
}

// Scope guard: JSBSim's VFS hooks consult the "active" VFS.
struct VfsScope {
  explicit VfsScope(Instance* i) { pree::set_active_vfs(&i->vfs); }
  ~VfsScope() { pree::set_active_vfs(nullptr); }
};

bool finite_or_zero(double v) { return std::isfinite(v); }

double clampd(double v, double lo, double hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

// Euler (NED->body, JSBSim phi/theta/psi) -> quaternion body->NED (w,x,y,z).
void euler_to_body_to_ned_quat(double phi, double theta, double psi,
                               double* w, double* x, double* y, double* z) {
  double cph = std::cos(phi * 0.5), sph = std::sin(phi * 0.5);
  double cth = std::cos(theta * 0.5), sth = std::sin(theta * 0.5);
  double cps = std::cos(psi * 0.5), sps = std::sin(psi * 0.5);
  // NED->body quaternion (ZYX order), then conjugate for body->NED.
  double qw = cps * cth * cph + sps * sth * sph;
  double qx = cps * cth * sph - sps * sth * cph;
  double qy = cps * sth * cph + sps * cth * sph;
  double qz = sps * cth * cph - cps * sth * sph;
  *w = qw;
  *x = -qx;
  *y = -qy;
  *z = -qz;
}

}  // namespace

JSB_EXPORT(jsb_abi_version) uint32_t jsb_abi_version() { return JSB_ABI_VERSION; }

JSB_EXPORT(jsb_build_info) int32_t jsb_build_info(char* buf, uint32_t cap) {
  static const std::string info =
      std::string("JSBSim ") + JSBSIM_VERSION + " pree-wasm abi=1 minimal=1";
  if (buf && cap > 0) {
    uint32_t n = (uint32_t)info.size() < cap - 1 ? (uint32_t)info.size() : cap - 1;
    std::memcpy(buf, info.data(), n);
    buf[n] = '\0';
  }
  return (int32_t)info.size();
}

JSB_EXPORT(jsb_alloc) void* jsb_alloc(uint32_t len) { return std::malloc(len); }
JSB_EXPORT(jsb_free) void jsb_free(void* p, uint32_t) { std::free(p); }

JSB_EXPORT(jsb_create) int32_t jsb_create(const JsbCreateV1* cfg) {
  if (!cfg) {
    g_global_error = "null cfg";
    return JSB_ERR_BAD_ARG;
  }
  if (cfg->struct_size != sizeof(JsbCreateV1)) {
    g_global_error = "JsbCreateV1 size mismatch";
    return JSB_ERR_STRUCT_SIZE;
  }
  if (cfg->abi_version != JSB_ABI_VERSION) {
    g_global_error = "ABI version mismatch";
    return JSB_ERR_STRUCT_SIZE;
  }
  if (!finite_or_zero(cfg->dt_s) || cfg->dt_s <= 0.0 || cfg->dt_s > 1.0) {
    g_global_error = "dt out of range";
    return JSB_ERR_BAD_ARG;
  }
  uint32_t idx = 0;
  for (; idx < kMaxSlots; ++idx)
    if (!g_slots[idx].inst) break;
  if (idx == kMaxSlots) {
    g_global_error = "instance table full";
    return JSB_ERR_SLOTS;
  }
  try {
    auto inst = std::make_unique<Instance>();
    inst->fdm = std::make_unique<JSBSim::FGFDMExec>();
    inst->fdm->SetDebugLevel((int)cfg->debug_level);
    inst->fdm->Setdt(cfg->dt_s);
    g_slots[idx].inst = std::move(inst);
    int32_t h = make_handle(idx);
    // Ground bridge: replace the default callback, keep JSBSim's ellipse.
    Instance* i = g_slots[idx].inst.get();
    auto inertial = i->fdm->GetInertial();
    auto* gcb = new pree::HostGroundCallback(h, inertial->GetSemimajor(),
                                             inertial->GetSemiminor());
    inertial->SetGroundCallback(gcb);
    i->ground = gcb;
    return h;
  } catch (const std::exception& e) {
    g_global_error = std::string("create: ") + e.what();
    return JSB_ERR_EXCEPTION;
  } catch (...) {
    g_global_error = "create: unknown exception";
    return JSB_ERR_EXCEPTION;
  }
}

JSB_EXPORT(jsb_destroy) int32_t jsb_destroy(int32_t h) {
  uint32_t idx = ((uint32_t)h & 0xffffu);
  uint16_t gen = (uint16_t)(((uint32_t)h >> 16) & 0xffffu);
  if (idx == 0 || idx > kMaxSlots) return JSB_OK;  // idempotent
  Slot& s = g_slots[idx - 1];
  if (!s.inst || s.gen != gen) return JSB_OK;      // idempotent/stale
  try {
    VfsScope scope(s.inst.get());
    s.inst.reset();
  } catch (...) {
    s.inst.reset();
  }
  ++s.gen;
  if (s.gen == 0) s.gen = 1;
  return JSB_OK;
}

JSB_EXPORT(jsb_vfs_add)
int32_t jsb_vfs_add(int32_t h, const char* path, uint32_t plen,
                    const char* data, uint32_t dlen) {
  Instance* inst = resolve(h);
  if (!inst) return JSB_ERR_BAD_HANDLE;
  if (!path || plen == 0 || (!data && dlen > 0)) {
    set_error(inst, "vfs_add: bad args");
    return JSB_ERR_BAD_ARG;
  }
  int rc = inst->vfs.add(std::string(path, plen), data, dlen,
                         /*strip_output=*/true);
  if (rc != JSB_OK) set_error(inst, "vfs_add: limit exceeded");
  return rc;
}

JSB_EXPORT(jsb_load_model)
int32_t jsb_load_model(int32_t h, const char* name, uint32_t nlen) {
  Instance* inst = resolve(h);
  if (!inst) return JSB_ERR_BAD_HANDLE;
  if (!name || nlen == 0 || nlen > 128) {
    set_error(inst, "load_model: bad name");
    return JSB_ERR_BAD_ARG;
  }
  try {
    VfsScope scope(inst);
    inst->fdm->SetRootDir(SGPath(""));
    inst->fdm->SetAircraftPath(SGPath("aircraft"));
    inst->fdm->SetEnginePath(SGPath("engine"));
    inst->fdm->SetSystemsPath(SGPath("systems"));
    bool ok = inst->fdm->LoadModel(std::string(name, nlen));
    if (!ok) {
      set_error(inst, "LoadModel returned false (missing/invalid data?)");
      return JSB_ERR_LOAD_FAILED;
    }
    inst->loaded = true;
    return JSB_OK;
  } catch (const std::exception& e) {
    set_error(inst, std::string("load_model: ") + e.what());
    return JSB_ERR_EXCEPTION;
  } catch (...) {
    set_error(inst, "load_model: unknown exception");
    return JSB_ERR_EXCEPTION;
  }
}

JSB_EXPORT(jsb_init) int32_t jsb_init(int32_t h, const JsbIcV1* ic) {
  Instance* inst = resolve(h);
  if (!inst) return JSB_ERR_BAD_HANDLE;
  if (!inst->loaded) {
    set_error(inst, "init: no model loaded");
    return JSB_ERR_NOT_LOADED;
  }
  if (!ic || ic->struct_size != sizeof(JsbIcV1)) {
    set_error(inst, "init: JsbIcV1 size mismatch");
    return JSB_ERR_STRUCT_SIZE;
  }
  const double vals[] = {ic->lat_deg, ic->lon_deg, ic->alt_msl_m,
                         ic->heading_true_deg, ic->airspeed_tas_mps,
                         ic->gamma_deg};
  for (double v : vals)
    if (!finite_or_zero(v)) {
      set_error(inst, "init: non-finite IC");
      return JSB_ERR_NOT_FINITE;
    }
  try {
    VfsScope scope(inst);
    auto fic = inst->fdm->GetIC();
    fic->SetGeodLatitudeDegIC(ic->lat_deg);
    fic->SetLongitudeDegIC(ic->lon_deg);
    fic->SetAltitudeASLFtIC(ic->alt_msl_m * kMToFt);
    fic->SetPsiDegIC(ic->heading_true_deg);
    fic->SetVtrueKtsIC(ic->airspeed_tas_mps * kMpsToKts);
    fic->SetFlightPathAngleDegIC(ic->gamma_deg);
    inst->fdm->GetFCS()->SetGearCmd(ic->gear_down ? 1.0 : 0.0);
    if (!inst->fdm->RunIC()) {
      set_error(inst, "RunIC failed");
      return JSB_ERR_INIT_FAILED;
    }
    if (ic->engines_running) inst->fdm->GetPropulsion()->InitRunning(-1);
    inst->initialized = true;
    return JSB_OK;
  } catch (const std::exception& e) {
    set_error(inst, std::string("init: ") + e.what());
    return JSB_ERR_EXCEPTION;
  } catch (...) {
    set_error(inst, "init: unknown exception");
    return JSB_ERR_EXCEPTION;
  }
}

namespace {

void fill_out(Instance* inst, JsbOutV1* out) {
  auto fdm = inst->fdm.get();
  auto prop = fdm->GetPropagate();
  auto aux = fdm->GetAuxiliary();
  auto acc = fdm->GetAccelerations();
  auto fcs = fdm->GetFCS();
  auto pul = fdm->GetPropulsion();

  out->sim_time_s = fdm->GetSimTime();
  out->lat_deg = prop->GetGeodLatitudeDeg();
  out->lon_deg = prop->GetLongitudeDeg();
  out->alt_msl_m = prop->GetAltitudeASL() * kFtToM;
  out->alt_agl_m = prop->GetDistanceAGL() * kFtToM;

  const auto& euler = prop->GetEuler();
  out->roll_rad = euler(1);
  out->pitch_rad = euler(2);
  out->yaw_rad = euler(3);
  euler_to_body_to_ned_quat(euler(1), euler(2), euler(3), &out->q_w, &out->q_x,
                            &out->q_y, &out->q_z);

  const auto& vel = prop->GetVel();  // NED, fps
  out->vn_mps = vel(1) * kFtToM;
  out->ve_mps = vel(2) * kFtToM;
  out->vd_mps = vel(3) * kFtToM;
  const auto& pqr = prop->GetPQR();
  out->p_rps = pqr(1);
  out->q_rps = pqr(2);
  out->r_rps = pqr(3);
  const auto& uvwdot = acc->GetUVWdot();  // fps^2
  out->ax_mps2 = uvwdot(1) * kFtToM;
  out->ay_mps2 = uvwdot(2) * kFtToM;
  out->az_mps2 = uvwdot(3) * kFtToM;

  out->alpha_rad = aux->Getalpha();
  out->beta_rad = aux->Getbeta();
  out->vtas_mps = aux->GetVtrueFPS() * kFtToM;
  out->vcas_mps = aux->GetVcalibratedKTS() * kKtsToMps;
  out->vground_mps = aux->GetVground() * kFtToM;

  size_t n = pul->GetNumEngines();
  out->engine_count = (uint32_t)(n > JSB_MAX_ENGINES ? JSB_MAX_ENGINES : n);
  for (uint32_t i = 0; i < JSB_MAX_ENGINES; ++i) {
    out->thrust_n[i] = 0.0;
    out->engine_rpm[i] = 0.0;
  }
  for (uint32_t i = 0; i < out->engine_count; ++i) {
    auto eng = pul->GetEngine(i);
    auto thr = eng->GetThruster();
    if (thr) {
      out->thrust_n[i] = thr->GetThrust() * kLbfToN;
      out->engine_rpm[i] = thr->GetRPM();
    }
  }
  double fuel_lbs = 0.0;
  for (size_t i = 0; i < pul->GetNumTanks(); ++i)
    fuel_lbs += pul->GetTank((unsigned)i)->GetContents();
  out->fuel_kg = fuel_lbs * kLbsToKg;

  out->surf_aileron = fcs->GetDaLPos(JSBSim::ofNorm);
  out->surf_elevator = fcs->GetDePos(JSBSim::ofNorm);
  out->surf_rudder = fcs->GetDrPos(JSBSim::ofNorm);
  out->surf_flaps = fcs->GetDfPos(JSBSim::ofNorm);
  out->gear_pos = fcs->GetGearPos();

  out->status_flags = fdm->GetGroundReactions()->GetWOW() ? 1u : 0u;
  out->ground_queries = inst->ground ? inst->ground->query_count() : 0;
  out->trapped = 0;
}

}  // namespace

JSB_EXPORT(jsb_step_io)
int32_t jsb_step_io(int32_t h, const JsbInV1* in, JsbOutV1* out,
                    uint32_t substeps) {
  Instance* inst = resolve(h);
  if (!inst) return JSB_ERR_BAD_HANDLE;
  if (!inst->initialized) {
    set_error(inst, "step: not initialized");
    return JSB_ERR_NOT_LOADED;
  }
  if (!in || in->struct_size != sizeof(JsbInV1) || !out ||
      out->struct_size != sizeof(JsbOutV1)) {
    set_error(inst, "step: struct size mismatch");
    return JSB_ERR_STRUCT_SIZE;
  }
  if (substeps == 0 || substeps > 64) {
    set_error(inst, "step: substeps out of range");
    return JSB_ERR_BAD_ARG;
  }
  const double controls[] = {in->aileron,    in->elevator, in->rudder,
                             in->throttle,   in->flaps,    in->brake_left,
                             in->brake_right, in->pitch_trim};
  for (double c : controls)
    if (!finite_or_zero(c)) {
      set_error(inst, "step: non-finite control");
      return JSB_ERR_NOT_FINITE;
    }
  try {
    VfsScope scope(inst);
    auto fcs = inst->fdm->GetFCS();
    fcs->SetDaCmd(clampd(in->aileron, -1, 1));
    fcs->SetDeCmd(clampd(in->elevator, -1, 1));
    fcs->SetDrCmd(clampd(in->rudder, -1, 1));
    // Rudder pedals also command nosewheel steering (fcs/steer-cmd-norm).
    // Steerable gear scales this by max_steer (aircraft FCS channels, e.g. the
    // F-16 speed-scheduled steer-pos-deg, can override); retracted/fixed gear
    // ignores it, so it is inert in the air.
    fcs->SetDsCmd(clampd(in->rudder, -1, 1));
    fcs->SetThrottleCmd(-1, clampd(in->throttle, 0, 1));
    fcs->SetDfCmd(clampd(in->flaps, 0, 1));
    fcs->SetLBrake(clampd(in->brake_left, 0, 1));
    fcs->SetRBrake(clampd(in->brake_right, 0, 1));
    fcs->SetPitchTrimCmd(clampd(in->pitch_trim, -1, 1));
    fcs->SetGearCmd(in->gear_down ? 1.0 : 0.0);

    if (inst->ground) inst->ground->begin_batch();
    for (uint32_t i = 0; i < substeps; ++i)
      if (!inst->fdm->Run()) break;

    fill_out(inst, out);
    return JSB_OK;
  } catch (const std::exception& e) {
    set_error(inst, std::string("step: ") + e.what());
    return JSB_ERR_EXCEPTION;
  } catch (...) {
    set_error(inst, "step: unknown exception");
    return JSB_ERR_EXCEPTION;
  }
}

JSB_EXPORT(jsb_prop_id)
int32_t jsb_prop_id(int32_t h, const char* name, uint32_t nlen) {
  Instance* inst = resolve(h);
  if (!inst) return JSB_ERR_BAD_HANDLE;
  if (!name || nlen == 0 || nlen > 256) return JSB_ERR_BAD_ARG;
  try {
    auto pm = inst->fdm->GetPropertyManager();
    SGPropertyNode* node = pm->GetNode(std::string(name, nlen), false);
    if (!node) {
      set_error(inst, "property not found: " + std::string(name, nlen));
      return JSB_ERR_PROP;
    }
    inst->props.emplace_back(node);
    return (int32_t)inst->props.size();  // 1-based
  } catch (const std::exception& e) {
    set_error(inst, std::string("prop_id: ") + e.what());
    return JSB_ERR_EXCEPTION;
  }
}

JSB_EXPORT(jsb_prop_get)
int32_t jsb_prop_get(int32_t h, int32_t id, double* out_val) {
  Instance* inst = resolve(h);
  if (!inst) return JSB_ERR_BAD_HANDLE;
  if (!out_val || id <= 0 || (size_t)id > inst->props.size())
    return JSB_ERR_PROP;
  *out_val = inst->props[(size_t)id - 1]->getDoubleValue();
  return JSB_OK;
}

JSB_EXPORT(jsb_prop_set)
int32_t jsb_prop_set(int32_t h, int32_t id, double val) {
  Instance* inst = resolve(h);
  if (!inst) return JSB_ERR_BAD_HANDLE;
  if (id <= 0 || (size_t)id > inst->props.size()) return JSB_ERR_PROP;
  if (!finite_or_zero(val)) {
    set_error(inst, "prop_set: non-finite value");
    return JSB_ERR_NOT_FINITE;
  }
  inst->props[(size_t)id - 1]->setDoubleValue(val);
  return JSB_OK;
}

JSB_EXPORT(jsb_last_error)
int32_t jsb_last_error(int32_t h, char* buf, uint32_t cap) {
  const std::string* msg = &g_global_error;
  if (h != 0) {
    Instance* inst = resolve(h);
    if (inst) msg = &inst->last_error;
  }
  if (buf && cap > 0) {
    uint32_t n =
        (uint32_t)msg->size() < cap - 1 ? (uint32_t)msg->size() : cap - 1;
    std::memcpy(buf, msg->data(), n);
    buf[n] = '\0';
  }
  return (int32_t)msg->size();
}
