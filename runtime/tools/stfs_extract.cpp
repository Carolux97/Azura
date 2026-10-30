// Lists and extracts an Xbox 360 STFS package (a title update, a DLC) with the SDK's reader, to
// inspect what it holds. Not part of the normal build:
//   cmake --build --preset <preset> --target azura_stfs_extract
// Usage: azura_stfs_extract <package> [output folder]   (without a folder it only lists)

#include <cstdio>
#include <filesystem>
#include <memory>
#include <queue>
#include <vector>

#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "Usage: %s <package> [output folder]\n", argv[0]);
    return 1;
  }
  auto device = std::make_unique<rex::filesystem::StfsContainerDevice>("", std::filesystem::path(argv[1]));
  if (!device->Initialize()) {
    std::fprintf(stderr, "Not an STFS package: %s\n", argv[1]);
    return 1;
  }
  const std::filesystem::path out = argc > 2 ? std::filesystem::path(argv[2]) : std::filesystem::path();
  std::queue<rex::filesystem::Entry*> queue;
  queue.push(device->ResolvePath(""));
  while (!queue.empty()) {
    auto* entry = queue.front();
    queue.pop();
    for (auto& child : entry->children()) queue.push(child.get());
    const bool directory = entry->attributes() & rex::filesystem::kFileAttributeDirectory;
    std::printf("%s %12llu %s\n", directory ? "d" : "f", static_cast<unsigned long long>(entry->size()),
                entry->path().c_str());
    if (out.empty() || directory) continue;
    // Package paths use backslashes; on the host they are folders.
    std::string relative = entry->path();
    for (char& c : relative) {
      if (c == '\\') c = '/';
    }
    const auto dest = out / relative;
    std::filesystem::create_directories(dest.parent_path());
    rex::filesystem::File* in = nullptr;
    if (entry->Open(rex::filesystem::FileAccess::kFileReadData, &in) != 0) continue;
    std::vector<uint8_t> data(entry->size());
    size_t read = 0;
    in->ReadSync(std::span<uint8_t>(data.data(), data.size()), 0, &read);
    in->Destroy();
    if (FILE* f = std::fopen(dest.string().c_str(), "wb")) {
      std::fwrite(data.data(), 1, read, f);
      std::fclose(f);
    }
  }
  return 0;
}
