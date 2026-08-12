#include "memvfs.h"
#include "abi.h"

namespace pree {

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
  // Removes FGOutput directive blocks — `<output name=... type=...>...` /
  // `<output file=.../>` — which are always ATTRIBUTED elements. FCS
  // component output bindings (`<output>some/property</output>`, never
  // attributed) MUST be preserved: stripping them silences control-surface
  // property writes (found the hard way: the f16 hook system, and the
  // c172p's surface-pos bindings, reference them).
  std::string out;
  out.reserve(xml.size());
  size_t i = 0;
  while (i < xml.size()) {
    size_t open = xml.find("<output", i);
    if (open == std::string::npos) {
      out.append(xml, i, std::string::npos);
      break;
    }
    char after = open + 7 < xml.size() ? xml[open + 7] : '\0';
    bool boundary = after == '>' || after == ' ' || after == '\t' ||
                    after == '\r' || after == '\n' || after == '/';
    size_t tag_end = xml.find('>', open);
    if (!boundary || tag_end == std::string::npos) {
      // Not an <output> element (e.g. "<outputs") or malformed: copy through.
      out.append(xml, i, open + 7 - i);
      i = open + 7;
      continue;
    }
    // Attribute test: '=' inside the opening tag => FGOutput directive.
    bool has_attrs = xml.find('=', open) < tag_end;
    if (!has_attrs) {
      // Component output binding: keep verbatim.
      out.append(xml, i, tag_end + 1 - i);
      i = tag_end + 1;
      continue;
    }
    out.append(xml, i, open - i);
    if (xml[tag_end - 1] == '/') {  // self-closing directive
      i = tag_end + 1;
      continue;
    }
    size_t close = xml.find("</output>", tag_end);
    if (close == std::string::npos) break;  // malformed; drop the rest
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

}  // namespace pree

extern "C" bool jsbsim_memvfs_exists(const char* utf8_path) {
  const pree::MemVfs* v = pree::active_vfs();
  if (!v || !utf8_path) return false;
  return v->exists(pree::MemVfs::normalize(utf8_path));
}

extern "C" const char* jsbsim_memvfs_get(const char* utf8_path,
                                         unsigned int* out_len) {
  const pree::MemVfs* v = pree::active_vfs();
  if (!v || !utf8_path || !out_len) return nullptr;
  const std::string* s = v->get(pree::MemVfs::normalize(utf8_path));
  if (!s) return nullptr;
  *out_len = static_cast<unsigned int>(s->size());
  return s->data();
}
