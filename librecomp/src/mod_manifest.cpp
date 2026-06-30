#include <unordered_map>

#include "json/json.hpp"

#include "recompiler/context.h"
#include "librecomp/files.hpp"
#include "librecomp/mods.hpp"

namespace {

using json = nlohmann::json;
using recomp::mods::ModOpenError;

// Consume an already-opened stream and decode it as a JSON document. Any read or
// parse failure is reported through the return value rather than an exception.
bool decode_json_stream(std::ifstream stream, json &out) {
    if (!stream.good()) {
        return false;
    }

    try {
        stream >> out;
    }
    catch (const json::parse_error &) {
        return false;
    }

    return true;
}

// Decode the file at the given path, falling back to its backup copy when the
// primary file is missing or malformed.
bool decode_json_file(const std::filesystem::path &path, json &out) {
    if (decode_json_stream(std::ifstream{ path }, out)) {
        return true;
    }
    return decode_json_stream(recomp::open_input_backup_file(path), out);
}

// Pull a value of JSON type JsonT out of `value`, writing it to `out`. Returns
// false (leaving `out` untouched) when the value is not exactly that JSON type.
template <typename JsonT, typename OutT>
bool extract_value(const json &value, OutT &out) {
    if (const JsonT *typed = value.get_ptr<const JsonT *>()) {
        out = *typed;
        return true;
    }
    return false;
}

// Pull a JSON array whose elements are all of type ElemT into `out`. On any type
// mismatch `out` is cleared and false is returned.
template <typename ElemT, typename OutT>
bool extract_array(const json &value, std::vector<OutT> &out) {
    const json::array_t *array = value.get_ptr<const json::array_t *>();
    if (array == nullptr) {
        return false;
    }

    out.clear();
    for (const json &element : *array) {
        const ElemT *typed = element.get_ptr<const ElemT *>();
        if (typed == nullptr) {
            out.clear();
            return false;
        }
        out.emplace_back(*typed);
    }

    return true;
}

// Manifest top-level field names.
const std::string game_mod_id_key = "game_id";
const std::string mod_id_key = "id";
const std::string display_name_key = "display_name";
const std::string description_key = "description";
const std::string short_description_key = "short_description";
const std::string version_key = "version";
const std::string authors_key = "authors";
const std::string minimum_recomp_version_key = "minimum_recomp_version";
const std::string enabled_by_default_key = "enabled_by_default";
const std::string dependencies_key = "dependencies";
const std::string optional_dependencies_key = "optional_dependencies";
const std::string native_libraries_key = "native_libraries";
const std::string config_schema_key = "config_schema";

// Look up `key` in `data` and read it into `out` as JSON type JsonT. A missing
// key is an error only when `required`; otherwise `out` receives `fallback`.
template <typename JsonT, typename OutT>
ModOpenError read_field(OutT &out, const json &data, const std::string &key, bool required, std::string &error_param, OutT fallback = {}) {
    auto found = data.find(key);
    if (found == data.end()) {
        if (required) {
            error_param = key;
            return ModOpenError::MissingManifestField;
        }
        out = std::move(fallback);
        return ModOpenError::Good;
    }

    const JsonT *typed = found->get_ptr<const JsonT *>();
    if (typed == nullptr) {
        error_param = key;
        return ModOpenError::IncorrectManifestFieldType;
    }

    out = *typed;
    return ModOpenError::Good;
}

// Read a required string field and parse it into a Version.
ModOpenError read_version_field(recomp::Version &out, const json &data, const std::string &key, std::string &error_param, ModOpenError parse_failure_error) {
    std::string raw{};
    ModOpenError field_error = read_field<json::string_t>(raw, data, key, true, error_param);
    if (field_error != ModOpenError::Good) {
        return field_error;
    }

    if (!recomp::Version::from_string(raw, out)) {
        error_param = raw;
        return parse_failure_error;
    }

    return ModOpenError::Good;
}

// Read an array field whose elements are all of JSON type JsonT into `out`. A
// missing key is an error only when `required`.
template <typename JsonT, typename OutT>
ModOpenError read_array_field(std::vector<OutT> &out, const json &data, const std::string &key, bool required, std::string &error_param) {
    auto found = data.find(key);
    if (found == data.end()) {
        if (required) {
            error_param = key;
            return ModOpenError::MissingManifestField;
        }
        return ModOpenError::Good;
    }

    const json::array_t *array = found->get_ptr<const json::array_t *>();
    if (array == nullptr) {
        error_param = key;
        return ModOpenError::IncorrectManifestFieldType;
    }

    out.clear();
    for (const json &element : *array) {
        const JsonT *typed = element.get_ptr<const JsonT *>();
        if (typed == nullptr) {
            out.clear();
            error_param = key;
            return ModOpenError::IncorrectManifestFieldType;
        }
        out.emplace_back(*typed);
    }

    return ModOpenError::Good;
}

// Parse a single dependency string of the form "id" or "id:version".
bool parse_dependency(const std::string &text, recomp::mods::Dependency &out) {
    recomp::mods::Dependency dep;
    dep.version.major = 0;
    dep.version.minor = 0;
    dep.version.patch = 0;

    size_t separator = text.find(':');
    if (separator == std::string::npos) {
        // No version qualifier: the entire string is the id, version stays zero.
        dep.mod_id = text;
        if (!N64Recomp::validate_mod_id(std::string_view{ text })) {
            return false;
        }
    }
    else {
        // "id:version": both halves must be well-formed.
        dep.mod_id = text.substr(0, separator);
        if (!N64Recomp::validate_mod_id(dep.mod_id)) {
            return false;
        }
        if (!recomp::Version::from_string(text.substr(separator + 1), dep.version)) {
            return false;
        }
    }

    out = std::move(dep);
    return true;
}

// Config schema field names.
constexpr std::string_view config_schema_id_key = "id";
constexpr std::string_view config_schema_name_key = "name";
constexpr std::string_view config_schema_description_key = "description";
constexpr std::string_view config_schema_type_key = "type";
constexpr std::string_view config_schema_min_key = "min";
constexpr std::string_view config_schema_max_key = "max";
constexpr std::string_view config_schema_step_key = "step";
constexpr std::string_view config_schema_precision_key = "precision";
constexpr std::string_view config_schema_percent_key = "percent";
constexpr std::string_view config_schema_options_key = "options";
constexpr std::string_view config_schema_default_key = "default";

const std::unordered_map<std::string, recomp::mods::ConfigOptionType> config_option_type_names{
    { "Enum",   recomp::mods::ConfigOptionType::Enum },
    { "Number", recomp::mods::ConfigOptionType::Number },
    { "String", recomp::mods::ConfigOptionType::String },
};

// Read a required string field out of a config schema object, mapping a missing
// key and a type mismatch to the config-schema-specific error codes.
ModOpenError read_required_schema_string(const json &object, std::string_view key, std::string &out, std::string &error_param) {
    auto found = object.find(key);
    if (found == object.end()) {
        error_param = key;
        return ModOpenError::MissingConfigSchemaField;
    }
    if (!extract_value<json::string_t>(*found, out)) {
        error_param = key;
        return ModOpenError::IncorrectConfigSchemaType;
    }
    return ModOpenError::Good;
}

// Parse the Enum-specific portion of a config schema option.
ModOpenError parse_enum_option(const json &option_json, recomp::mods::ConfigOptionEnum &out, std::string &error_param) {
    auto options = option_json.find(config_schema_options_key);
    if (options != option_json.end()) {
        if (!extract_array<std::string>(*options, out.options)) {
            error_param = config_schema_options_key;
            return ModOpenError::IncorrectConfigSchemaType;
        }
    }

    auto default_value = option_json.find(config_schema_default_key);
    if (default_value != option_json.end()) {
        std::string default_string;
        if (!extract_value<json::string_t>(*default_value, default_string)) {
            error_param = config_schema_default_key;
            return ModOpenError::IncorrectConfigSchemaType;
        }

        // The default must name one of the listed options; store its index.
        auto match = std::find(out.options.begin(), out.options.end(), default_string);
        if (match == out.options.end()) {
            error_param = config_schema_default_key;
            return ModOpenError::InvalidConfigSchemaDefault;
        }
        out.default_value = uint32_t(match - out.options.begin());
    }

    return ModOpenError::Good;
}

// Parse the Number-specific portion of a config schema option.
ModOpenError parse_number_option(const json &option_json, recomp::mods::ConfigOptionNumber &out, std::string &error_param) {
    // Helper for the numeric fields that share the same is_number() check.
    auto read_number = [&](std::string_view key, double &dest) -> ModOpenError {
        auto found = option_json.find(key);
        if (found != option_json.end()) {
            if (!found->is_number()) {
                error_param = key;
                return ModOpenError::IncorrectConfigSchemaType;
            }
            dest = found->template get<double>();
        }
        return ModOpenError::Good;
    };

    ModOpenError err;
    if ((err = read_number(config_schema_min_key, out.min)) != ModOpenError::Good) return err;
    if ((err = read_number(config_schema_max_key, out.max)) != ModOpenError::Good) return err;
    if ((err = read_number(config_schema_step_key, out.step)) != ModOpenError::Good) return err;

    auto precision = option_json.find(config_schema_precision_key);
    if (precision != option_json.end()) {
        int64_t precision_value;
        if (!extract_value<int64_t>(*precision, precision_value)) {
            error_param = config_schema_precision_key;
            return ModOpenError::IncorrectConfigSchemaType;
        }
        out.precision = int(precision_value);
    }

    auto percent = option_json.find(config_schema_percent_key);
    if (percent != option_json.end()) {
        if (!extract_value<bool>(*percent, out.percent)) {
            error_param = config_schema_percent_key;
            return ModOpenError::IncorrectConfigSchemaType;
        }
    }

    double default_value;
    if ((err = read_number(config_schema_default_key, default_value)) != ModOpenError::Good) return err;
    if (option_json.find(config_schema_default_key) != option_json.end()) {
        out.default_value = default_value;
    }

    return ModOpenError::Good;
}

// Parse the String-specific portion of a config schema option.
ModOpenError parse_string_option(const json &option_json, recomp::mods::ConfigOptionString &out, std::string &error_param) {
    auto default_value = option_json.find(config_schema_default_key);
    if (default_value != option_json.end()) {
        if (!extract_value<json::string_t>(*default_value, out.default_value)) {
            error_param = config_schema_default_key;
            return ModOpenError::IncorrectConfigSchemaType;
        }
    }
    return ModOpenError::Good;
}

// Parse one entry of the config schema's "options" array and append it to the
// manifest's config schema.
ModOpenError parse_config_schema_option(const json &option_json, recomp::mods::ModManifest &manifest, std::string &error_param) {
    recomp::mods::ConfigOption option;

    ModOpenError err = read_required_schema_string(option_json, config_schema_id_key, option.id, error_param);
    if (err != ModOpenError::Good) {
        return err;
    }

    err = read_required_schema_string(option_json, config_schema_name_key, option.name, error_param);
    if (err != ModOpenError::Good) {
        return err;
    }

    // Description is optional but must be a string when present.
    auto description = option_json.find(config_schema_description_key);
    if (description != option_json.end()) {
        if (!extract_value<json::string_t>(*description, option.description)) {
            error_param = config_schema_description_key;
            return ModOpenError::IncorrectConfigSchemaType;
        }
    }

    // Type is required, and selects which variant fields are parsed below.
    auto type = option_json.find(config_schema_type_key);
    if (type == option_json.end()) {
        error_param = config_schema_type_key;
        return ModOpenError::MissingConfigSchemaField;
    }
    std::string type_string;
    if (!extract_value<json::string_t>(*type, type_string)) {
        error_param = config_schema_type_key;
        return ModOpenError::IncorrectConfigSchemaType;
    }
    auto type_it = config_option_type_names.find(type_string);
    if (type_it == config_option_type_names.end()) {
        error_param = config_schema_type_key;
        return ModOpenError::IncorrectConfigSchemaType;
    }
    option.type = type_it->second;

    switch (option.type) {
    case recomp::mods::ConfigOptionType::Enum: {
        recomp::mods::ConfigOptionEnum variant;
        err = parse_enum_option(option_json, variant, error_param);
        if (err != ModOpenError::Good) {
            return err;
        }
        option.variant = variant;
        break;
    }
    case recomp::mods::ConfigOptionType::Number: {
        recomp::mods::ConfigOptionNumber variant;
        err = parse_number_option(option_json, variant, error_param);
        if (err != ModOpenError::Good) {
            return err;
        }
        option.variant = variant;
        break;
    }
    case recomp::mods::ConfigOptionType::String: {
        recomp::mods::ConfigOptionString variant;
        err = parse_string_option(option_json, variant, error_param);
        if (err != ModOpenError::Good) {
            return err;
        }
        option.variant = variant;
        break;
    }
    default:
        break;
    }

    manifest.config_schema.options_by_id.emplace(option.id, manifest.config_schema.options.size());
    manifest.config_schema.options.emplace_back(std::move(option));

    return ModOpenError::Good;
}

// Append every dependency string in `key`'s array to the manifest, marking each
// with the given optional flag.
ModOpenError read_dependency_list(recomp::mods::ModManifest &manifest, const json &manifest_json, const std::string &key, bool optional, std::string &error_param) {
    std::vector<std::string> raw_dependencies{};
    ModOpenError err = read_array_field<json::string_t>(raw_dependencies, manifest_json, key, false, error_param);
    if (err != ModOpenError::Good) {
        return err;
    }

    for (const std::string &entry : raw_dependencies) {
        recomp::mods::Dependency dependency;
        if (!parse_dependency(entry, dependency)) {
            error_param = entry;
            return ModOpenError::InvalidDependencyString;
        }
        dependency.optional = optional;

        size_t index = manifest.dependencies.size();
        manifest.dependencies_by_id.emplace(dependency.mod_id, index);
        manifest.dependencies.emplace_back(std::move(dependency));
    }

    return ModOpenError::Good;
}

// Read the stored configuration values for a mod, validated against its schema.
bool load_mod_config_storage(const std::filesystem::path &path, const std::string &expected_mod_id, recomp::mods::ConfigStorage &config_storage, const recomp::mods::ConfigSchema &config_schema) {
    json config_json;
    if (!decode_json_file(path, config_json)) {
        return false;
    }

    // The stored config must declare the mod id it belongs to and it must match.
    auto mod_id = config_json.find("mod_id");
    if (mod_id == config_json.end()) {
        return false;
    }
    std::string stored_mod_id;
    if (!extract_value<json::string_t>(*mod_id, stored_mod_id)) {
        return false;
    }
    if (*mod_id != expected_mod_id) {
        return false;
    }

    auto storage_json = config_json.find("storage");
    if (storage_json == config_json.end() || !storage_json->is_object()) {
        return false;
    }

    // Walk the schema (not the stored object) so only recognized options load.
    std::string value_str;
    for (const recomp::mods::ConfigOption &option : config_schema.options) {
        auto stored = storage_json->find(option.id);
        if (stored == storage_json->end()) {
            continue;
        }

        switch (option.type) {
        case recomp::mods::ConfigOptionType::Enum:
            if (extract_value<json::string_t>(*stored, value_str)) {
                const auto &option_enum = std::get<recomp::mods::ConfigOptionEnum>(option.variant);
                auto match = std::find(option_enum.options.begin(), option_enum.options.end(), value_str);
                if (match != option_enum.options.end()) {
                    config_storage.value_map[option.id] = uint32_t(match - option_enum.options.begin());
                }
            }
            break;
        case recomp::mods::ConfigOptionType::Number:
            if (stored->is_number()) {
                config_storage.value_map[option.id] = stored->template get<double>();
            }
            break;
        case recomp::mods::ConfigOptionType::String:
            if (extract_value<json::string_t>(*stored, value_str)) {
                config_storage.value_map[option.id] = value_str;
            }
            break;
        default:
            assert(false && "Unknown option type.");
            break;
        }
    }

    return true;
}

} // namespace

recomp::mods::ZipModFileHandle::~ZipModFileHandle() {
    if (file_handle != nullptr) {
        fclose(file_handle);
        file_handle = nullptr;
    }

    if (archive) {
        mz_zip_reader_end(archive.get());
    }
    archive = {};
}

recomp::mods::ZipModFileHandle::ZipModFileHandle(const std::filesystem::path& mod_path, ModOpenError& error) {
#ifdef _WIN32
    if (_wfopen_s(&file_handle, mod_path.c_str(), L"rb") != 0) {
        error = ModOpenError::FileError;
        return;
    }
#else
    file_handle = fopen(mod_path.c_str(), "rb");
    if (!file_handle) {
        error = ModOpenError::FileError;
        return;
    }
#endif
    archive = std::make_unique<mz_zip_archive>();
    if (!mz_zip_reader_init_cfile(archive.get(), file_handle, 0, 0)) {
        error = ModOpenError::InvalidZip;
        return;
    }

    error = ModOpenError::Good;
}

recomp::mods::ZipModFileHandle::ZipModFileHandle(std::span<const uint8_t> mod_bytes, ModOpenError& error) {
    archive = std::make_unique<mz_zip_archive>();
    if (!mz_zip_reader_init_mem(archive.get(), mod_bytes.data(), mod_bytes.size(), 0)) {
        error = ModOpenError::InvalidZip;
        return;
    }

    error = ModOpenError::Good;
}

std::vector<char> recomp::mods::ZipModFileHandle::read_file(const std::string& filepath, bool& exists) const {
    exists = false;
    std::vector<char> ret{};

    mz_uint32 file_index;
    if (!mz_zip_reader_locate_file_v2(archive.get(), filepath.c_str(), nullptr, MZ_ZIP_FLAG_CASE_SENSITIVE, &file_index)) {
        return ret;
    }

    mz_zip_archive_file_stat stat;
    if (!mz_zip_reader_file_stat(archive.get(), file_index, &stat)) {
        return ret;
    }

    ret.resize(stat.m_uncomp_size);
    if (!mz_zip_reader_extract_to_mem(archive.get(), file_index, ret.data(), ret.size(), 0)) {
        return {};
    }

    exists = true;
    return ret;
}

bool recomp::mods::ZipModFileHandle::file_exists(const std::string& filepath) const {
    mz_uint32 file_index;
    return mz_zip_reader_locate_file_v2(archive.get(), filepath.c_str(), nullptr, MZ_ZIP_FLAG_CASE_SENSITIVE, &file_index) != 0;
}

recomp::mods::LooseModFileHandle::~LooseModFileHandle() {
    // Nothing to do here, members will be destroyed automatically.
}

recomp::mods::LooseModFileHandle::LooseModFileHandle(const std::filesystem::path& mod_path, ModOpenError& error) {
    root_path = mod_path;

    std::error_code ec;
    if (!std::filesystem::is_directory(root_path, ec)) {
        error = ModOpenError::NotAFileOrFolder;
    }

    if (ec) {
        error = ModOpenError::FileError;
    }

    error = ModOpenError::Good;
}

std::vector<char> recomp::mods::LooseModFileHandle::read_file(const std::string& filepath, bool& exists) const {
    exists = false;
    std::vector<char> ret{};
    std::filesystem::path full_path = root_path / filepath;

    std::error_code ec;
    if (!std::filesystem::is_regular_file(full_path, ec) || ec) {
        return ret;
    }

    std::ifstream file{ full_path, std::ios::binary };
    if (!file.good()) {
        return ret;
    }

    file.seekg(0, std::ios::end);
    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    ret.resize(file_size);
    file.read(ret.data(), ret.size());

    exists = true;
    return ret;
}

bool recomp::mods::LooseModFileHandle::file_exists(const std::string& filepath) const {
    std::filesystem::path full_path = root_path / filepath;

    std::error_code ec;
    if (!std::filesystem::is_regular_file(full_path, ec) || ec) {
        return false;
    }

    return true;
}

recomp::mods::ModOpenError recomp::mods::parse_manifest(ModManifest& ret, const std::vector<char>& manifest_data, std::string& error_param) {
    json manifest_json = json::parse(manifest_data.begin(), manifest_data.end(), nullptr, false);

    if (manifest_json.is_discarded()) {
        return ModOpenError::FailedToParseManifest;
    }

    if (!manifest_json.is_object()) {
        return ModOpenError::InvalidManifestSchema;
    }

    ModOpenError err;

    // The game id is stored as a single-element list on the manifest.
    std::string mod_game_id{};
    err = read_field<json::string_t>(mod_game_id, manifest_json, game_mod_id_key, true, error_param);
    if (err != ModOpenError::Good) {
        return err;
    }
    ret.mod_game_ids.emplace_back(std::move(mod_game_id));

    err = read_field<json::string_t>(ret.mod_id, manifest_json, mod_id_key, true, error_param);
    if (err != ModOpenError::Good) {
        return err;
    }

    err = read_field<json::string_t>(ret.display_name, manifest_json, display_name_key, true, error_param);
    if (err != ModOpenError::Good) {
        return err;
    }

    // Description and short description are optional.
    err = read_field<json::string_t>(ret.description, manifest_json, description_key, false, error_param);
    if (err != ModOpenError::Good) {
        return err;
    }

    err = read_field<json::string_t>(ret.short_description, manifest_json, short_description_key, false, error_param);
    if (err != ModOpenError::Good) {
        return err;
    }

    err = read_version_field(ret.version, manifest_json, version_key, error_param, ModOpenError::InvalidVersionString);
    if (err != ModOpenError::Good) {
        return err;
    }

    err = read_array_field<json::string_t>(ret.authors, manifest_json, authors_key, true, error_param);
    if (err != ModOpenError::Good) {
        return err;
    }

    err = read_version_field(ret.minimum_recomp_version, manifest_json, minimum_recomp_version_key, error_param, ModOpenError::InvalidMinimumRecompVersionString);
    if (err != ModOpenError::Good) {
        return err;
    }

    // Defaults to enabled when the field is absent.
    err = read_field<json::boolean_t>(ret.enabled_by_default, manifest_json, enabled_by_default_key, false, error_param, true);
    if (err != ModOpenError::Good) {
        return err;
    }

    // Required and optional dependencies share the same parsing, differing only
    // in the flag stamped onto each entry.
    err = read_dependency_list(ret, manifest_json, dependencies_key, false, error_param);
    if (err != ModOpenError::Good) {
        return err;
    }

    err = read_dependency_list(ret, manifest_json, optional_dependencies_key, true, error_param);
    if (err != ModOpenError::Good) {
        return err;
    }

    // Native libraries (optional): an object mapping a library name to its list
    // of exported symbols.
    auto native_libraries = manifest_json.find(native_libraries_key);
    if (native_libraries != manifest_json.end()) {
        if (!native_libraries->is_object()) {
            error_param = native_libraries_key;
            return ModOpenError::IncorrectManifestFieldType;
        }
        for (const auto& [lib_name, lib_exports] : native_libraries->items()) {
            NativeLibraryManifest& library = ret.native_libraries.emplace_back();
            library.name = lib_name;
            if (!extract_array<std::string>(lib_exports, library.exports)) {
                error_param = native_libraries_key;
                return ModOpenError::IncorrectManifestFieldType;
            }
        }
    }

    // Config schema (optional): an object holding an "options" array.
    auto config_schema = manifest_json.find(config_schema_key);
    if (config_schema != manifest_json.end()) {
        if (!config_schema->is_object()) {
            error_param = config_schema_key;
            return ModOpenError::IncorrectManifestFieldType;
        }

        auto options = config_schema->find(config_schema_options_key);
        if (options == config_schema->end()) {
            error_param = config_schema_options_key;
            return ModOpenError::MissingConfigSchemaField;
        }
        if (!options->is_array()) {
            error_param = config_schema_options_key;
            return ModOpenError::IncorrectManifestFieldType;
        }

        for (const json& option : *options) {
            err = parse_config_schema_option(option, ret, error_param);
            if (err != ModOpenError::Good) {
                return err;
            }
        }
    }

    return ModOpenError::Good;
}

recomp::mods::ModOpenError recomp::mods::ModContext::open_mod_from_manifest(ModManifest& manifest, std::string& error_param, const std::vector<ModContentTypeId>& supported_content_types, bool requires_manifest) {
    {
        bool exists;
        std::vector<char> manifest_data = manifest.file_handle->read_file("mod.json", exists);
        if (!exists) {
            // No manifest present. Reject if one is mandatory for this container.
            if (requires_manifest) {
                return ModOpenError::NoManifest;
            }

            // Otherwise synthesize a default manifest, preserving the file handle
            // and root path while resetting everything else.
            std::unique_ptr<ModFileHandle> file_handle = std::move(manifest.file_handle);
            std::filesystem::path root_path = std::move(manifest.mod_root_path);
            manifest = {};
            manifest.file_handle = std::move(file_handle);
            manifest.mod_root_path = std::move(root_path);

            for (const auto &[key, val] : mod_game_ids) {
                manifest.mod_game_ids.emplace_back(key);
            }

            manifest.mod_id = manifest.mod_root_path.stem().string();
            manifest.display_name = manifest.mod_id;
            manifest.description.clear();
            manifest.short_description.clear();
            manifest.authors = { "Unknown" };

            manifest.minimum_recomp_version.major = 0;
            manifest.minimum_recomp_version.minor = 0;
            manifest.minimum_recomp_version.patch = 0;
            manifest.version.major = 0;
            manifest.version.minor = 0;
            manifest.version.patch = 0;
            manifest.enabled_by_default = true;
        }
        else {
            ModOpenError parse_error = parse_manifest(manifest, manifest_data, error_param);
            if (parse_error != ModOpenError::Good) {
                return parse_error;
            }
        }
    }

    // Reject a mod whose id collides with one already opened.
    if (mod_ids.contains(manifest.mod_id)) {
        error_param = manifest.mod_id;
        return ModOpenError::DuplicateMod;
    }
    mod_ids.emplace(manifest.mod_id);

    // Every game id the mod targets must be one this context knows about.
    std::vector<size_t> game_indices;
    for (const auto &mod_game_id : manifest.mod_game_ids) {
        auto find_id_it = mod_game_ids.find(mod_game_id);
        if (find_id_it == mod_game_ids.end()) {
            error_param = mod_game_id;
            return ModOpenError::WrongGame;
        }
        game_indices.emplace_back(find_id_it->second);
    }

    // Determine which content types this mod actually carries.
    std::vector<ModContentTypeId> detected_content_types;
    auto scan_for_content_type = [&detected_content_types, &manifest](ModContentTypeId type_id, std::vector<ModContentType> &content_types) {
        const ModContentType &content_type = content_types[type_id.value];
        if (manifest.file_handle->file_exists(content_type.content_filename)) {
            detected_content_types.emplace_back(type_id);
        }
        };

    if (!supported_content_types.empty()) {
        // Restrict the scan to the caller-provided set of content types.
        for (ModContentTypeId content_type_id : supported_content_types) {
            scan_for_content_type(content_type_id, content_types);
        }
    }
    else {
        // No restriction: scan every registered content type.
        for (size_t content_type_index = 0; content_type_index < content_types.size(); content_type_index++) {
            scan_for_content_type(ModContentTypeId{ .value = content_type_index }, content_types);
        }
    }

    // Load any previously stored configuration values for this mod.
    ConfigStorage config_storage;
    std::filesystem::path config_path = mod_config_directory / (manifest.mod_id + ".json");
    load_mod_config_storage(config_path, manifest.mod_id, config_storage, manifest.config_schema);

    // Load the mod thumbnail if present, preferring the DDS over the PNG.
    static const std::string thumbnail_dds_name = "thumb.dds";
    static const std::string thumbnail_png_name = "thumb.png";
    bool exists = false;
    std::vector<char> thumbnail_data = manifest.file_handle->read_file(thumbnail_dds_name, exists);
    if (!exists) {
        thumbnail_data = manifest.file_handle->read_file(thumbnail_png_name, exists);
    }

    add_opened_mod(std::move(manifest), std::move(config_storage), std::move(game_indices), std::move(detected_content_types), std::move(thumbnail_data));

    return ModOpenError::Good;
}

recomp::mods::ModOpenError recomp::mods::ModContext::open_mod_from_path(const std::filesystem::path& mod_path, std::string& error_param, const std::vector<ModContentTypeId>& supported_content_types, bool requires_manifest) {
    ModManifest manifest{};
    manifest.mod_root_path = mod_path;

    std::error_code ec;
    error_param = "";

    if (!std::filesystem::exists(mod_path, ec) || ec) {
        return ModOpenError::DoesNotExist;
    }

    // TODO support symlinks?
    bool is_file = std::filesystem::is_regular_file(mod_path, ec);
    if (ec) {
        return ModOpenError::FileError;
    }

    bool is_directory = std::filesystem::is_directory(mod_path, ec);
    if (ec) {
        return ModOpenError::FileError;
    }

    // A regular file is treated as a zip archive; a directory as a loose mod.
    ModOpenError handle_error;
    if (is_file) {
        manifest.file_handle = std::make_unique<recomp::mods::ZipModFileHandle>(mod_path, handle_error);
    }
    else if (is_directory) {
        manifest.file_handle = std::make_unique<recomp::mods::LooseModFileHandle>(mod_path, handle_error);
    }
    else {
        return ModOpenError::NotAFileOrFolder;
    }

    if (handle_error != ModOpenError::Good) {
        return handle_error;
    }

    return open_mod_from_manifest(manifest, error_param, supported_content_types, requires_manifest);
}

recomp::mods::ModOpenError recomp::mods::ModContext::open_mod_from_memory(std::span<const uint8_t> mod_bytes, std::string &error_param, const std::vector<ModContentTypeId> &supported_content_types, bool requires_manifest) {
    ModManifest manifest{};
    ModOpenError handle_error;
    manifest.file_handle = std::make_unique<recomp::mods::ZipModFileHandle>(mod_bytes, handle_error);
    if (handle_error != ModOpenError::Good) {
        return handle_error;
    }

    return open_mod_from_manifest(manifest, error_param, supported_content_types, requires_manifest);
}

std::string recomp::mods::error_to_string(ModOpenError error) {
    switch (error) {
        case ModOpenError::Good:
            return "Good";
        case ModOpenError::DoesNotExist:
            return "Mod does not exist";
        case ModOpenError::NotAFileOrFolder:
            return "Mod is not a file or folder";
        case ModOpenError::FileError:
            return "Error reading mod file(s)";
        case ModOpenError::InvalidZip:
            return "Mod is an invalid zip file";
        case ModOpenError::NoManifest:
            return "Mod is missing a mod.json";
        case ModOpenError::FailedToParseManifest:
            return "Failed to parse mod's mod.json";
        case ModOpenError::InvalidManifestSchema:
            return "Mod's mod.json has an invalid schema";
        case ModOpenError::IncorrectManifestFieldType:
            return "Incorrect type for field in mod.json";
        case ModOpenError::MissingConfigSchemaField:
            return "Missing required field in config schema in mod.json";
        case ModOpenError::IncorrectConfigSchemaType:
            return "Incorrect type for field in config schema in mod.json";
        case ModOpenError::InvalidConfigSchemaDefault:
            return "Invalid default for option in config schema in mod.json";
        case ModOpenError::InvalidVersionString:
            return "Invalid version string in mod.json";
        case ModOpenError::InvalidMinimumRecompVersionString:
            return "Invalid minimum recomp version string in mod.json";
        case ModOpenError::InvalidDependencyString:
            return "Invalid dependency string in mod.json";
        case ModOpenError::MissingManifestField:
            return "Missing required field in mod.json";
        case ModOpenError::DuplicateMod:
            return "Duplicate mod found";
        case ModOpenError::WrongGame:
            return "Mod is for a different game";
    }
    return "Unknown mod opening error: " + std::to_string((int)error);
}

std::string recomp::mods::error_to_string(ModLoadError error) {
    switch (error) {
        case ModLoadError::Good:
            return "Good";
        case ModLoadError::InvalidGame:
            return "Invalid game";
        case ModLoadError::MinimumRecompVersionNotMet:
            return "Mod requires a newer version of this project";
        case ModLoadError::MissingDependency:
            return "Missing dependency";
        case ModLoadError::WrongDependencyVersion:
            return "Wrong dependency version";
        case ModLoadError::FailedToLoadCode:
            return "Failed to load mod code";
    }
    return "Unknown mod loading error " + std::to_string((int)error);
}

std::string recomp::mods::error_to_string(CodeModLoadError error) {
    switch (error) {
        case CodeModLoadError::Good:
            return "Good";
        case CodeModLoadError::InternalError:
            return "Code mod loading internal error";
        case CodeModLoadError::HasSymsButNoBinary:
            return "Mod has a symbol file but no binary file";
        case CodeModLoadError::HasBinaryButNoSyms:
            return "Mod has a binary file but no symbol file";
        case CodeModLoadError::FailedToParseSyms:
            return "Failed to parse mod symbol file";
        case CodeModLoadError::MissingDependencyInManifest:
            return "Dependency is present in mod symbols but not in the manifest";
        case CodeModLoadError::FailedToLoadNativeCode:
            return "Failed to load offline mod library";
        case CodeModLoadError::FailedToLoadNativeLibrary:
            return "Failed to load mod library";
        case CodeModLoadError::FailedToFindNativeExport:
            return "Failed to find native export";
        case CodeModLoadError::FailedToRecompile:
            return "Failed to recompile mod";
        case CodeModLoadError::InvalidReferenceSymbol:
            return "Reference symbol does not exist";
        case CodeModLoadError::InvalidImport:
            return "Imported function not found";
        case CodeModLoadError::InvalidCallbackEvent:
            return "Event for callback not found";
        case CodeModLoadError::InvalidFunctionReplacement:
            return "Function to be replaced does not exist";
        case CodeModLoadError::HooksUnavailable:
            // This error will occur if the ROM's GameEntry is set as having compressed code, but no
            // ROM decompression routine has been provided.
            return "Function hooks are currently unavailable in this project";
        case CodeModLoadError::InvalidHook:
            return "Function to be hooked does not exist";
        case CodeModLoadError::CannotBeHooked:
            return "Function is not hookable";
        case CodeModLoadError::FailedToFindReplacement:
            return "Failed to find replacement function";
        case CodeModLoadError::BaseRecompConflict:
            return "Attempted to replace a function that's been patched by the base recomp";
        case CodeModLoadError::ModConflict:
            return "Conflicts with other mod";
        case CodeModLoadError::DuplicateExport:
            return "Duplicate exports in mod";
        case CodeModLoadError::OfflineModHooked:
            return "Offline recompiled mod has a function hooked by another mod";
        case CodeModLoadError::NoSpecifiedApiVersion:
            return "Mod DLL does not specify an API version";
        case CodeModLoadError::UnsupportedApiVersion:
            return "Mod DLL has an unsupported API version";
    }
}
