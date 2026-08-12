// kiro-wasm ground bridge: FGGroundCallback subclass that forwards terrain
// queries to the host through the single wasm import env.pree_ground_query.
// Contract in abi.h §KiroGround*: geodetic in, ECEF meters out, AGL meters.
// On miss the WGS84 ellipsoid at elevation 0 answers (FGDefaultGroundCallback
// behavior). Results are cached on quantized lat/lon within a step batch and
// capped at JSB_GROUND_MAX_PER_BATCH queries (excess served from cache/miss).
#ifndef JSBSIM_KIRO_WASM_GROUND_HOST_H
#define JSBSIM_KIRO_WASM_GROUND_HOST_H

#include "FGJSBBase.h"  // defines JSBSIM_API before the headers below need it
#include "input_output/FGGroundCallback.h"
#include "abi.h"

#define JSB_GROUND_MAX_PER_BATCH 64u

namespace kiro {

class HostGroundCallback : public JSBSim::FGGroundCallback {
public:
  // handle: facade instance handle, passed through to the host so it can
  // route per-aircraft queries. semimajor/semiminor in feet (JSBSim units).
  HostGroundCallback(int32_t handle, double semimajor_ft, double semiminor_ft);

  double GetAGLevel(double t, const JSBSim::FGLocation& location,
                    JSBSim::FGLocation& contact,
                    JSBSim::FGColumnVector3& normal,
                    JSBSim::FGColumnVector3& v,
                    JSBSim::FGColumnVector3& w) const override;

  void SetEllipse(double semimajor, double semiminor) override {
    a_ft_ = semimajor;
    b_ft_ = semiminor;
  }

  // Facade calls this at the start of every jsb_step_io batch.
  void begin_batch() const {
    queries_ = 0;
    cache_valid_ = false;
  }
  uint32_t query_count() const { return queries_; }

private:
  // Ellipsoid fallback (also the "miss" path).
  double ellipsoid_agl(const JSBSim::FGLocation& location,
                       JSBSim::FGLocation& contact,
                       JSBSim::FGColumnVector3& normal,
                       JSBSim::FGColumnVector3& v,
                       JSBSim::FGColumnVector3& w) const;

  int32_t handle_;
  double a_ft_, b_ft_;
  // Per-batch cache + counter (mutable: GetAGLevel is const).
  mutable uint32_t queries_ = 0;
  mutable bool cache_valid_ = false;
  mutable double cache_lat_ = 0, cache_lon_ = 0;
  mutable KiroGroundOutV1 cache_out_ = {};
};

}  // namespace kiro
#endif
