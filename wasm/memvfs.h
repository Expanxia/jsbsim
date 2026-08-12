// kiro-wasm in-memory VFS. Instances each own a MemVfs; the facade marks one
// "active" around every JSBSim call so the two guarded hooks in JSBSim
// source (SGPath::exists, FGXMLFileRead::LoadXMLDocument) can consult it via
// the extern "C" functions jsbsim_memvfs_exists / jsbsim_memvfs_get.
#ifndef JSBSIM_KIRO_WASM_MEMVFS_H
#define JSBSIM_KIRO_WASM_MEMVFS_H

#include <map>
#include <string>

namespace pree {

class MemVfs {
public:
  // Returns a JSB_* status code (0 OK). Enforces the JSB_VFS_* limits.
  // The aircraft top-level XML gets <output> elements stripped at ingestion
  // (strip_output=true is set by the facade for every .xml file).
  int add(const std::string& path, const char* data, unsigned len,
          bool strip_output);
  const std::string* get(const std::string& normalized_path) const;
  bool exists(const std::string& normalized_path) const;
  void clear();

  // Path normalization: backslashes -> '/', strip leading "./" and "/".
  static std::string normalize(const std::string& p);
  // Removes top-level <output ...>...</output> / <output .../> elements.
  static std::string strip_output_elements(const std::string& xml);

private:
  std::map<std::string, std::string> files_;
  size_t total_bytes_ = 0;
};

// Active-VFS registration (single-threaded; facade scopes it per call).
void set_active_vfs(const MemVfs* vfs);
const MemVfs* active_vfs();

}  // namespace pree

extern "C" {
bool jsbsim_memvfs_exists(const char* utf8_path);
const char* jsbsim_memvfs_get(const char* utf8_path, unsigned int* out_len);
}

#endif
