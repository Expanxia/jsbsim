#include "memvfs.h"
#include "abi.h"

namespace kiro {

namespace {
const MemVfs* g_active = nullptr;
}

void set_active_vfs(const MemVfs* vfs) { g_active = vfs; }
const MemVfs* active_vfs() { return g_active; }

std::string MemVfs::normalize(const std::string& p) {
  std::string out;
  out.reserve(p.size());
  for (char c : p) out.push_back(c == '\\' ? '/' : c);
  while (out.rfind("./", 0) == 0) out.erase(0, 2);
  while (!out.empty() && out.front() == '/') out.erase(0, 1);
  return out;
}

std::string MemVfs::strip_output_elements(const std::string& xml) {
  // Lightweight scan: removes every <output ...>...</output> and
  // <output .../> element. JSBSim aircraft never nest <output>.
  std::string out;
  out.reserve(xml.size());
  size_t i = 0;
  while (i < xml.size()) {
    size_t open = xml.find("<output", i);
    // Must be a real element boundary: "<output>" or "<output " or "<output/".
    if (open != std::string::npos) {
      char after = open + 7 < xml.size() ? xml[open + 7] : '\0';
      if (after != '>' && after != ' ' && after != '\t' && after != '\r' &&
          after != '\n' && after != '/') {
        out.append(xml, i, open + 7 - i);
        i = open + 7;
        continue;
      }
    }
    if (open == std::string::npos) {
      out.append(xml, i, std::string::npos);
      break;
    }
    out.append(xml, i, open - i);
    // Find the end of the opening tag.
    size_t tag_end = xml.find('>', open);
    if (tag_end == std::string::npos) break;  // malformed; drop the rest
    if (xml[tag_end - 1] == '/') {            // self-closing
      i = tag_end + 1;
      continue;
    }
    size_t close = xml.find("</output>", tag_end);
    if (close == std::string::npos) break;    // malformed; drop the rest
    i = close + 9;
  }
  return out;
}

int MemVfs::add(const std::string& path, const char* data, unsigned len,
                bool strip_output) {
  if (path.empty() || path.size() > JSB_VFS_MAX_PATH) return JSB_ERR_VFS_LIMIT;
  if (len > JSB_VFS_MAX_FILE_BYTES) return JSB_ERR_VFS_LIMIT;
  if (files_.size() >= JSB_VFS_MAX_FILES) return JSB_ERR_VFS_LIMIT;

  std::string key = normalize(path);
  std::string content(data, len);
  bool is_xml = key.size() > 4 && key.compare(key.size() - 4, 4, ".xml") == 0;
  if (strip_output && is_xml && content.find("<output") != std::string::npos)
    content = strip_output_elements(content);

  size_t prev = 0;
  auto it = files_.find(key);
  if (it != files_.end()) prev = it->second.size();
  if (total_bytes_ - prev + content.size() > JSB_VFS_MAX_TOTAL)
    return JSB_ERR_VFS_LIMIT;
  total_bytes_ = total_bytes_ - prev + content.size();
  files_[key] = std::move(content);
  return JSB_OK;
}

const std::string* MemVfs::get(const std::string& normalized_path) const {
  auto it = files_.find(normalized_path);
  return it == files_.end() ? nullptr : &it->second;
}

bool MemVfs::exists(const std::string& normalized_path) const {
  return files_.count(normalized_path) != 0;
}

void MemVfs::clear() {
  files_.clear();
  total_bytes_ = 0;
}

}  // namespace kiro

extern "C" bool jsbsim_memvfs_exists(const char* utf8_path) {
  const kiro::MemVfs* v = kiro::active_vfs();
  if (!v || !utf8_path) return false;
  return v->exists(kiro::MemVfs::normalize(utf8_path));
}

extern "C" const char* jsbsim_memvfs_get(const char* utf8_path,
                                         unsigned int* out_len) {
  const kiro::MemVfs* v = kiro::active_vfs();
  if (!v || !utf8_path || !out_len) return nullptr;
  const std::string* s = v->get(kiro::MemVfs::normalize(utf8_path));
  if (!s) return nullptr;
  *out_len = static_cast<unsigned int>(s->size());
  return s->data();
}
