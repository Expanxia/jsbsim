// Native reference trajectory for wasm-vs-native validation.
// MUST match the harness scenario (wasm/harness/src/main.rs `SCENARIO`):
//   dt=1/120, c172p, geodetic lat 37 lon -122 alt 1000 m MSL, heading 90,
//   TAS 55 m/s, gamma 0, gear down, engines running, throttle 0.65,
//   elevator doublet -0.1 @ [30,31)s then +0.1 @ [31,32)s.
// Prints raw JSBSim-unit samples (no SI conversion) every 1 s so the diff
// exercises the FDM, not unit conversions.
#include <FGFDMExec.h>
#include <initialization/FGInitialCondition.h>
#include <models/FGFCS.h>
#include <models/FGPropagate.h>
#include <models/FGAuxiliary.h>
#include <models/FGPropulsion.h>
#include <cstdio>

int main(int argc, char** argv) {
  const char* root = argc > 1 ? argv[1] : "..";
  JSBSim::FGFDMExec fdm;
  fdm.SetDebugLevel(0);
  fdm.SetRootDir(SGPath(root));
  fdm.SetAircraftPath(SGPath("aircraft"));
  fdm.SetEnginePath(SGPath("engine"));
  fdm.SetSystemsPath(SGPath("systems"));
  fdm.Setdt(1.0 / 120.0);
  if (!fdm.LoadModel("c172p")) {
    std::fprintf(stderr, "LoadModel failed\n");
    return 1;
  }
  auto ic = fdm.GetIC();
  ic->SetGeodLatitudeDegIC(37.0);
  ic->SetLongitudeDegIC(-122.0);
  ic->SetAltitudeASLFtIC(1000.0 / 0.3048);
  ic->SetPsiDegIC(90.0);
  ic->SetVtrueKtsIC(55.0 / 0.514444);
  ic->SetFlightPathAngleDegIC(0.0);
  auto fcs = fdm.GetFCS();
  fcs->SetGearCmd(1.0);
  if (!fdm.RunIC()) {
    std::fprintf(stderr, "RunIC failed\n");
    return 1;
  }
  fdm.GetPropulsion()->InitRunning(-1);

  auto prop = fdm.GetPropagate();
  auto aux = fdm.GetAuxiliary();
  std::printf("time_s,lat_geod_deg,lon_deg,alt_asl_ft,phi_rad,theta_rad,"
              "psi_rad,vn_fps,ve_fps,vd_fps,alpha_rad,vcas_kts\n");
  for (int i = 0; i <= 12000; ++i) {
    double t = fdm.GetSimTime();
    if (i % 120 == 0) {
      const auto& e = prop->GetEuler();
      const auto& v = prop->GetVel();
      std::printf("%.6f,%.12f,%.12f,%.8f,%.10f,%.10f,%.10f,%.8f,%.8f,%.8f,"
                  "%.10f,%.8f\n",
                  t, prop->GetGeodLatitudeDeg(), prop->GetLongitudeDeg(),
                  prop->GetAltitudeASL(), e(1), e(2), e(3), v(1), v(2), v(3),
                  aux->Getalpha(), aux->GetVcalibratedKTS());
    }
    if (i == 12000) break;
    fcs->SetDaCmd(0.0);
    double de = 0.0;
    if (t >= 30.0 && t < 31.0) de = -0.1;
    else if (t >= 31.0 && t < 32.0) de = 0.1;
    fcs->SetDeCmd(de);
    fcs->SetDrCmd(0.0);
    fcs->SetThrottleCmd(-1, 0.65);
    if (!fdm.Run()) break;
  }
  return 0;
}
