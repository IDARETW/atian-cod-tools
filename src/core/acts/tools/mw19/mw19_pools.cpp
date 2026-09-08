#include <includes.hpp>
#include "mw19_schema.hpp"
#include "mw19_payload.hpp"
#include "mw19_mesh.hpp"
#include "mw19_sound.hpp"
#include "mw19_xpak.hpp"
#include <deps/oodle.hpp>
#include <TlHelp32.h>
#include <fstream>
#include <zlib.h>

namespace {
    using namespace tool::mw19::schema;

    class ReadOnlyProcess {
        HANDLE handle{};

      public:
        DWORD pid{};
        uint64_t base{};
        ReadOnlyProcess(const std::string& module, DWORD requestedPid) {
            HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snapshot == INVALID_HANDLE_VALUE)
                throw std::runtime_error("Cannot enumerate processes");
            PROCESSENTRY32 entry{};
            entry.dwSize = sizeof(entry);
            std::vector<DWORD> matches;
            if (Process32First(snapshot, &entry)) {
                do {
                    if ((!requestedPid || entry.th32ProcessID == requestedPid) &&
                        !_stricmp(entry.szExeFile, module.c_str()))
                        matches.push_back(entry.th32ProcessID);
                } while (Process32Next(snapshot, &entry));
            }
            CloseHandle(snapshot);
            if (matches.size() != 1)
                throw std::runtime_error(
                    matches.empty() ? "Target game is not running" : "More than one matching game process; supply --pid"
                );
            pid = matches[0];
            snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
            if (snapshot == INVALID_HANDLE_VALUE)
                throw std::runtime_error("Cannot enumerate target modules");
            MODULEENTRY32 mod{};
            mod.dwSize = sizeof(mod);
            if (Module32First(snapshot, &mod)) {
                do {
                    if (!_stricmp(mod.szModule, module.c_str())) {
                        base = reinterpret_cast<uint64_t>(mod.modBaseAddr);
                        break;
                    }
                } while (Module32Next(snapshot, &mod));
            }
            CloseHandle(snapshot);
            if (!base)
                throw std::runtime_error("Target module not found");
            handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
            if (!handle)
                throw std::runtime_error("Cannot open target for read access");
        }
        ~ReadOnlyProcess() {
            if (handle)
                CloseHandle(handle);
        }
        ReadOnlyProcess(const ReadOnlyProcess&) = delete;
        bool IsRunning() const {
            DWORD code{};
            return GetExitCodeProcess(handle, &code) && code == STILL_ACTIVE;
        }
        bool Read(uint64_t address, void* target, size_t size) const {
            SIZE_T actual{};
            return ReadProcessMemory(handle, reinterpret_cast<void*>(address), target, size, &actual) && actual == size;
        }
    };

    int mw19pools(int argc, const char* argv[]) {
        std::filesystem::path out = "output/mw19";
        bool started{};
        try {
            std::filesystem::path schemaFile = utils::GetProgDir() / "data/mw19/schema.json";
            std::string profileId = "replay-1.20";
            std::vector<std::string> selected;
            std::vector<std::filesystem::path> packagePaths;
            std::filesystem::path oodlePath;
            bool catalog = false, all = false, headers = false, fields = false, payloads = false, test = false;
            bool geometry = false;
            bool audioSamples = false;
            size_t limitPerPool = SIZE_MAX;
            DWORD pid{};
            for (int i = 2; i < argc; i++) {
                std::string arg = argv[i];
                auto value = [&]() -> const char* {
                    if (++i >= argc)
                        throw std::runtime_error("Missing value for " + arg);
                    return argv[i];
                };
                if (arg == "--schema")
                    schemaFile = value();
                else if (arg == "--profile")
                    profileId = value();
                else if (arg == "--output" || arg == "-o")
                    out = value();
                else if (arg == "--pid")
                    pid = static_cast<DWORD>(std::stoul(value()));
                else if (arg == "--xpak")
                    packagePaths.emplace_back(value());
                else if (arg == "--oodle")
                    oodlePath = value();
                else if (arg == "--catalog")
                    catalog = true;
                else if (arg == "--all")
                    all = true;
                else if (arg == "--headers")
                    headers = true;
                else if (arg == "--fields")
                    fields = true;
                else if (arg == "--payloads")
                    payloads = true;
                else if (arg == "--geometry") {
                    geometry = true;
                    payloads = true;
                } else if (arg == "--audio") {
                    audioSamples = true;
                    payloads = true;
                } else if (arg == "--dump") {
                    headers = true;
                    fields = true;
                    payloads = true;
                } else if (arg == "--test") {
                    test = true;
                    headers = true;
                    fields = true;
                    payloads = true;
                } else if (arg == "--limit-per-pool")
                    limitPerPool = std::stoull(value());
                else if (arg == "--help" || arg == "-h") {
                    LOG_INFO(
                        "mw19pools --catalog | [--all | pool ...] [--test|--dump|--headers|--fields|--payloads] "
                        "[--limit-per-pool n] [--pid n] [--profile replay-1.20|game-test] [-o directory] [--schema "
                        "file] [--xpak file.xpak ...] [--oodle absolute.dll] [--geometry] [--audio]"
                    );
                    LOG_INFO(
                        "--test validates one asset per loaded pool in memory and writes a report only; "
                        "--limit-per-pool may select up to 32 samples."
                    );
                    LOG_INFO("--geometry adds XModelSurfs base geometry as GLB; --test keeps it in memory.");
                    LOG_INFO(
                        "--audio extracts FLAC samples from loaded sound banks; --test checks one sound per sampled "
                        "bank in memory."
                    );
                    return tool::OK;
                } else if (arg.starts_with('-'))
                    throw std::runtime_error("Unknown option: " + arg);
                else
                    selected.push_back(arg);
            }
            if (test && limitPerPool == SIZE_MAX)
                limitPerPool = 1;
            if (test && (!limitPerPool || limitPerPool > 32))
                throw std::runtime_error("Test samples per pool must be between 1 and 32");
            Database database(schemaFile);
            const Json& profile = database.Profile(profileId);
            std::set<unsigned> ids;
            for (const auto& name : selected)
                ids.insert(database.Pool(profile, name).at("id").get<unsigned>());
            if (all || selected.empty())
                for (const auto& p : profile.at("pools"))
                    ids.insert(p.at("id").get<unsigned>());
            if (catalog) {
                WriteJsonAtomic(out / "pools.json", profile);
                for (const auto& p : profile.at("pools")) {
                    LOG_INFO(
                        "{:3} {:26} size=0x{:x} capacity={} {}",
                        p.at("id").get<unsigned>(),
                        p.at("name").get<std::string>(),
                        p.at("size").get<unsigned>(),
                        p.at("capacity").get<unsigned>(),
                        p.at("status").get<std::string>()
                    );
                }
                LOG_INFO(
                    "Wrote {} pool definitions to {}",
                    profile.at("pool_count").get<unsigned>(),
                    (out / "pools.json").string()
                );
                return tool::OK;
            }
            if (!profile.contains("entry_table_rva"))
                throw std::runtime_error("No verified entry-table binding for profile " + profileId);
            namespace xpak = tool::mw19::xpak;
            if (packagePaths.size() > 64)
                throw std::runtime_error("At most 64 explicit XPak archives may be opened in one run");
            if ((!packagePaths.empty() || !oodlePath.empty()) && profileId != "replay-1.20")
                throw std::runtime_error("XPak retrieval is only verified for Replay PC");
            std::vector<xpak::Archive> archives;
            size_t indexBytes = 0;
            for (const auto& path : packagePaths) {
                auto archive = xpak::Archive::Open(path);
                indexBytes += archive.Entries().size() * sizeof(xpak::Entry);
                if (indexBytes > 128 * 1024 * 1024)
                    throw std::runtime_error("Combined XPak indexes exceed 128 MiB budget");
                archives.push_back(std::move(archive));
            }
            deps::oodle::Oodle oodle;
            xpak::OodleDecoder decode;
            if (!oodlePath.empty()) {
                if (!oodlePath.is_absolute())
                    throw std::runtime_error("Use an absolute Oodle DLL path");
                if (!oodle.LoadOodle(oodlePath.string().c_str()))
                    throw std::runtime_error("Cannot load supplied Oodle library");
                decode = [&](std::span<const uint8_t> input, std::span<uint8_t> output) {
                    const auto size = oodle.Decompress(
                        input.data(),
                        static_cast<uint32_t>(input.size()),
                        output.data(),
                        static_cast<uint32_t>(output.size()),
                        deps::oodle::OODLE_FS_YES
                    );
                    return size >= 0 && static_cast<size_t>(size) == output.size();
                };
            }
            ReadOnlyProcess proc(profile.at("module"), pid);
            MemoryReader read = [&](uint64_t a, void* b, size_t n) { return proc.Read(a, b, n); };
            ValidateProfile(read, proc.base, profile);
            auto entries = ReadReplayEntries(read, proc.base + profile.at("entry_table_rva").get<uint64_t>(), profile);
            Json manifest{ { "schema_version", 1 },          { "profile", profileId },    { "pid", proc.pid },
                           { "module_base", proc.base },     { "complete", false },       { "test_only", test },
                           { "pools", profile.at("pools") }, { "assets", Json::array() }, { "failures", 0 } };
            auto manifestPath = out / "manifest.json";
            WriteJsonAtomic(manifestPath, manifest);
            started = true;
            std::ofstream journal(out / "assets.jsonl", std::ios::binary | std::ios::trunc);
            if (!journal)
                throw std::runtime_error("Cannot create asset journal");
            manifest["journal"] = "assets.jsonl";
            manifest["geometry_requested"] = geometry;
            manifest["audio_requested"] = audioSamples;
            manifest["limit_per_pool"] = limitPerPool == SIZE_MAX ? Json(nullptr) : Json(limitPerPool);
            manifest["package_archives"] = Json::array();
            for (const auto& path : packagePaths)
                manifest["package_archives"].push_back(path.string());
            size_t failures{}, partials{};
            std::map<unsigned, size_t> poolCounts;
            for (const auto& entry : entries) {
                if (!ids.contains(entry.type))
                    continue;
                if (poolCounts[entry.type]++ >= limitPerPool)
                    continue;
                const auto& pool = profile.at("pools").at(entry.type);
                Json asset{ { "entry_index", entry.index },
                            { "pool_id", entry.type },
                            { "pool", pool.at("name") },
                            { "address", entry.header },
                            { "zone", entry.zoneFlags & 0x7ff },
                            { "zone_flags", entry.zoneFlags },
                            { "next_hash", entry.nextHash },
                            { "next_stashed", entry.nextStashed },
                            { "in_use", entry.inUse } };
                bool partial{}, failed{};
                try {
                    if (!proc.IsRunning())
                        throw std::runtime_error("Target exited during capability test or export");
                    if (pool.at("name_offset").is_null())
                        throw std::runtime_error("Allocated asset in unused pool");
                    uint64_t namePtr{};
                    if (!read(entry.header + pool.at("name_offset").get<uint64_t>(), &namePtr, 8))
                        throw std::runtime_error("Cannot read asset name pointer");
                    std::string name = ReadString(read, namePtr);
                    asset["name"] = name;
                    if (name.starts_with(','))
                        name.erase(0, 1);
                    const auto fileRoot = out / "assets" / pool.at("name").get<std::string>();
                    std::filesystem::path path;
                    if (!test && (headers || fields || payloads)) {
                        try {
                            path = AssetPath(fileRoot, name);
                        } catch (const std::exception&) {
                            // Engine names such as "*" are valid asset names,
                            // but not Windows filenames. Preserve the name in
                            // the manifest and use its unique entry index.
                            path = fileRoot / "_encoded" / std::to_string(entry.index);
                            asset["filename_encoded"] = true;
                        }
                    }
                    // Different zones may hold assets with the same name. Keep
                    // every allocated entry instead of overwriting an override.
                    path += ".entry-" + std::to_string(entry.index);
                    auto check = [&](const std::string& stage, auto action) {
                        try {
                            action();
                            asset[stage + "_status"] = "ok";
                        } catch (const PayloadUnavailable& unavailable) {
                            partial = true;
                            asset[stage + "_status"] = "unavailable";
                            asset[stage + "_reason"] = unavailable.reason;
                            asset[stage + "_detail"] = unavailable.what();
                        } catch (const std::exception& error) {
                            failed = true;
                            asset[stage + "_status"] = "failed";
                            asset[stage + "_error"] = error.what();
                        }
                    };
                    if (headers)
                        check("header", [&] {
                            const auto size = pool.at("size").get<size_t>();
                            if (!size || size > 1024 * 1024)
                                throw std::runtime_error("Invalid asset size");
                            std::vector<uint8_t> data(size);
                            if (!read(entry.header, data.data(), size))
                                throw std::runtime_error("Cannot read asset header");
                            asset["header_bytes"] = data.size();
                            if (!test) {
                                auto binary = path;
                                binary += ".bin";
                                std::filesystem::create_directories(binary.parent_path());
                                if (!utils::WriteFile(binary, data.data(), data.size()))
                                    throw std::runtime_error("Cannot write header");
                                asset["header_file"] = binary.lexically_relative(out).generic_string();
                            }
                        });
                    const bool structuredPayload = payloads && !HasPayloadExporter(pool.at("name"));
                    Json fieldRecord;
                    if (fields || structuredPayload)
                        check("field", [&] {
                            if (pool.at("root_type").is_null())
                                throw std::runtime_error(
                                    "Pool layout is pending: " + pool.at("name").get<std::string>()
                                );
                            Inspector inspector(database, read, {}, profileId);
                            fieldRecord = inspector.Inspect(pool.at("root_type"), entry.header);
                            auto& value = fieldRecord;
                            value["layout_evidence"] = pool.at("status");
                            asset["field_read_errors"] = value.at("read_errors");
                            asset["unresolved_pointers"] = value.at("unresolved_pointers");
                            asset["unresolved_unions"] = value.at("unresolved_unions");
                            asset["external_payloads"] = value.at("external_payloads");
                            asset["field_bytes_read"] = value.at("bytes_read");
                            asset["field_nodes"] = value.at("nodes");
                            asset["field_issues"] = value.at("issues");
                            asset["field_issue_count"] = value.at("issue_count");
                            asset["runtime_references"] = value.at("runtime_references");
                            partial |= value.at("unresolved_pointers").get<size_t>() ||
                                       value.at("unresolved_unions").get<size_t>() ||
                                       value.at("external_payloads").get<size_t>();
                            if (!test && fields &&
                                (!structuredPayload || partial || value.at("read_errors").get<size_t>())) {
                                auto json = path;
                                json += ".json";
                                WriteJsonAtomic(json, value);
                                asset["fields_file"] = json.lexically_relative(out).generic_string();
                            }
                            if (value.at("read_errors").get<size_t>())
                                throw std::runtime_error("Field traversal contains read errors; see field_issues");
                        });
                    if (payloads && HasPayloadExporter(pool.at("name"))) {
                        check("payload", [&] {
                            PackageReader packages;
                            if (!archives.empty()) {
                                asset["package_reads"] = Json::array();
                                packages = [&](uint64_t key, size_t size) -> std::vector<uint8_t> {
                                    // Explicit later paths take priority for keys present in several packages.
                                    for (size_t i = archives.size(); i-- > 0;) {
                                        if (!archives[i].Find(key))
                                            continue;
                                        auto& record = asset["package_reads"].emplace_back(
                                            Json{ { "key", key },
                                                  { "archive", packagePaths[i].string() },
                                                  { "expected_bytes", size },
                                                  { "status", "pending" } }
                                        );
                                        try {
                                            auto bytes = archives[i].Extract(key, size, decode);
                                            record["status"] = "ok";
                                            record["decoded_bytes"] = bytes.size();
                                            record["crc32"] = crc32(0, bytes.data(), static_cast<uInt>(bytes.size()));
                                            return bytes;
                                        } catch (const xpak::DecoderUnavailable& e) {
                                            record["status"] = "unavailable";
                                            record["error"] = e.what();
                                            throw PayloadUnavailable("package_decoder_unavailable", e.what());
                                        } catch (const std::exception& e) {
                                            record["status"] = "failed";
                                            record["error"] = e.what();
                                            throw;
                                        }
                                    }
                                    asset["package_reads"].push_back(
                                        Json{ { "key", key }, { "expected_bytes", size }, { "status", "unavailable" } }
                                    );
                                    throw PayloadUnavailable(
                                        "package_key_missing",
                                        "Required XPak key is absent from the supplied archives"
                                    );
                                };
                            }
                            auto payload = ExportPayload(
                                read,
                                pool.at("name"),
                                entry.header,
                                128 * 1024 * 1024,
                                profileId,
                                packages
                            );
                            asset["payload_bytes"] = payload.data.size();
                            asset["payload_crc32"] =
                                crc32(0, payload.data.data(), static_cast<uInt>(payload.data.size()));
                            if (!test) {
                                auto payloadPath = path;
                                payloadPath += ".payload" + payload.extension;
                                std::filesystem::create_directories(payloadPath.parent_path());
                                if (!utils::WriteFile(payloadPath, payload.data.data(), payload.data.size()))
                                    throw std::runtime_error("Cannot write payload");
                                asset["payload_file"] = payloadPath.lexically_relative(out).generic_string();
                            }
                            asset["payload_format"] = payload.format;
                            asset["payload_status"] = "ok";
                            if (audioSamples &&
                                (pool.at("name") == "soundbank" || pool.at("name") == "soundbanktransient")) {
                                check("audio", [&] {
                                    const auto& data = payload.data;
                                    MemoryReader bankRead = [&](uint64_t at, void* out, size_t n) {
                                        if (at > data.size() || n > data.size() - at)
                                            return false;
                                        std::memcpy(out, data.data() + at, n);
                                        return true;
                                    };
                                    tool::mw19::sound::Bank bank(bankRead, data.size());
                                    asset["audio_total_samples"] = bank.Entries().size();
                                    asset["audio_samples"] = Json::array();
                                    size_t unavailable = 0, errors = 0;
                                    for (const auto& sample : bank.Entries()) {
                                        if (test && !asset["audio_samples"].empty())
                                            break;
                                        Json record = tool::mw19::sound::Describe(sample);
                                        try {
                                            const auto flac = bank.ExtractFlac(sample.key);
                                            record["bytes"] = flac.data.size();
                                            record["crc32"] =
                                                crc32(0, flac.data.data(), static_cast<uInt>(flac.data.size()));
                                            if (!test) {
                                                auto soundPath = path;
                                                soundPath += ".sound-" + std::to_string(sample.key) + ".flac";
                                                std::filesystem::create_directories(soundPath.parent_path());
                                                if (!utils::WriteFile(soundPath, flac.data.data(), flac.data.size()))
                                                    throw std::runtime_error("Cannot write sound sample");
                                                record["file"] = soundPath.lexically_relative(out).generic_string();
                                            }
                                            record["status"] = "ok";
                                        } catch (const PayloadUnavailable& error) {
                                            record["status"] = "unavailable";
                                            record["reason"] = error.reason;
                                            record["error"] = error.what();
                                            ++unavailable;
                                        } catch (const std::exception& error) {
                                            record["status"] = "failed";
                                            record["error"] = error.what();
                                            ++errors;
                                        }
                                        asset["audio_samples"].push_back(std::move(record));
                                    }
                                    if (errors)
                                        throw std::runtime_error("Sound sample export failed; see audio_samples");
                                    if (unavailable)
                                        throw PayloadUnavailable(
                                            "sound_samples_unavailable",
                                            "Some sound codecs or samples are unavailable"
                                        );
                                });
                            }
                            if (geometry && pool.at("name") == "xmodelsurfs") {
                                asset["geometry_scope"] = "base surface geometry";
                                check("geometry", [&] {
                                    if (profileId != "replay-1.20")
                                        throw std::runtime_error("Geometry layout is only available for Replay PC");
                                    const auto mesh = ExportReplayGeometry(read, entry.header, payload.data);
                                    asset["geometry_surfaces"] = mesh.surfaces;
                                    asset["geometry_vertices"] = mesh.vertices;
                                    asset["geometry_triangles"] = mesh.triangles;
                                    asset["geometry_bytes"] = mesh.payload.data.size();
                                    asset["geometry_crc32"] =
                                        crc32(0, mesh.payload.data.data(), static_cast<uInt>(mesh.payload.data.size()));
                                    if (!test) {
                                        auto meshPath = path;
                                        meshPath += mesh.payload.extension;
                                        std::filesystem::create_directories(meshPath.parent_path());
                                        if (!utils::WriteFile(
                                                meshPath,
                                                mesh.payload.data.data(),
                                                mesh.payload.data.size()
                                            ))
                                            throw std::runtime_error("Cannot write geometry");
                                        asset["geometry_file"] = meshPath.lexically_relative(out).generic_string();
                                    }
                                });
                            }
                        });
                        if (geometry && pool.at("name") == "xmodelsurfs" && asset["payload_status"] != "ok") {
                            asset["geometry_status"] = "unavailable";
                            asset["geometry_reason"] = "shared_buffer_unavailable";
                        }
                        if (audioSamples &&
                            (pool.at("name") == "soundbank" || pool.at("name") == "soundbanktransient") &&
                            asset["payload_status"] != "ok") {
                            asset["audio_status"] = "unavailable";
                            asset["audio_reason"] = "sound_bank_unavailable";
                        }
                    } else if (structuredPayload) {
                        check("payload", [&] {
                            auto payload = ExportStructuredAsset(profile, pool, std::move(fieldRecord));
                            asset["payload_bytes"] = payload.data.size();
                            asset["payload_crc32"] =
                                crc32(0, payload.data.data(), static_cast<uInt>(payload.data.size()));
                            asset["payload_format"] = payload.format;
                            if (!test) {
                                auto output = path;
                                output += payload.extension;
                                std::filesystem::create_directories(output.parent_path());
                                if (!utils::WriteFile(output, payload.data.data(), payload.data.size()))
                                    throw std::runtime_error("Cannot write structured asset");
                                asset["payload_file"] = output.lexically_relative(out).generic_string();
                                if (fields)
                                    asset["fields_file"] = asset["payload_file"];
                            }
                        });
                    }
                    asset["status"] = failed ? "failed" : partial ? "partial" : "ok";
                    if (failed)
                        ++failures;
                    else if (partial)
                        partials++;
                } catch (const std::exception& error) {
                    if (!proc.IsRunning())
                        throw std::runtime_error("Target exited; completed records are retained in assets.jsonl");
                    asset["status"] = "failed";
                    asset["error"] = error.what();
                    failures++;
                }
                journal << asset.dump(-1, ' ', false, Json::error_handler_t::replace) << '\n';
                if (!journal)
                    throw std::runtime_error("Asset journal write failed");
                manifest["assets"].push_back(std::move(asset));
                // Append the asset journal once. Rewriting a growing manifest
                // every batch is quadratic for 100,000+ loaded assets.
                if (manifest["assets"].size() % 256 == 0 || manifest["assets"].back().at("status") == "failed") {
                    journal.flush();
                    manifest["failures"] = failures;
                    WriteJsonAtomic(
                        out / "progress.json",
                        Json{ { "complete", false },
                              { "assets", manifest["assets"].size() },
                              { "failures", failures },
                              { "journal", "assets.jsonl" } }
                    );
                }
            }
            manifest["complete"] = true;
            manifest["failures"] = failures;
            manifest["partials"] = partials;
            manifest["success"] = failures == 0 && partials == 0;
            journal.flush();
            if (!journal)
                throw std::runtime_error("Asset journal flush failed");
            WriteJsonAtomic(manifestPath, manifest);
            WriteJsonAtomic(
                out / "progress.json",
                Json{ { "complete", true },
                      { "assets", manifest["assets"].size() },
                      { "failures", failures },
                      { "journal", "assets.jsonl" } }
            );
            LOG_INFO(
                "Recorded {} assets from {} selected pools; {} partial, {} failed. Manifest: {}",
                manifest["assets"].size(),
                ids.size(),
                partials,
                failures,
                manifestPath.string()
            );
            return failures || partials ? tool::BASIC_ERROR : tool::OK;
        } catch (const std::exception& error) {
            if (started) {
                try {
                    WriteJsonAtomic(
                        out / "progress.json",
                        Json{ { "complete", false }, { "error", error.what() }, { "journal", "assets.jsonl" } }
                    );
                } catch (...) {
                    LOG_ERROR("Could not persist failure status");
                }
            }
            LOG_ERROR("MW2019 pool operation failed: {}", error.what());
            return tool::BASIC_ERROR;
        }
    }
    ADD_TOOL(
        mw19pools, "mw19", " [--catalog] [--all|pool ...] [--headers] [--fields] [options]",
        "Inspect versioned MW2019 pools and read allocated game assets", mw19pools
    );
} // namespace
