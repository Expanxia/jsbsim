#include "ground_host.h"

#include <cmath>

#include "math/FGLocation.h"
#include "math/FGColumnVector3.h"

extern "C" __attribute__((import_module("env"),
                          import_name("kiro_ground_query")))
int32_t kiro_ground_query(int32_t handle, const KiroGroundInV1* in,
                          KiroGroundOutV1* out);

namespace kiro {

namespace {
constexpr double kFtToM = 0.3048;
constexpr double kMToFt = 1.0 / 0.3048;
constexpr double kDegToRad = M_PI / 180.0;
// ~0.1 m quantization for the per-batch cache key.
constexpr double kCacheQuantDeg = 1.0e-6;
}  // namespace

HostGroundCallback::HostGroundCallback(int32_t handle, double semimajor_ft,
                                       double semiminor_ft)
    : handle_(handle), a_ft_(semimajor_ft), b_ft_(semiminor_ft) {}

double HostGroundCallback::ellipsoid_agl(const JSBSim::FGLocation& location,
                                         JSBSim::FGLocation& contact,
                                         JSBSim::FGColumnVector3& normal,
                                         JSBSim::FGColumnVector3& v,
                                         JSBSim::FGColumnVector3& w) const {
  // FGDefaultGroundCallback behavior at terrain elevation 0.
  JSBSim::FGLocation l = location;
  l.SetEllipse(a_ft_, b_ft_);
  double lat_r = l.GetGeodLatitudeRad();
  double lon_r = l.GetLongitude();
  contact.SetEllipse(a_ft_, b_ft_);
  contact.SetPositionGeodetic(lon_r, lat_r, 0.0);
  normal(1) = std::cos(lat_r) * std::cos(lon_r);
  normal(2) = std::cos(lat_r) * std::sin(lon_r);
  normal(3) = std::sin(lat_r);
  v.InitMatrix();
  w.InitMatrix();
  return l.GetGeodAltitude();
}

double HostGroundCallback::GetAGLevel(double, const JSBSim::FGLocation& location,
                                      JSBSim::FGLocation& contact,
                                      JSBSim::FGColumnVector3& normal,
                                      JSBSim::FGColumnVector3& v,
                                      JSBSim::FGColumnVector3& w) const {
  JSBSim::FGLocation l = location;
  l.SetEllipse(a_ft_, b_ft_);
  double lat_deg = l.GetGeodLatitudeDeg();
  double lon_deg = l.GetLongitudeDeg();

  const KiroGroundOutV1* result = nullptr;

  double qlat = std::round(lat_deg / kCacheQuantDeg);
  double qlon = std::round(lon_deg / kCacheQuantDeg);
  if (cache_valid_ && qlat == cache_lat_ && qlon == cache_lon_) {
    result = &cache_out_;
  } else if (queries_ < JSB_GROUND_MAX_PER_BATCH) {
    KiroGroundInV1 in = {};
    in.struct_size = sizeof(KiroGroundInV1);
    in.lat_deg = lat_deg;
    in.lon_deg = lon_deg;
    in.alt_msl_m = l.GetGeodAltitude() * kFtToM;
    KiroGroundOutV1 out = {};
    out.struct_size = sizeof(KiroGroundOutV1);
    ++queries_;
    int32_t rc = kiro_ground_query(handle_, &in, &out);
    if (rc == 0 && out.status == 0) {
      cache_out_ = out;
      cache_lat_ = qlat;
      cache_lon_ = qlon;
      cache_valid_ = true;
      result = &cache_out_;
    }
  }
  // No host answer (miss, error, or query cap): ellipsoid fallback.
  if (!result) return ellipsoid_agl(location, contact, normal, v, w);

  contact.SetEllipse(a_ft_, b_ft_);
  contact = JSBSim::FGLocation(JSBSim::FGColumnVector3(
      result->contact_ecef_m[0] * kMToFt, result->contact_ecef_m[1] * kMToFt,
      result->contact_ecef_m[2] * kMToFt));
  contact.SetEllipse(a_ft_, b_ft_);
  normal(1) = result->normal_ecef[0];
  normal(2) = result->normal_ecef[1];
  normal(3) = result->normal_ecef[2];
  v(1) = result->vel_ecef_mps[0] * kMToFt;
  v(2) = result->vel_ecef_mps[1] * kMToFt;
  v(3) = result->vel_ecef_mps[2] * kMToFt;
  w(1) = result->angvel_ecef_rps[0];
  w(2) = result->angvel_ecef_rps[1];
  w(3) = result->angvel_ecef_rps[2];
  return result->agl_m * kMToFt;
}

}  // namespace kiro
