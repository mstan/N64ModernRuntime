#include <span>
#include <fstream>
#include <sstream>
#include <functional>

#include "librecomp/files.hpp"
#include "librecomp/mods.hpp"
#include "librecomp/overlays.hpp"
#include "librecomp/game.hpp"
#include "recompiler/context.h"
#include "recompiler/live_recompiler.h"

static bool read_json(std::ifstream input_file, nlohmann::json &json_out) {
    // Nothing to do if the stream isn't usable.
    if (!input_file.good()) {
        return false;
    }

    // Deserialize from the stream, treating a malformed document as a soft failure.
    try {
        input_file >> json_out;
    }
    catch (nlohmann::json::parse_error &) {
        return false;
    }

    return true;
}

static bool read_json_with_backups(const std::filesystem::path &path, nlohmann::json &json_out) {
    // First attempt: the primary file at the given path.
    if (read_json(std::ifstream{ path }, json_out)) {
        return true;
    }

    // Fallback attempt: the associated backup copy.
    if (read_json(recomp::open_input_backup_file(path), json_out)) {
        return true;
    }

    // Neither the primary nor the backup could be parsed.
    return false;
}


template <typename T1, typename T2>
bool get_to_vec(const nlohmann::json& val, std::vector<T2>& out) {
    // The value must be a JSON array for this to succeed.
    const nlohmann::json::array_t* array_ptr = val.get_ptr<const nlohmann::json::array_t*>();
    if (array_ptr == nullptr) {
        return false;
    }

    out.clear();

    // Each element must be convertible to the requested element type; on any
    // mismatch the partially-built output is discarded.
    for (const nlohmann::json& element : *array_ptr) {
        const T1* element_ptr = element.get_ptr<const T1*>();
        if (element_ptr == nullptr) {
            out.clear();
            return false;
        }

        out.emplace_back(*element_ptr);
    }

    return true;
}

// Architecture detection.

// MSVC x86_64
#if defined (_M_AMD64) && (_M_AMD64 == 100) && !defined (_M_ARM64EC)
#   define IS_X86_64
// GCC/Clang x86_64
#elif defined(__x86_64__)
#   define IS_X86_64
// MSVC/GCC/Clang ARM64
#elif defined(__ARM_ARCH_ISA_A64)
#   define IS_ARM64
#else
#   error "Unsupported architecture!"
#endif


#if defined(_WIN32)
#define PATHFMT "%ls"
#else
#define PATHFMT "%s"
#endif

template<class... Ts>
struct overloaded : Ts... { using Ts::operator()...; };
template<class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

#if defined(_WIN32)
#   define WIN32_LEAN_AND_MEAN
#   include "Windows.h"

class recomp::mods::DynamicLibrary {
public:
    static constexpr std::string_view PlatformExtension = ".dll";
    DynamicLibrary() = default;
    DynamicLibrary(const std::filesystem::path& path) {
        native_handle = LoadLibraryExW(std::filesystem::absolute(path).c_str(), nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);

        // When the module loads, query the version it exports; absence is encoded as all-ones.
        if (good()) {
            uint32_t* version_symbol;
            api_version = get_dll_symbol(version_symbol, "recomp_api_version") ? *version_symbol : (uint32_t)-1;
        }
    }
    ~DynamicLibrary() {
        unload();
    }
    DynamicLibrary(const DynamicLibrary&) = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;
    DynamicLibrary(DynamicLibrary&&) = delete;
    DynamicLibrary& operator=(DynamicLibrary&&) = delete;

    void unload() {
        if (native_handle != nullptr) {
            FreeLibrary(native_handle);
        }
        native_handle = nullptr;
    }

    bool good() const {
        return native_handle != nullptr;
    }

    template <typename T>
    bool get_dll_symbol(T& out, const char* name) const {
        out = (T)(void*)GetProcAddress(native_handle, name);
        if (out == nullptr) {
            return false;
        }
        return true;
    };

    uint32_t get_api_version() {
        return api_version;
    }
private:
    HMODULE native_handle;
    uint32_t api_version;
};

void unprotect(void* target_func, uint64_t* old_flags) {
    // Make the patch region writable, handing back the previous protection.
    DWORD previous_protection = 0;
    BOOL ok = VirtualProtect(target_func, 16, PAGE_READWRITE, &previous_protection);
    *old_flags = previous_protection;
    (void)ok;
}

void protect(void* target_func, uint64_t old_flags) {
    // Restore the protection that unprotect() previously captured.
    DWORD discarded_protection = 0;
    BOOL ok = VirtualProtect(target_func, 16, static_cast<DWORD>(old_flags), &discarded_protection);
    (void)ok;
}
#else
#  include <unistd.h>
#  include <dlfcn.h>
#  include <sys/mman.h>

class recomp::mods::DynamicLibrary {
public:
    #if defined(__APPLE__)
    static constexpr std::string_view PlatformExtension = ".dylib";
    #else
    static constexpr std::string_view PlatformExtension = ".so";
    #endif
    DynamicLibrary() = default;
    DynamicLibrary(const std::filesystem::path& path) {
        native_handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);

        // When the module loads, query the version it exports; absence is encoded as all-ones.
        if (good()) {
            uint32_t* version_symbol;
            api_version = get_dll_symbol(version_symbol, "recomp_api_version") ? *version_symbol : (uint32_t)-1;
        }
    }
    ~DynamicLibrary() {
        unload();
    }
    DynamicLibrary(const DynamicLibrary&) = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;
    DynamicLibrary(DynamicLibrary&&) = delete;
    DynamicLibrary& operator=(DynamicLibrary&&) = delete;

    void unload() {
        if (native_handle != nullptr) {
            dlclose(native_handle);
        }
        native_handle = nullptr;
    }

    bool good() const {
        return native_handle != nullptr;
    }

    template <typename T>
    bool get_dll_symbol(T& out, const char* name) const {
        out = (T)dlsym(native_handle, name);
        if (out == nullptr) {
            return false;
        }
        return true;
    };

    uint32_t get_api_version() {
        return api_version;
    }
private:
    void* native_handle;
    uint32_t api_version;
};

void unprotect(void* target_func, uint64_t* old_flags) {
    // mprotect operates on whole pages, so round the target down to its page base.
    int page_size = getpagesize();
    uintptr_t page_base = ((uintptr_t)target_func / page_size) * page_size;

    int rc = mprotect((void*)page_base, page_size, PROT_READ | PROT_WRITE);
    *old_flags = 0;
    (void)rc;
}

void protect(void* target_func, uint64_t old_flags) {
    // mprotect operates on whole pages, so round the target down to its page base.
    int page_size = getpagesize();
    uintptr_t page_base = ((uintptr_t)target_func / page_size) * page_size;

    int rc = mprotect((void*)page_base, page_size, PROT_READ | PROT_EXEC);
    (void)rc;
}
#endif

namespace modpaths {
    constexpr std::string_view default_mod_extension = "nrm";
    constexpr std::string_view binary_path = "mod_binary.bin";
    constexpr std::string_view binary_syms_path = "mod_syms.bin";
};

recomp::mods::CodeModLoadError recomp::mods::validate_api_version(uint32_t api_version, std::string& error_param) {
    switch (api_version) {
        case 1:
            return CodeModLoadError::Good;
        case (uint32_t)-1:
            return CodeModLoadError::NoSpecifiedApiVersion;
        default:
            error_param = std::to_string(api_version);
            return CodeModLoadError::UnsupportedApiVersion;
    }
}

recomp::mods::ModHandle::ModHandle(const ModContext& context, ModManifest&& manifest, ConfigStorage&& config_storage, std::vector<size_t>&& game_indices, std::vector<ModContentTypeId>&& content_types, std::vector<char>&& thumbnail) :
    manifest(std::move(manifest)),
    config_storage(std::move(config_storage)),
    code_handle(),
    recompiler_context{std::make_unique<N64Recomp::Context>()},
    content_types{std::move(content_types)},
    thumbnail{ std::move(thumbnail) },
    game_indices{std::move(game_indices)}
{
    // The mod can be toggled at runtime only if none of its content types forbid it.
    runtime_toggleable = true;
    for (ModContentTypeId type : this->content_types) {
        if (!context.is_content_runtime_toggleable(type)) {
            runtime_toggleable = false;
            break;
        }
    }
}

recomp::mods::ModHandle::ModHandle(ModHandle&& rhs) = default;
recomp::mods::ModHandle& recomp::mods::ModHandle::operator=(ModHandle&& rhs) = default;
recomp::mods::ModHandle::~ModHandle() = default;

size_t recomp::mods::ModHandle::num_exports() const {
    return recompiler_context->exported_funcs.size();
}

size_t recomp::mods::ModHandle::num_events() const {
    return recompiler_context->event_symbols.size();
}

void recomp::mods::ModHandle::populate_exports() {
    // Index every exported function by its name for quick export lookups.
    for (size_t func_index : recompiler_context->exported_funcs) {
        const auto& exported = recompiler_context->functions[func_index];
        exports_by_name.emplace(exported.name, func_index);
    }
}

recomp::mods::CodeModLoadError recomp::mods::ModHandle::load_native_library(const recomp::mods::NativeLibraryManifest& lib_manifest, std::string& error_param) {
    // Build the platform-specific filename and resolve it relative to the mod's directory.
    std::string library_filename = lib_manifest.name + std::string{DynamicLibrary::PlatformExtension};
    std::filesystem::path library_path = manifest.mod_root_path.parent_path() / library_filename;

    std::unique_ptr<DynamicLibrary>& library = native_libraries.emplace_back(std::make_unique<DynamicLibrary>(library_path));

    // The library has to have actually loaded.
    if (!library->good()) {
        error_param = library_filename;
        return CodeModLoadError::FailedToLoadNativeLibrary;
    }

    // The library's advertised API version has to be one we support.
    std::string version_error_detail;
    CodeModLoadError version_error = validate_api_version(library->get_api_version(), version_error_detail);

    if (version_error != CodeModLoadError::Good) {
        // Append the extra detail to the filename only when there is some.
        error_param = version_error_detail.empty()
            ? library_filename
            : library_filename + ":" + version_error_detail;
        return version_error;
    }

    // Resolve every requested export, rejecting duplicates and missing symbols.
    native_library_exports.clear();
    for (const std::string& export_name : lib_manifest.exports) {
        if (native_library_exports.contains(export_name)) {
            error_param = export_name;
            return CodeModLoadError::DuplicateExport;
        }

        recomp_func_t* resolved_func;
        if (!library->get_dll_symbol(resolved_func, export_name.c_str())) {
            error_param = lib_manifest.name + ":" + export_name;
            return CodeModLoadError::FailedToFindNativeExport;
        }

        native_library_exports.emplace(export_name, resolved_func);
    }

    return CodeModLoadError::Good;
}

bool recomp::mods::ModHandle::get_export_function(const std::string& export_name, GenericFunction& out) const {
    // Prefer a recompiled code export if one matches the name.
    auto code_find_it = exports_by_name.find(export_name);
    if (code_find_it != exports_by_name.end()) {
        out = code_handle->get_function_handle(code_find_it->second);
        return true;
    }

    // Otherwise fall back to an export provided by a native library.
    auto native_find_it = native_library_exports.find(export_name);
    if (native_find_it != native_library_exports.end()) {
        out = native_find_it->second;
        return true;
    }

    // No export by that name in either table.
    return false;
}

void recomp::mods::ModHandle::populate_events() {
    // Map each event symbol's name to its local index.
    size_t event_count = recompiler_context->event_symbols.size();
    for (size_t event_index = 0; event_index < event_count; event_index++) {
        const N64Recomp::EventSymbol& event = recompiler_context->event_symbols[event_index];
        events_by_name.emplace(event.base.name, event_index);
    }
}

bool recomp::mods::ModHandle::get_global_event_index(const std::string& event_name, size_t& event_index_out) const {
    auto find_it = events_by_name.find(event_name);
    if (find_it == events_by_name.end()) {
        return false;
    }

    event_index_out = code_handle->get_base_event_index() + find_it->second;
    return true;
}

recomp::mods::DynamicLibraryCodeHandle::DynamicLibraryCodeHandle(const std::filesystem::path& dll_path, const N64Recomp::Context& context, const ModCodeHandleInputs& inputs) {
    is_good = true;
    // Open the shared library.
    dynamic_lib = std::make_unique<DynamicLibrary>(dll_path);
    if (!dynamic_lib->good()) {
        is_good = false;
        return;
    }

    // Resolve a pointer for each context function. Named functions are looked up
    // directly; anonymous ones use the "mod_func_<index>" fallback naming.
    functions.resize(context.functions.size());
    for (size_t func_index = 0; func_index < functions.size(); func_index++) {
        const std::string& context_name = context.functions[func_index].name;
        if (!context_name.empty()) {
            is_good &= dynamic_lib->get_dll_symbol(functions[func_index], context_name.c_str());
        }
        else {
            std::string fallback_name = "mod_func_" + std::to_string(func_index);
            is_good &= dynamic_lib->get_dll_symbol(functions[func_index], fallback_name.c_str());
        }
        if (!is_good) {
            return;
        }
    }

    // Resolve the fixed set of well-known exported symbols.
    is_good = true;
    is_good &= dynamic_lib->get_dll_symbol(imported_funcs, "imported_funcs");
    is_good &= dynamic_lib->get_dll_symbol(reference_symbol_funcs, "reference_symbol_funcs");
    is_good &= dynamic_lib->get_dll_symbol(base_event_index, "base_event_index");
    is_good &= dynamic_lib->get_dll_symbol(recomp_trigger_event, "recomp_trigger_event");
    is_good &= dynamic_lib->get_dll_symbol(get_function, "get_function");
    is_good &= dynamic_lib->get_dll_symbol(cop0_status_write, "cop0_status_write");
    is_good &= dynamic_lib->get_dll_symbol(cop0_status_read, "cop0_status_read");
    is_good &= dynamic_lib->get_dll_symbol(switch_error, "switch_error");
    is_good &= dynamic_lib->get_dll_symbol(do_break, "do_break");
    is_good &= dynamic_lib->get_dll_symbol(reference_section_addresses, "reference_section_addresses");
    is_good &= dynamic_lib->get_dll_symbol(section_addresses, "section_addresses");

    if (is_good) {
        *base_event_index = inputs.base_event_index;
        *recomp_trigger_event = inputs.recomp_trigger_event;
        *get_function = inputs.get_function;
        *cop0_status_write = inputs.cop0_status_write;
        *cop0_status_read = inputs.cop0_status_read;
        *switch_error = inputs.switch_error;
        *do_break = inputs.do_break;
        *reference_section_addresses = inputs.reference_section_addresses;
    }
}

bool recomp::mods::DynamicLibraryCodeHandle::good()  {
    return dynamic_lib->good() && is_good;
}

uint32_t recomp::mods::DynamicLibraryCodeHandle::get_api_version() {
    return dynamic_lib->get_api_version();
}

void recomp::mods::DynamicLibraryCodeHandle::set_bad() {
    dynamic_lib.reset();
    is_good = false;
}

void recomp::mods::DynamicLibraryCodeHandle::set_imported_function(size_t import_index, GenericFunction func) {
    std::visit(overloaded {
        [this, import_index](recomp_func_t* native_func) {
            imported_funcs[import_index] = native_func;
        }
    }, func);
}

recomp::mods::CodeModLoadError recomp::mods::DynamicLibraryCodeHandle::populate_reference_symbols(const N64Recomp::Context& context, std::string& error_param) {
    // Walk every relocation across all sections, resolving the call-target
    // relocations that point at regular reference sections.
    size_t resolved_count = 0;
    for (const auto& section : context.sections) {
        for (const auto& reloc : section.relocs) {
            bool is_call_reference =
                reloc.type == N64Recomp::RelocType::R_MIPS_26 &&
                reloc.reference_symbol &&
                context.is_regular_reference_section(reloc.target_section);
            if (!is_call_reference) {
                continue;
            }

            recomp_func_t* resolved_func = recomp::overlays::get_func_by_section_index_function_offset(reloc.target_section, reloc.target_section_offset);
            if (resolved_func == nullptr) {
                std::stringstream error_param_stream{};
                error_param_stream << std::hex <<
                    "section: " << reloc.target_section <<
                    " func offset: 0x" << reloc.target_section_offset;
                error_param = error_param_stream.str();
                return CodeModLoadError::InvalidReferenceSymbol;
            }

            reference_symbol_funcs[resolved_count] = resolved_func;
            resolved_count++;
        }
    }
    return CodeModLoadError::Good;
}

recomp::mods::LiveRecompilerCodeHandle::LiveRecompilerCodeHandle(
    const N64Recomp::Context& context, const ModCodeHandleInputs& inputs,
    std::unordered_map<size_t, size_t>&& entry_func_hooks, std::unordered_map<size_t, size_t>&& return_func_hooks, std::vector<size_t>&& original_section_indices, bool regenerated)
{
    if (!regenerated) {
       section_addresses = std::make_unique<int32_t[]>(context.sections.size());
    }
    base_event_index = inputs.base_event_index;

    N64Recomp::LiveGeneratorInputs recompiler_inputs{
        .base_event_index = inputs.base_event_index,
        .cop0_status_write = inputs.cop0_status_write,
        .cop0_status_read = inputs.cop0_status_read,
        .switch_error = inputs.switch_error,
        .do_break = inputs.do_break,
        .get_function = inputs.get_function,
        .syscall_handler = nullptr, // TODO hook this up
        .pause_self = pause_self,
        .trigger_event = inputs.recomp_trigger_event,
        .reference_section_addresses = inputs.reference_section_addresses,
        // For regenerated code reuse the reference section addresses as the local section addresses so that jump tables resolve correctly.
        .local_section_addresses = regenerated ? inputs.reference_section_addresses : section_addresses.get(),
        .run_hook = run_hook,
        .entry_func_hooks = std::move(entry_func_hooks),
        .return_func_hooks = std::move(return_func_hooks),
        .original_section_indices = std::move(original_section_indices)
    };

    N64Recomp::LiveGenerator generator{ context.functions.size(), recompiler_inputs };
    std::vector<std::vector<uint32_t>> dummy_static_funcs{};

    // Live-recompile every function in turn, stopping at the first failure.
    bool recompile_failed = false;
    for (size_t func_index = 0; func_index < context.functions.size(); func_index++) {
        std::ostringstream dummy_ostream{};

        if (!N64Recomp::recompile_function_live(generator, context, func_index, dummy_ostream, dummy_static_funcs, true)) {
            recompile_failed = true;
            break;
        }
    }

    // Emit the generated code and record whether the whole process succeeded.
    recompiler_output = std::make_unique<N64Recomp::LiveGeneratorOutput>(generator.finish());
    is_good = !recompile_failed && recompiler_output->good;
}

void recomp::mods::LiveRecompilerCodeHandle::set_imported_function(size_t import_index, GenericFunction func) {
    std::visit(overloaded {
        [this, import_index](recomp_func_t* native_func) {
            recompiler_output->populate_import_symbol_jumps(import_index, native_func);
        }
    }, func);
}

recomp::mods::CodeModLoadError recomp::mods::LiveRecompilerCodeHandle::populate_reference_symbols(const N64Recomp::Context& context, std::string& error_param) {
    // Resolve a real function pointer for every reference-symbol jump emitted by
    // the live recompiler and patch it back into the generated output.
    size_t jump_count = recompiler_output->num_reference_symbol_jumps();
    for (size_t jump_index = 0; jump_index < jump_count; jump_index++) {
        N64Recomp::ReferenceJumpDetails jump_details = recompiler_output->get_reference_symbol_jump_details(jump_index);

        recomp_func_t* resolved_func = recomp::overlays::get_func_by_section_index_function_offset(jump_details.section, jump_details.section_offset);
        if (resolved_func == nullptr) {
            std::stringstream error_param_stream{};
            error_param_stream << std::hex <<
                "section: " << jump_details.section <<
                " func offset: 0x" << jump_details.section_offset;
            error_param = error_param_stream.str();
            return CodeModLoadError::InvalidReferenceSymbol;
        }

        recompiler_output->set_reference_symbol_jump(jump_index, resolved_func);
    }
    return CodeModLoadError::Good;
}

recomp::mods::GenericFunction recomp::mods::LiveRecompilerCodeHandle::get_function_handle(size_t func_index) {
    return GenericFunction{ recompiler_output->functions[func_index] };
}

void patch_func(recomp_func_t* target_func, recomp::mods::GenericFunction replacement_func) {
    uint8_t* target_func_u8 = reinterpret_cast<uint8_t*>(target_func);
    size_t offset = 0;

    auto write_bytes = [&](const void* bytes, size_t count) {
        memcpy(target_func_u8 + offset, bytes, count);
        offset += count;
    };

    // Make the target writable, overwrite its prologue with a trampoline to the
    // replacement, then restore the original protection.
    uint64_t old_flags;
    unprotect(target_func_u8, &old_flags);

#if defined(IS_X86_64)
    // Encodes: movabs rax, <addr> ; jmp rax
    static const uint8_t movabs_rax[] = {0x48, 0xB8};
    static const uint8_t jmp_rax[] = {0xFF, 0xE0};
    std::visit(overloaded {
        [&write_bytes](recomp_func_t* dest) {
           write_bytes(movabs_rax, sizeof(movabs_rax));
           write_bytes(&dest, sizeof(&dest));
           write_bytes(jmp_rax, sizeof(jmp_rax));
        }
    }, replacement_func);
#elif defined(IS_ARM64)
    static const uint8_t ldr_x2_8__br_x2[] = {0x42, 0x00, 0x00, 0x58, 0x40, 0x00, 0x1F, 0xD6};
    std::visit(overloaded {
        [&write_bytes](recomp_func_t* native_func) {
           write_bytes(ldr_x2_8__br_x2, sizeof(ldr_x2_8__br_x2));
           write_bytes(&native_func, sizeof(&native_func));
        }
    }, replacement_func);
#else
#   error "Unsupported architecture"
#endif

    protect(target_func_u8, old_flags);
}

void unpatch_func(void* target_func, const recomp::mods::PatchData& data) {
    uint64_t old_flags;
    unprotect(target_func, &old_flags);
    memcpy(target_func, data.replaced_bytes.data(), data.replaced_bytes.size());
    protect(target_func, old_flags);
}

void recomp::mods::ModContext::add_opened_mod(ModManifest&& manifest, ConfigStorage&& config_storage, std::vector<size_t>&& game_indices, std::vector<ModContentTypeId>&& detected_content_types, std::vector<char>&& thumbnail) {
    std::unique_lock lock(opened_mods_mutex);
    size_t mod_index = opened_mods.size();
    opened_mods_by_id.emplace(manifest.mod_id, mod_index);
    opened_mods_by_filename.emplace(manifest.mod_root_path.filename().native(), mod_index);
    opened_mods.emplace_back(*this, std::move(manifest), std::move(config_storage), std::move(game_indices), std::move(detected_content_types), std::move(thumbnail));
    opened_mods_order.emplace_back(mod_index);
}

recomp::mods::ModLoadError recomp::mods::ModContext::load_mod(recomp::mods::ModHandle& mod, std::string& error_param) {
    using namespace recomp::mods;
    mod.section_load_addresses.clear();

    // Refuse to load if the runtime is older than the mod requires.
    if (get_project_version() < mod.manifest.minimum_recomp_version) {
        error_param = mod.manifest.minimum_recomp_version.to_string();
        return ModLoadError::MinimumRecompVersionNotMet;
    }

    // Fire the enable hook for every content type this mod provides.
    for (ModContentTypeId type_id : mod.content_types) {
        content_enabled_callback* on_enabled = content_types[type_id.value].on_enabled;
        if (on_enabled != nullptr) {
            on_enabled(*this, mod);
        }
    }

    return ModLoadError::Good;
}

void recomp::mods::ModContext::register_game(const std::string& mod_game_id) {
    mod_game_ids.emplace(mod_game_id, mod_game_ids.size());
}

void recomp::mods::ModContext::register_embedded_mod(const std::string &mod_id, std::span<const uint8_t> mod_bytes) {
    embedded_mod_bytes.emplace(mod_id, mod_bytes);
}

void recomp::mods::ModContext::close_mods() {
    std::unique_lock lock(opened_mods_mutex);
    opened_mods_by_id.clear();
    opened_mods_by_filename.clear();
    opened_mods.clear();
    opened_mods_order.clear();
    mod_order_lookup.clear();
    mod_ids.clear();
    enabled_mods.clear();
    auto_enabled_mods.clear();
}

bool save_mod_config_storage(const std::filesystem::path &path, const std::string &mod_id, const recomp::Version &mod_version, const recomp::mods::ConfigStorage &config_storage, const recomp::mods::ConfigSchema &config_schema) {
    using json = nlohmann::json;
    json config_json;
    config_json["mod_id"] = mod_id;
    config_json["mod_version"] = mod_version.to_string();
    config_json["recomp_version"] = recomp::get_project_version().to_string();

    json &storage_json = config_json["storage"];
    // Serialize each stored value, skipping any whose id is no longer in the schema.
    for (auto stored_value : config_storage.value_map) {
        const std::string &option_id = stored_value.first;
        auto option_lookup = config_schema.options_by_id.find(option_id);
        if (option_lookup == config_schema.options_by_id.end()) {
            continue;
        }

        const recomp::mods::ConfigOption &config_option = config_schema.options[option_lookup->second];
        switch (config_option.type) {
        case recomp::mods::ConfigOptionType::Enum:
            // Enums are persisted as the human-readable option string rather than the index.
            storage_json[option_id] = std::get<recomp::mods::ConfigOptionEnum>(config_option.variant).options[std::get<uint32_t>(stored_value.second)];
            break;
        case recomp::mods::ConfigOptionType::Number:
            storage_json[option_id] = std::get<double>(stored_value.second);
            break;
        case recomp::mods::ConfigOptionType::String:
            storage_json[option_id] = std::get<std::string>(stored_value.second);
            break;
        default:
            assert(false && "Unknown config type.");
            break;
        }
    }

    std::ofstream output_file = recomp::open_output_file_with_backup(path);
    if (!output_file.good()) {
        return false;
    }

    output_file << std::setw(4) << config_json;
    output_file.close();

    return recomp::finalize_output_file_with_backup(path);
}

bool parse_mods_config(const std::filesystem::path &path, std::unordered_set<std::string> &enabled_mods, std::vector<std::string> &mod_order) {
    using json = nlohmann::json;
    json config_json;
    if (!read_json_with_backups(path, config_json)) {
        return false;
    }

    // Pull the set of enabled mods, if the field is present and well-formed.
    auto enabled_mods_json = config_json.find("enabled_mods");
    if (enabled_mods_json != config_json.end()) {
        std::vector<std::string> parsed_enabled;
        if (get_to_vec<std::string>(*enabled_mods_json, parsed_enabled)) {
            for (const std::string &mod_id : parsed_enabled) {
                enabled_mods.emplace(mod_id);
            }
        }
    }

    // Pull the mod ordering directly into the caller's vector when present.
    auto mod_order_json = config_json.find("mod_order");
    if (mod_order_json != config_json.end()) {
        get_to_vec<std::string>(*mod_order_json, mod_order);
    }

    return true;
}

bool save_mods_config(const std::filesystem::path &path, const std::unordered_set<std::string> &enabled_mods, const std::vector<std::string> &mod_order) {
    nlohmann::json config_json;
    config_json["enabled_mods"] = enabled_mods;
    config_json["mod_order"] = mod_order;

    std::ofstream output_file = recomp::open_output_file_with_backup(path);
    if (!output_file.good()) {
        return false;
    }

    output_file << std::setw(4) << config_json;
    output_file.close();

    return recomp::finalize_output_file_with_backup(path);
}

void recomp::mods::ModContext::dirty_mod_configuration_thread_process() {
    using namespace std::chrono_literals;
    ModConfigQueueVariant variant;
    ModConfigQueueSaveMod save_mod;
    std::unordered_set<std::string> pending_mods;
    std::unordered_map<std::string, ConfigStorage> pending_mod_storage;
    std::unordered_map<std::string, ConfigSchema> pending_mod_schema;
    std::unordered_map<std::string, Version> pending_mod_version;
    std::unordered_set<std::string> config_enabled_mods;
    std::vector<std::string> config_mod_order;
    bool pending_config_save = false;
    std::filesystem::path config_path;
    bool active = true;
    auto handle_variant = [&](const ModConfigQueueVariant &variant) {
        if (std::get_if<ModConfigQueueEnd>(&variant) != nullptr) {
            active = false;
        }
        else if (std::get_if<ModConfigQueueSave>(&variant) != nullptr) {
            pending_config_save = true;
        }
        else if (const ModConfigQueueSaveMod* queue_save_mod = std::get_if<ModConfigQueueSaveMod>(&variant)) {
            pending_mods.emplace(queue_save_mod->mod_id);
        }
    };

    while (active) {
        // Block until there's at least one request to process.
        mod_configuration_thread_queue.wait_dequeue(variant);
        handle_variant(variant);


        // Keep draining the queue (with a short timeout) so that bursts of writes get batched together.
        while (active && mod_configuration_thread_queue.wait_dequeue_timed(variant, 1s)) {
            handle_variant(variant);
        }

        if (active && !pending_mods.empty()) {
            {
                // Snapshot each pending mod's storage/schema/version while holding the locks.
                std::unique_lock opened_mods_lock(opened_mods_mutex);
                for (const std::string &id : pending_mods) {
                    auto mod_it = opened_mods_by_id.find(id);
                    if (mod_it != opened_mods_by_id.end()) {
                        const ModHandle &mod = opened_mods[mod_it->second];
                        std::unique_lock config_storage_lock(mod_config_storage_mutex);
                        pending_mod_storage[id] = mod.config_storage;
                        pending_mod_schema[id] = mod.manifest.config_schema;
                        pending_mod_version[id] = mod.manifest.version;
                    }
                }
            }

            // Write the snapshots out without holding any locks.
            for (const std::string &id : pending_mods) {
                config_path = mod_config_directory / std::string(id + ".json");
                save_mod_config_storage(config_path, id, pending_mod_version[id], pending_mod_storage[id], pending_mod_schema[id]);
            }

            pending_mods.clear();
        }

        if (active && pending_config_save) {
            {
                // Copy out the enabled set and current order under the lock.
                std::unique_lock order_lock(opened_mods_mutex);
                config_enabled_mods = enabled_mods;
                config_mod_order.clear();
                for (size_t mod_index : opened_mods_order) {
                    config_mod_order.emplace_back(opened_mods[mod_index].manifest.mod_id);
                }
            }

            save_mods_config(mods_config_path, config_enabled_mods, config_mod_order);
            pending_config_save = false;
        }
    }
}

std::vector<recomp::mods::ModOpenErrorDetails> recomp::mods::ModContext::scan_mod_folder(const std::filesystem::path& mod_folder) {
    std::vector<recomp::mods::ModOpenErrorDetails> ret{};
    std::error_code ec;
    close_mods();

    static const std::vector<ModContentTypeId> empty_content_types{};
    for (const auto& mod_path : std::filesystem::directory_iterator{mod_folder, std::filesystem::directory_options::skip_permission_denied, ec}) {
        bool is_mod = false;
        bool requires_manifest = true;
        std::reference_wrapper<const std::vector<ModContentTypeId>> supported_content_types = std::cref(empty_content_types);
        // A regular file is a mod only if its extension matches a registered container type;
        // a directory is always treated as a (loose) mod.
        if (mod_path.is_regular_file()) {
            auto container_it = container_types.find(mod_path.path().extension().string());
            if (container_it != container_types.end()) {
                is_mod = true;
                supported_content_types = container_it->second.supported_content_types;
                requires_manifest = container_it->second.requires_manifest;
            }
        }
        else if (mod_path.is_directory()) {
            is_mod = true;
        }
        if (is_mod) {
            printf("Opening mod " PATHFMT "\n", mod_path.path().stem().c_str());
            std::string open_error_param;
            ModOpenError open_error = open_mod_from_path(mod_path, open_error_param, supported_content_types, requires_manifest);

            if (open_error != ModOpenError::Good) {
                ret.emplace_back(mod_path.path(), open_error, open_error_param);
            }
        }
        else {
            printf("Skipping non-mod " PATHFMT PATHFMT "\n", mod_path.path().stem().c_str(), mod_path.path().extension().c_str());
        }
    }

    for (const auto &mod_bytes : embedded_mod_bytes) {
        if (opened_mods_by_id.contains(mod_bytes.first)) {
            continue;
        }

        std::string open_error_param;
        ModOpenError open_error = open_mod_from_memory(mod_bytes.second, open_error_param, empty_content_types, true);
        if (open_error != ModOpenError::Good) {
            ret.emplace_back(mod_bytes.first, open_error, open_error_param);
        }
    }

    return ret;
}

void recomp::mods::ModContext::load_mods_config() {
    std::unordered_set<std::string> config_enabled_mods;
    std::vector<std::string> config_mod_order;
    std::vector<bool> opened_mod_is_known;
    parse_mods_config(mods_config_path, config_enabled_mods, config_mod_order);

    // Give every mod a sort key. Unrecognized (new) mods keep their default key
    // (their index), while mods named in the saved order receive larger keys so
    // they sort after the new ones while retaining their saved relative order.
    std::vector<size_t> relative_order;
    relative_order.resize(opened_mods.size());
    opened_mod_is_known.resize(opened_mods.size(), false);
    std::iota(relative_order.begin(), relative_order.end(), 0);
    for (size_t order_pos = 0; order_pos < config_mod_order.size(); order_pos++) {
        auto id_it = opened_mods_by_id.find(config_mod_order[order_pos]);
        if (id_it != opened_mods_by_id.end()) {
            relative_order[id_it->second] = opened_mods.size() + order_pos;
            opened_mod_is_known[id_it->second] = true;
        }
    }

    // Sort the order list by the computed keys.
    std::iota(opened_mods_order.begin(), opened_mods_order.end(), 0);
    std::sort(opened_mods_order.begin(), opened_mods_order.end(), [&](size_t lhs, size_t rhs) {
        return relative_order[lhs] < relative_order[rhs];
    });

    rebuild_mod_order_lookup();

    // Enable any mod that the config marks enabled, plus brand-new mods whose manifest opts in by default.
    for (size_t mod_index = 0; mod_index < opened_mods.size(); mod_index++) {
        const ModHandle& mod = opened_mods[mod_index];
        const std::string &mod_id = mod.manifest.mod_id;
        bool default_enabled = !opened_mod_is_known[mod_index] && mod.manifest.enabled_by_default;
        bool manually_enabled = config_enabled_mods.contains(mod_id);
        if (default_enabled || manually_enabled) {
            enable_mod(mod_id, true, false);
        }
    }
}

void recomp::mods::ModContext::rebuild_mod_order_lookup() {
    // Default every entry to -1; a mod that never appears in the order list keeps that sentinel.
    mod_order_lookup.resize(opened_mods.size());
    std::fill(mod_order_lookup.begin(), mod_order_lookup.end(), static_cast<size_t>(-1));

    // The lookup is just the inverse of opened_mods_order (mod index -> order position).
    for (size_t order_index = 0; order_index < opened_mods_order.size(); order_index++) {
        mod_order_lookup[opened_mods_order[order_index]] = order_index;
    }
}

recomp::mods::ModContext::ModContext() {
    // Register the code content type.
    ModContentType code_content_type {
        .content_filename = std::string{modpaths::binary_syms_path},
        .allow_runtime_toggle = false,
        .on_enabled = ModContext::on_code_mod_enabled,
        .on_disabled = nullptr,
        .on_reordered = nullptr
    };
    code_content_type_id = register_content_type(code_content_type);
    
    // Register the default mod container type (.nrm) and allow it to have any content type by passing an empty vector.
    register_container_type(std::string{ modpaths::default_mod_extension }, {}, true);

    mod_configuration_thread = std::make_unique<std::thread>(&ModContext::dirty_mod_configuration_thread_process, this);
}

void recomp::mods::ModContext::on_code_mod_enabled(ModContext& context, const ModHandle& mod) {
    auto find_mod_it = context.loaded_mods_by_id.find(mod.manifest.mod_id);
    if (find_mod_it == context.loaded_mods_by_id.end()) {
        assert(false && "Failed to find enabled code mod");
    }
    else {    
        context.loaded_code_mods.emplace_back(find_mod_it->second);
    }
}

recomp::mods::ModContext::~ModContext() {
    mod_configuration_thread_queue.enqueue(ModConfigQueueEnd());
    mod_configuration_thread->join();
    mod_configuration_thread.reset();
}

recomp::mods::ModContentTypeId recomp::mods::ModContext::register_content_type(const ModContentType& type) {
    size_t ret = content_types.size();
    content_types.emplace_back(type);

    return ModContentTypeId{.value = ret};
}

bool recomp::mods::ModContext::register_container_type(const std::string& extension, const std::vector<ModContentTypeId>& container_content_types, bool requires_manifest) {
    // Every referenced content type ID must already exist.
    for (ModContentTypeId id : container_content_types) {
        if (id.value >= content_types.size()) {
            return false;
        }
    }

    // The caller passes the extension without a leading dot, so reject one if present.
    if (extension.find('.') != std::string::npos) {
        return false;
    }

    // The key actually stored includes the leading dot.
    std::string true_extension = "." + extension;

    // Reject a second registration of the same extension.
    if (container_types.contains(true_extension)) {
        return false;
    }

    // Store the new container type.
    container_types.emplace(true_extension,
        ModContainerType {
            .supported_content_types = container_content_types,
            .requires_manifest = requires_manifest
        });

    return true;
}

std::string recomp::mods::ModContext::get_mod_display_name(size_t mod_index) const {
    return opened_mods[mod_index].manifest.display_name;
}

std::filesystem::path recomp::mods::ModContext::get_mod_path(size_t mod_index) const {
    return opened_mods[mod_index].manifest.mod_root_path;
}

std::pair<std::string, std::string> recomp::mods::ModContext::get_mod_import_info(size_t mod_index, size_t import_index) const {
    const ModHandle& mod = opened_mods[mod_index];
    const N64Recomp::ImportSymbol& imported_func = mod.recompiler_context->import_symbols[import_index];
    const std::string& dependency_id = mod.recompiler_context->dependencies[imported_func.dependency_index];

    return std::make_pair<std::string, std::string>(std::string{ dependency_id }, std::string{ imported_func.base.name });
}

recomp::mods::DependencyStatus recomp::mods::ModContext::is_dependency_met(size_t mod_index, const std::string& dependency_id) const {
    const ModHandle& mod = opened_mods[mod_index];

    // The id must actually be declared as a dependency of this mod.
    auto manifest_dep_it = mod.manifest.dependencies_by_id.find(dependency_id);
    if (manifest_dep_it == mod.manifest.dependencies_by_id.end()) {
        return DependencyStatus::InvalidDependency;
    }

    // The dependency also has to currently be loaded.
    auto loaded_dep_it = loaded_mods_by_id.find(dependency_id);
    if (loaded_dep_it == loaded_mods_by_id.end()) {
        return DependencyStatus::NotFound;
    }

    // Finally the loaded version has to satisfy the required version.
    const Dependency& required = mod.manifest.dependencies[manifest_dep_it->second];
    const ModHandle& dep_mod = opened_mods[loaded_dep_it->second];
    if (dep_mod.manifest.version < required.version) {
        return DependencyStatus::WrongVersion;
    }

    return DependencyStatus::Found;
}

bool recomp::mods::ModContext::is_content_runtime_toggleable(ModContentTypeId content_type) const {
    assert(content_type.value < content_types.size());

    return content_types[content_type.value].allow_runtime_toggle;
}

void recomp::mods::ModContext::enable_mod(const std::string& mod_id, bool enabled, bool trigger_save) {
    // Look the mod up; silently ignore unknown ids.
    std::unique_lock lock(opened_mods_mutex);
    auto mod_it = opened_mods_by_id.find(mod_id);
    if (mod_it == opened_mods_by_id.end()) {
        return;
    }
    ModHandle& mod = opened_mods[mod_it->second];

    bool mods_loaded = active_game != (size_t)-1;

    // Once mods are loaded, only runtime-toggleable mods belonging to the active
    // game may be toggled; otherwise this is a no-op.
    if (mods_loaded && !mod.is_runtime_toggleable()) {
        return;
    }
    if (mods_loaded && !mod.is_for_game(active_game)) {
        return;
    }

    if (enabled) {
        bool newly_enabled = enabled_mods.emplace(mod_id).second;

        // Fire on_enabled for the mod's content types if it actually transitioned to enabled after load.
        if (newly_enabled && mods_loaded) {
            for (ModContentTypeId type_id : mod.content_types) {
                content_enabled_callback* on_enabled = content_types[type_id.value].on_enabled;
                if (on_enabled != nullptr) {
                    on_enabled(*this, mod);
                }
            }
        }

        if (newly_enabled) {
            // Walk the required-dependency graph starting from this mod, marking each
            // newly reached non-optional dependency as auto-enabled.
            std::vector<std::string> pending;
            pending.emplace_back(mod_id);
            while (!pending.empty()) {
                std::string current_id = std::move(pending.back());
                pending.pop_back();

                auto current_it = opened_mods_by_id.find(current_id);
                if (current_it == opened_mods_by_id.end()) {
                    continue;
                }

                const ModHandle &current_mod = opened_mods[current_it->second];
                for (const Dependency &dependency : current_mod.manifest.dependencies) {
                    if (dependency.optional || auto_enabled_mods.contains(dependency.mod_id)) {
                        continue;
                    }

                    auto_enabled_mods.emplace(dependency.mod_id);
                    pending.emplace_back(dependency.mod_id);

                    if (mods_loaded) {
                        for (ModContentTypeId type_id : current_mod.content_types) {
                            content_enabled_callback* on_enabled = content_types[type_id.value].on_enabled;
                            if (on_enabled != nullptr) {
                                on_enabled(*this, current_mod);
                            }
                        }
                    }
                }
            }
        }
    }
    else {
        bool newly_disabled = enabled_mods.erase(mod_id) != 0;

        // Fire on_disabled for the mod's content types if it actually transitioned to disabled after load.
        if (newly_disabled && mods_loaded) {
            for (ModContentTypeId type_id : mod.content_types) {
                content_disabled_callback* on_disabled = content_types[type_id.value].on_disabled;
                if (on_disabled != nullptr) {
                    on_disabled(*this, mod);
                }
            }
        }

        if (newly_disabled) {
            // Disabling a mod can change the whole auto-enabled set, so recompute it
            // from scratch by traversing the dependency graph of every still-enabled mod.
            std::unordered_set<std::string> rebuilt_auto_enabled;
            for (const std::string &root_id : enabled_mods) {
                std::vector<std::string> pending;
                pending.emplace_back(root_id);
                while (!pending.empty()) {
                    std::string current_id = std::move(pending.back());
                    pending.pop_back();

                    auto current_it = opened_mods_by_id.find(current_id);
                    if (current_it == opened_mods_by_id.end()) {
                        continue;
                    }

                    const ModHandle &current_mod = opened_mods[current_it->second];
                    for (const Dependency &dependency : current_mod.manifest.dependencies) {
                        if (!dependency.optional && !rebuilt_auto_enabled.contains(dependency.mod_id)) {
                            rebuilt_auto_enabled.emplace(dependency.mod_id);
                            pending.emplace_back(dependency.mod_id);
                        }
                    }
                }
            }

            if (mods_loaded) {
                // Any mod that was auto-enabled before but is no longer in the rebuilt set
                // has effectively been disabled, so fire its on_disabled callbacks.
                for (const std::string &previous_id : auto_enabled_mods) {
                    if (rebuilt_auto_enabled.contains(previous_id)) {
                        continue;
                    }

                    auto previous_it = opened_mods_by_id.find(previous_id);
                    if (previous_it != opened_mods_by_id.end()) {
                        const ModHandle &previous_mod = opened_mods[previous_it->second];
                        for (ModContentTypeId type_id : previous_mod.content_types) {
                            content_disabled_callback* on_disabled = content_types[type_id.value].on_disabled;
                            if (on_disabled != nullptr) {
                                on_disabled(*this, previous_mod);
                            }
                        }
                    }
                }
            }

            auto_enabled_mods = rebuilt_auto_enabled;
        }
    }

    if (trigger_save) {
        mod_configuration_thread_queue.enqueue(ModConfigQueueSave());
    }
}

bool recomp::mods::ModContext::is_mod_enabled(const std::string& mod_id) {
    return enabled_mods.contains(mod_id);
}

bool recomp::mods::ModContext::is_mod_auto_enabled(const std::string& mod_id) {
    return auto_enabled_mods.contains(mod_id);
}

size_t recomp::mods::ModContext::num_opened_mods() {
    return opened_mods.size();
}

std::string recomp::mods::ModContext::get_mod_id_from_filename(const std::filesystem::path& filename) const {
    auto find_it = opened_mods_by_filename.find(filename.native());
    if (find_it == opened_mods_by_filename.end()) {
        return {};
    }

    return opened_mods[find_it->second].manifest.mod_id;
}

std::filesystem::path recomp::mods::ModContext::get_mod_filename(const std::string& mod_id) const {
    auto find_it = opened_mods_by_id.find(mod_id);
    if (find_it == opened_mods_by_id.end()) {
        return {};
    }

    return opened_mods[find_it->second].manifest.mod_root_path;
}

size_t recomp::mods::ModContext::get_mod_order_index(const std::string& mod_id) const {
    auto find_it = opened_mods_by_id.find(mod_id);
    if (find_it == opened_mods_by_id.end()) {
        return static_cast<size_t>(-1);
    }

    return get_mod_order_index(find_it->second);
}

size_t recomp::mods::ModContext::get_mod_order_index(size_t mod_index) const {
    size_t order_index = mod_order_lookup[mod_index];
    // Check if the mod has a proper order index and assert if it doesn't, as that means the mod isn't actually loaded.
    if (order_index == static_cast<size_t>(-1)) {
        assert(false);
        return static_cast<size_t>(-1);
    }

    return order_index;
}

std::optional<recomp::mods::ModDetails> recomp::mods::ModContext::get_details_for_mod(const std::string& mod_id) const {
    auto find_it = opened_mods_by_id.find(mod_id);
    if (find_it == opened_mods_by_id.end()) {
        return {};
    }

    size_t mod_index = find_it->second;
    const ModHandle &mod = opened_mods[mod_index];
    return mod.get_details();
}

std::vector<recomp::mods::ModDetails> recomp::mods::ModContext::get_all_mod_details(const std::string &mod_game_id) {
    std::vector<ModDetails> ret{};
    bool all_games = mod_game_id.empty();
    size_t game_index = (size_t)-1;

    auto find_game_it = mod_game_ids.find(mod_game_id);
    if (find_game_it != mod_game_ids.end()) {
        game_index = find_game_it->second;
    }

    for (size_t mod_index : opened_mods_order) {
        const ModHandle &mod = opened_mods[mod_index];
        if (all_games || mod.is_for_game(game_index)) {
            std::vector<Dependency> cur_dependencies{};

            ret.emplace_back(mod.get_details());
        }
    }

    return ret;
}

recomp::Version recomp::mods::ModContext::get_mod_version(size_t mod_index) {
    return opened_mods[mod_index].manifest.version;
}

std::string recomp::mods::ModContext::get_mod_id(size_t mod_index) {
    return opened_mods[mod_index].manifest.mod_id;
}

struct RegeneratedSection {
    uint32_t rom_addr;
    uint32_t ram_addr;
    uint16_t original_index;
    size_t first_func_index;
    size_t first_reloc_index;
    bool relocatable;
};

struct RegeneratedFunction {
    uint32_t section_offset;
    uint32_t size;
};

struct RegeneratedReloc {
    uint32_t section_offset;
    uint32_t target_section;
    uint32_t target_section_offset;
    RelocEntryType type;
};

struct RegeneratedList {
    std::vector<RegeneratedSection> sections;
    std::vector<RegeneratedFunction> functions;
    std::vector<RegeneratedReloc> relocs;

    // The native function pointers to be used for patching.
    std::vector<recomp_func_t*> func_ptrs;
    // Mappings of function index within context to hook slot index.
    std::unordered_map<size_t, size_t> entry_func_hooks;
    std::unordered_map<size_t, size_t> return_func_hooks;

    // Regeneration list for the patches.
    std::vector<std::pair<recomp::overlays::BasePatchedFunction, std::pair<recomp::mods::HookDefinition, size_t>>> patched_hooks;
};

N64Recomp::Context context_from_regenerated_list(const RegeneratedList& regenlist, std::span<const uint8_t> rom) {
    N64Recomp::Context ret{};

    // TODO find a way to avoid duplicating the entire ROM into the context.
    ret.rom.assign(rom.begin(), rom.end());

    ret.sections.resize(regenlist.sections.size());
    ret.section_functions.resize(regenlist.sections.size());
    ret.functions.resize(regenlist.functions.size());

    size_t section_count = regenlist.sections.size();
    for (size_t section_index = 0; section_index < section_count; section_index++) {
        const RegeneratedSection& section_in = regenlist.sections[section_index];
        N64Recomp::Section& section_out = ret.sections[section_index];

        // The function/reloc counts for a section are the gap between its first
        // index and the next section's first index (or the list end for the last one).
        bool is_last_section = (section_index + 1 == section_count);
        size_t func_list_end = is_last_section ? regenlist.functions.size() : regenlist.sections[section_index + 1].first_func_index;
        size_t reloc_list_end = is_last_section ? regenlist.relocs.size() : regenlist.sections[section_index + 1].first_reloc_index;
        size_t cur_num_funcs = func_list_end - section_in.first_func_index;
        size_t cur_num_relocs = reloc_list_end - section_in.first_reloc_index;

        section_out.rom_addr = section_in.rom_addr;
        section_out.ram_addr = section_in.ram_addr;
        section_out.size = 0;
        section_out.bss_size = 0;
        section_out.function_addrs.resize(cur_num_funcs);
        section_out.relocs.resize(cur_num_relocs);
        section_out.name = "patch_section_" + std::to_string(section_index);
        section_out.bss_section_index = 0;
        section_out.executable = true;
        section_out.relocatable = section_in.relocatable;
        section_out.has_mips32_relocs = false;

        std::vector<size_t>& section_funcs_out = ret.section_functions[section_index];
        section_funcs_out.resize(cur_num_funcs);

        for (size_t section_function_index = 0; section_function_index < cur_num_funcs; section_function_index++) {
            // Translate the section-local function index to the context-global one.
            size_t function_index = section_in.first_func_index + section_function_index;
            section_funcs_out[section_function_index] = function_index;

            // Fill in this function's metadata.
            const RegeneratedFunction& function_in = regenlist.functions[function_index];
            N64Recomp::Function& function_out = ret.functions[function_index];
            function_out.vram = section_out.ram_addr + function_in.section_offset;
            function_out.rom = section_out.rom_addr + function_in.section_offset;
            function_out.words.resize(function_in.size / sizeof(uint32_t));
            function_out.name = "patch_function_" + std::to_string(function_index);
            function_out.section_index = section_index;
            function_out.ignored = false;
            function_out.reimplemented = false;
            function_out.stubbed = false;
            function_out.function_hooks.clear();

            // Copy the raw instruction words straight out of the ROM image.
            const uint32_t* func_words = reinterpret_cast<const uint32_t*>(rom.data() + function_out.rom);
            function_out.words.assign(func_words, func_words + function_in.size / sizeof(uint32_t));

            // Register the function in the vram lookup map.
            ret.functions_by_vram[function_out.vram].push_back(function_index);
        }

        for (size_t section_reloc_index = 0; section_reloc_index < cur_num_relocs; section_reloc_index++) {
            // Translate the section-local reloc index to the global one.
            size_t reloc_index = section_in.first_reloc_index + section_reloc_index;

            const RegeneratedReloc& reloc_in = regenlist.relocs[reloc_index];
            N64Recomp::Reloc& reloc_out = section_out.relocs[section_reloc_index];

            reloc_out.address = reloc_in.section_offset + section_out.ram_addr;
            reloc_out.target_section_offset = reloc_in.target_section_offset;
            if (reloc_in.target_section == N64Recomp::SectionEvent) {
                // For event reference symbols the symbol index doubles as the event index.
                reloc_out.symbol_index = reloc_in.target_section_offset;
            }
            else {
                reloc_out.symbol_index = 0; // Live recompilation doesn't consult this field.
            }
            reloc_out.target_section = reloc_in.target_section;
            reloc_out.type = static_cast<N64Recomp::RelocType>(reloc_in.type);
            reloc_out.reference_symbol = true;
        }
    }

    return ret;
}

void recomp::mods::ModContext::set_mod_index(const std::string &mod_game_id, const std::string &mod_id, size_t index) {
    std::unique_lock lock(opened_mods_mutex);
    // An empty game id means the ordering view spans every game.
    bool all_games = mod_game_id.empty();
    size_t game_index = (size_t)-1;
    auto game_it = mod_game_ids.find(mod_game_id);
    if (game_it != mod_game_ids.end()) {
        game_index = game_it->second;
    }

    auto id_it = opened_mods_by_id.find(mod_id);
    if (id_it == opened_mods_by_id.end()) {
        return;
    }

    size_t mod_index = id_it->second;
    // The requested index counts only mods visible in this view, so track a
    // separate visible position while scanning the full order list.
    size_t visible_index = 0;
    bool inserted = false;
    bool erased = false;
    for (size_t i = 0; i < opened_mods_order.size() && (!inserted || !erased); i++) {
        size_t order_entry = opened_mods_order[i];
        const ModHandle &mod = opened_mods[order_entry];
        if (all_games || mod.is_for_game(game_index)) {
            if (index == visible_index) {
                // Reached the requested visible slot: drop the mod in ahead of this entry.
                opened_mods_order.insert(opened_mods_order.begin() + i, mod_index);
                inserted = true;
            }
            else if (mod_index == order_entry) {
                // Reached the mod's old location: remove it so it only lives at the new slot.
                opened_mods_order.erase(opened_mods_order.begin() + i);
                erased = true;
            }

            visible_index++;
        }
    }

    // If the requested slot was past the end, just append.
    if (!inserted) {
        opened_mods_order.push_back(mod_index);
    }

    rebuild_mod_order_lookup();

    // Notify content types that care about reordering.
    for (ModContentTypeId type_id : opened_mods[mod_index].content_types) {
        content_reordered_callback* on_reordered = content_types[type_id.value].on_reordered;
        if (on_reordered != nullptr) {
            on_reordered(*this);
        }
    }

    mod_configuration_thread_queue.enqueue(ModConfigQueueSave());
}

const recomp::mods::ConfigSchema &recomp::mods::ModContext::get_mod_config_schema(const std::string &mod_id) const {
    // Check that the mod exists.
    auto find_it = opened_mods_by_id.find(mod_id);
    if (find_it == opened_mods_by_id.end()) {
        return empty_schema;
    }

    const ModHandle &mod = opened_mods[find_it->second];
    return mod.manifest.config_schema;
}

const std::vector<char> &recomp::mods::ModContext::get_mod_thumbnail(const std::string &mod_id) const {
    // Check that the mod exists.
    auto find_it = opened_mods_by_id.find(mod_id);
    if (find_it == opened_mods_by_id.end()) {
        return empty_bytes;
    }

    const ModHandle &mod = opened_mods[find_it->second];
    return mod.thumbnail;
}

void recomp::mods::ModContext::set_mod_config_value(size_t mod_index, const std::string &option_id, const ConfigValueVariant &value) {
    // Check that the mod exists.
    if (mod_index >= opened_mods.size()) {
        return;
    }

    ModHandle &mod = opened_mods[mod_index];
    std::unique_lock lock(mod_config_storage_mutex);
    auto schema_it = mod.manifest.config_schema.options_by_id.find(option_id);
    if (schema_it != mod.manifest.config_schema.options_by_id.end()) {
        // Store the value only when its variant type matches the option's declared type
        // (and, for enums, when the index is within range).
        const ConfigOption &option = mod.manifest.config_schema.options[schema_it->second];
        switch (option.type) {
        case ConfigOptionType::Enum:
            if (std::holds_alternative<uint32_t>(value)) {
                if (std::get<uint32_t>(value) < std::get<ConfigOptionEnum>(option.variant).options.size()) {
                    mod.config_storage.value_map[option_id] = value;
                }
            }

            break;
        case ConfigOptionType::Number:
            if (std::holds_alternative<double>(value)) {
                mod.config_storage.value_map[option_id] = value;
            }

            break;
        case ConfigOptionType::String:
            if (std::holds_alternative<std::string>(value)) {
                mod.config_storage.value_map[option_id] = value;
            }

            break;
        default:
            assert(false && "Unknown config option type.");
            return;
        }
    }

    // Ask the background thread to persist this mod's configuration.
    mod_configuration_thread_queue.enqueue(ModConfigQueueSaveMod{ mod.manifest.mod_id });
}

void recomp::mods::ModContext::set_mod_config_value(const std::string &mod_id, const std::string &option_id, const ConfigValueVariant &value) {
    // Check that the mod exists.
    auto find_it = opened_mods_by_id.find(mod_id);
    if (find_it == opened_mods_by_id.end()) {
        return;
    }

    set_mod_config_value(find_it->second, option_id, value);
}

recomp::mods::ConfigValueVariant recomp::mods::ModContext::get_mod_config_value(size_t mod_index, const std::string &option_id) const {
    // Check that the mod exists.
    if (mod_index >= opened_mods.size()) {
        return std::monostate();
    }

    const ModHandle &mod = opened_mods[mod_index];
    std::unique_lock lock(mod_config_storage_mutex);
    auto stored_it = mod.config_storage.value_map.find(option_id);
    if (stored_it != mod.config_storage.value_map.end()) {
        return stored_it->second;
    }
    else {
        // No stored value, so fall back to the schema's declared default.
        auto schema_it = mod.manifest.config_schema.options_by_id.find(option_id);
        if (schema_it == mod.manifest.config_schema.options_by_id.end()) {
            return std::monostate();
        }

        const ConfigOption &option = mod.manifest.config_schema.options[schema_it->second];
        switch (option.type) {
        case ConfigOptionType::Enum:
            return std::get<ConfigOptionEnum>(option.variant).default_value;
        case ConfigOptionType::Number:
            return std::get<ConfigOptionNumber>(option.variant).default_value;
        case ConfigOptionType::String:
            return std::get<ConfigOptionString>(option.variant).default_value;
        default:
            assert(false && "Unknown config option type.");
            return std::monostate();
        }
    }
}

recomp::mods::ConfigValueVariant recomp::mods::ModContext::get_mod_config_value(const std::string &mod_id, const std::string &option_id) const {
    // Check that the mod exists.
    auto find_it = opened_mods_by_id.find(mod_id);
    if (find_it == opened_mods_by_id.end()) {
        return std::monostate();
    }

    return get_mod_config_value(find_it->second, option_id);
}

void recomp::mods::ModContext::set_mods_config_path(const std::filesystem::path &path) {
    mods_config_path = path;
}

void recomp::mods::ModContext::set_mod_config_directory(const std::filesystem::path &path) {
    mod_config_directory = path;
}

std::vector<recomp::mods::ModLoadErrorDetails> recomp::mods::ModContext::load_mods(const GameEntry& game_entry, uint8_t* rdram, int32_t load_address, uint32_t& ram_used) {
    std::vector<recomp::mods::ModLoadErrorDetails> ret{};
    ram_used = 0;
    num_events = recomp::overlays::num_base_events();
    loaded_code_mods.clear();

    std::span<const uint8_t> decompressed_rom{};

    // Games with compressed code need their ROM run through the decompression
    // routine first; everything else can use the ROM image as-is.
    std::vector<uint8_t> decompressed_rom_data{};
    if (game_entry.has_compressed_code) {
        if (game_entry.decompression_routine != nullptr) {
            decompressed_rom_data = game_entry.decompression_routine(recomp::get_rom());
        }
        decompressed_rom = std::span{decompressed_rom_data};
    }
    else {
        decompressed_rom = recomp::get_rom();
    }

    // Gather every function that the base recomp itself has already patched.
    std::unordered_map<recomp_func_t*, recomp::overlays::BasePatchedFunction> base_patched_funcs = recomp::overlays::get_base_patched_funcs();

    // The game has to be one we have a registered mod id for.
    auto game_index_it = mod_game_ids.find(game_entry.mod_game_id);
    if (game_index_it == mod_game_ids.end()) {
        ret.emplace_back(game_entry.mod_game_id, ModLoadError::InvalidGame, std::string{});
        return ret;
    }

    size_t mod_game_index = game_index_it->second;

    if (!patched_funcs.empty()) {
        printf("Mods already loaded!\n");
        return {};
    }

    const std::unordered_map<uint32_t, uint16_t>& section_vrom_map = recomp::overlays::get_vrom_to_section_map();

    std::vector<size_t> active_mods{};

    // Collect every mod that targets this game and is either explicitly or automatically enabled, then load each.
    for (size_t mod_index = 0; mod_index < opened_mods.size(); mod_index++) {
        auto& mod = opened_mods[mod_index];
        if (mod.is_for_game(mod_game_index) && (enabled_mods.contains(mod.manifest.mod_id) || auto_enabled_mods.contains(mod.manifest.mod_id))) {
            active_mods.push_back(mod_index);
            loaded_mods_by_id.emplace(mod.manifest.mod_id, mod_index);

            printf("Loading mod %s\n", mod.manifest.mod_id.c_str());
            std::string load_error_param;
            ModLoadError load_error = load_mod(mod, load_error_param);

            if (load_error != ModLoadError::Good) {
                ret.emplace_back(mod.manifest.mod_id, load_error, load_error_param);
            }
        }
    }

    // Abort the whole load if anything went wrong in this phase.
    if (!ret.empty()) {
        unload_mods();
        return ret;
    }

    // Verify every active mod's dependencies are satisfied.
    for (size_t mod_index : active_mods) {
        auto& mod = opened_mods[mod_index];
        std::vector<std::pair<ModLoadError, std::string>> cur_errors;
        check_dependencies(mod, cur_errors);

        if (!cur_errors.empty()) {
            for (auto const& [cur_error, cur_error_param] : cur_errors) {
                ret.emplace_back(mod.manifest.mod_id, cur_error, cur_error_param);
            }
        }
    }

    // Abort the whole load if anything went wrong in this phase.
    if (!ret.empty()) {
        unload_mods();
        return ret;
    }

    std::vector<uint32_t> base_event_indices;
    base_event_indices.resize(opened_mods.size());

    // First code-mod pass: parse symbols, copy binaries into RDRAM and reserve event slots.
    for (size_t mod_index : loaded_code_mods) {
        uint32_t cur_ram_used = 0;
        auto& mod = opened_mods[mod_index];
        std::string cur_error_param;
        size_t base_event_index = num_events;
        CodeModLoadError cur_error = init_mod_code(rdram, section_vrom_map, mod, load_address, !decompressed_rom.empty(), cur_ram_used, cur_error_param);
        if (cur_error != CodeModLoadError::Good) {
            if (cur_error_param.empty()) {
                ret.emplace_back(mod.manifest.mod_id, ModLoadError::FailedToLoadCode, error_to_string(cur_error));
            }
            else {
                ret.emplace_back(mod.manifest.mod_id, ModLoadError::FailedToLoadCode, error_to_string(cur_error) + ":" + cur_error_param);
            }
        }
        else {
            load_address += cur_ram_used;
            ram_used += cur_ram_used;
            base_event_indices[mod_index] = static_cast<uint32_t>(base_event_index);
        }
    }

    // Abort the whole load if anything went wrong in this phase.
    if (!ret.empty()) {
        unload_mods();
        return ret;
    }

    // Allocate the event callback table now that the total event count is known.
    recomp::mods::setup_events(num_events);

    // TODO if any hooks have been made but the decompressed rom isn't available,
    // present an error and stop loading mods.

    // Allocate one hook slot per unique hook definition discovered above.
    recomp::mods::setup_hooks(hook_slots.size());

    // Prepare the processed-slot tracking bitset.
    processed_hook_slots.clear();
    processed_hook_slots.resize(hook_slots.size());

    // Second code-mod pass: recompile/load each mod's code and register its exports.
    for (size_t mod_index : loaded_code_mods) {
        auto& mod = opened_mods[mod_index];
        std::string cur_error_param;
        CodeModLoadError cur_error = load_mod_code(rdram, mod, base_event_indices[mod_index], cur_error_param);
        if (cur_error != CodeModLoadError::Good) {
            if (cur_error_param.empty()) {
                ret.emplace_back(mod.manifest.mod_id, ModLoadError::FailedToLoadCode, error_to_string(cur_error));
            }
            else {
                ret.emplace_back(mod.manifest.mod_id, ModLoadError::FailedToLoadCode, error_to_string(cur_error) + ":" + cur_error_param);
            }
        }
    }

    // Abort the whole load if anything went wrong in this phase.
    if (!ret.empty()) {
        unload_mods();
        return ret;
    }
    
    // Third code-mod pass: wire up imports, callbacks, hooks and replacements.
    for (size_t mod_index : loaded_code_mods) {
        auto& mod = opened_mods[mod_index];
        std::string cur_error_param;
        CodeModLoadError cur_error = resolve_code_dependencies(mod, mod_index, base_patched_funcs, cur_error_param);
        if (cur_error != CodeModLoadError::Good) {
            if (cur_error_param.empty()) {
                ret.emplace_back(mod.manifest.mod_id, ModLoadError::FailedToLoadCode, error_to_string(cur_error));
            }
            else {
                ret.emplace_back(mod.manifest.mod_id, ModLoadError::FailedToLoadCode, error_to_string(cur_error) + ":" + cur_error_param);
            }
        }
    }

    // Abort the whole load if anything went wrong in this phase.
    if (!ret.empty()) {
        unload_mods();
        return ret;
    }

    // Any hook slot not already handled by a function replacement still needs the
    // underlying vanilla function regenerated so the hook can fire.

    // Gather the leftover hook slots and record each slot's entry/return kind.
    std::vector<std::pair<recomp::mods::HookDefinition, size_t>> unprocessed_hooks;
    for (const auto& [def, index] : hook_slots) {
        if (!processed_hook_slots[index]) {
            unprocessed_hooks.emplace_back(std::make_pair(def, index));
        }
        recomp::mods::set_hook_type(index, def.at_return);
    }

    if (!unprocessed_hooks.empty()) {

        // Order the leftover hooks by section ROM then function vram.
        std::sort(unprocessed_hooks.begin(), unprocessed_hooks.end(),
            [](const std::pair<recomp::mods::HookDefinition, size_t>& lhs, const std::pair<recomp::mods::HookDefinition, size_t>& rhs) {
                if (lhs.first.section_rom == rhs.first.section_rom) {
                    return lhs.first.function_vram < rhs.first.function_vram;
                }
                else {
                    return lhs.first.section_rom < rhs.first.section_rom;
                }
            }
        );

        ret = regenerate_with_hooks(unprocessed_hooks, section_vrom_map, base_patched_funcs, decompressed_rom);
        // Exit early if errors were found.
        if (!ret.empty()) {
            unload_mods();
            return ret;
        }
    }

    finish_event_setup(*this);
    finish_hook_setup(*this);

    active_game = mod_game_index;
    return ret;
}

template <bool patched_regenlist>
std::vector<recomp::mods::ModLoadErrorDetails> build_regen_list(
    const std::vector<std::pair<recomp::mods::HookDefinition, size_t>>& sorted_unprocessed_hooks,
    const std::unordered_map<uint32_t, uint16_t>& section_vrom_map,
    const std::unordered_map<recomp_func_t*, recomp::overlays::BasePatchedFunction>& base_patched_funcs,
    RegeneratedList& regenlist
) {
    using namespace recomp;
    using namespace recomp::mods;

    std::vector<ModLoadErrorDetails> ret{};
    uint32_t cur_section_rom = 0xFFFFFFFF;
    uint32_t cur_section_vram = 0xFFFFFFFF;
    uint16_t cur_section_index = 0xFFFF;
    uint32_t cur_function_vram = 0xFFFFFFFF;
    std::span<const RelocEntry> cur_section_relocs = {};
    size_t cur_section_reloc_index = 0;
    bool cur_func_is_base_patched = false;
    recomp::overlays::BasePatchedFunction cur_base_patched = {};

    // Group the (already section/function-sorted) hooks into the regenerated list.
    // Because they're sorted by section ROM then function vram, a simple single pass
    // can gather them section-by-section and function-by-function.
    for (size_t hook_index = 0; hook_index < sorted_unprocessed_hooks.size(); hook_index++) {
        const auto& cur_hook = sorted_unprocessed_hooks[hook_index];
        const auto& cur_hook_def = cur_hook.first;
        size_t cur_hook_slot_index = cur_hook.second;

        if (cur_hook_def.section_rom != cur_section_rom) {
            // Crossed into a new section; resolve its index.
            auto find_section_it = section_vrom_map.find(cur_hook_def.section_rom);
            if (find_section_it == section_vrom_map.end()) {
                std::stringstream error_param_stream{};
                error_param_stream << std::hex <<
                    "section: 0x" << cur_hook_def.section_rom <<
                    " func: 0x" << std::setfill('0') << std::setw(8) << cur_hook_def.function_vram;
                ret.emplace_back(ModLoadErrorDetails{
                    "", ModLoadError::FailedToLoadCode, error_to_string(CodeModLoadError::InvalidHook) + ":" + error_param_stream.str()
                });
                return ret;
            }

            uint16_t section_index = find_section_it->second;
            uint32_t section_ram_addr;

            if constexpr (patched_regenlist) {
                section_ram_addr = recomp::overlays::get_patch_section_ram_addr(section_index);
                cur_section_relocs = recomp::overlays::get_patch_section_relocs(section_index);
            }
            else {
                section_ram_addr = recomp::overlays::get_section_ram_addr(section_index);
                cur_section_relocs = recomp::overlays::get_section_relocs(section_index);
            }

            // Start a fresh regenerated section.
            auto& section_out = regenlist.sections.emplace_back(RegeneratedSection{
                .rom_addr = cur_hook_def.section_rom,
                .ram_addr = section_ram_addr,
                .original_index = section_index,
                .first_func_index = regenlist.functions.size(),
                .first_reloc_index = regenlist.relocs.size(),
                // Base patch sections are never relocatable; a regular section is relocatable when it actually has relocs.
                .relocatable = !patched_regenlist && !cur_section_relocs.empty()
            });

            // Refresh the carried section state.
            cur_section_rom = section_out.rom_addr;
            cur_section_vram = section_out.ram_addr;
            cur_section_index = section_index;
            cur_section_reloc_index = 0;

            // Clear the carried function vram so identical vrams in different sections don't collide.
            cur_function_vram = 0xFFFFFFFF;
        }

        if (cur_hook_def.function_vram != cur_function_vram) {
            uint32_t function_section_offset = cur_hook_def.function_vram - cur_section_vram;
            FuncEntry func_entry{};
            bool found_func;
            cur_func_is_base_patched = false;

            if constexpr (patched_regenlist) {
                found_func = recomp::overlays::get_patch_func_entry_by_section_index_function_offset(cur_section_index, function_section_offset, func_entry);
            }
            else {
                found_func = recomp::overlays::get_func_entry_by_section_index_function_offset(cur_section_index, function_section_offset, func_entry);
            }

            if (!found_func) {
                std::stringstream error_param_stream{};
                error_param_stream << std::hex <<
                    "section: 0x" << cur_hook_def.section_rom <<
                    " func: 0x" << std::setfill('0') << std::setw(8) << cur_hook_def.function_vram;
                ret.emplace_back(ModLoadErrorDetails{
                    "", ModLoadError::FailedToLoadCode, error_to_string(CodeModLoadError::InvalidHook) + ":" + error_param_stream.str()
                });
                return ret;
            }
        
            uint32_t function_rom_size = func_entry.rom_size;

            // A zero ROM size marks a function that has no real body to regenerate
            // (for instance a natively reimplemented one), so it can't carry a hook.
            if (function_rom_size == 0) {
                std::stringstream error_param_stream{};
                error_param_stream << std::hex <<
                    "section: 0x" << cur_hook_def.section_rom <<
                    " func: 0x" << std::setfill('0') << std::setw(8) << cur_hook_def.function_vram;
                ret.emplace_back(ModLoadErrorDetails{
                    "", ModLoadError::FailedToLoadCode, error_to_string(CodeModLoadError::CannotBeHooked) + ":" + error_param_stream.str()
                });
                return ret;
            }
            
            // For regular (non-patch) regeneration, note whether the base recomp already patched this function.
            if constexpr (!patched_regenlist) {
                auto find_patched_it = base_patched_funcs.find(func_entry.func);
                if (find_patched_it != base_patched_funcs.end()) {
                    cur_func_is_base_patched = true;
                    cur_base_patched = find_patched_it->second;
                }
            }

            if (!cur_func_is_base_patched) {
                // Append a function entry plus its native pointer for later patching.
                regenlist.functions.emplace_back(RegeneratedFunction{
                    .section_offset = function_section_offset,
                    .size = function_rom_size
                });
                regenlist.func_ptrs.push_back(func_entry.func);
            }

            // Remember this function as the currently tracked one.
            cur_function_vram = cur_hook_def.function_vram;

            // Skip past any relocs that fall before this function's start offset.
            size_t cur_function_offset = cur_function_vram - cur_section_vram;
            size_t cur_function_end_offset = cur_function_offset + function_rom_size;
            while (true) {
                if (cur_section_reloc_index >= cur_section_relocs.size()) {
                    break;
                }
                const auto& reloc_in = cur_section_relocs[cur_section_reloc_index];
                if (reloc_in.offset >= cur_function_offset) {
                    break;
                }

                cur_section_reloc_index++;
            }

            // Copy every reloc that lies within this function's extent.
            while (true) {
                if (cur_section_reloc_index >= cur_section_relocs.size()) {
                    break;
                }

                const auto& reloc_in = cur_section_relocs[cur_section_reloc_index];
                if (reloc_in.offset >= cur_function_end_offset) {
                    break;
                }

                regenlist.relocs.emplace_back(RegeneratedReloc {
                    .section_offset = reloc_in.offset,
                    .target_section = reloc_in.target_section,
                    .target_section_offset = reloc_in.target_section_offset,
                    .type = reloc_in.type
                });

                cur_section_reloc_index++;
            }
        }

        if (cur_func_is_base_patched) {
            // Base-patched functions get handled later through a separate patch regenlist.
            regenlist.patched_hooks.emplace_back(std::make_pair(cur_base_patched, cur_hook));
        }
        else {
            // Map the most recently added function to this hook slot, by hook kind.
            size_t func_index = regenlist.functions.size() - 1;
            if (cur_hook_def.at_return) {
                regenlist.return_func_hooks[func_index] = cur_hook_slot_index;
            }
            else {
                regenlist.entry_func_hooks[func_index] = cur_hook_slot_index;
            }
        }
    }

    return {};
}

std::unique_ptr<recomp::mods::LiveRecompilerCodeHandle> apply_regenlist(RegeneratedList& regenlist, std::span<const uint8_t> rom) {
    using namespace recomp::mods;

    std::unique_ptr<LiveRecompilerCodeHandle> regenerated_code_handle{};

    // Build a recompiler context from the regenerated list, marking all reference
    // sections relocatable and forcing every call to go through the lookup table.
    N64Recomp::Context hook_context = context_from_regenerated_list(regenlist, rom);
    hook_context.set_all_reference_sections_relocatable();
    hook_context.use_lookup_for_all_function_calls = true;

    // Inputs handed to the live recompiler for the regenerated functions.
    ModCodeHandleInputs handle_inputs{
        .base_event_index = 0, // No events in vanilla functions, so this doesn't matter.
        .recomp_trigger_event = recomp_trigger_event,
        .get_function = get_function,
        .cop0_status_write = cop0_status_write,
        .cop0_status_read = cop0_status_read,
        .switch_error = switch_error,
        .do_break = do_break,
        .reference_section_addresses = section_addresses,
    };

    // Map each regenerated section back to the original section index it came from.
    std::vector<size_t> original_section_indices{};
    original_section_indices.resize(regenlist.sections.size());
    for (size_t new_section_index = 0; new_section_index < regenlist.sections.size(); new_section_index++) {
        original_section_indices[new_section_index] = regenlist.sections[new_section_index].original_index;
    }

    regenerated_code_handle = std::make_unique<LiveRecompilerCodeHandle>(hook_context, handle_inputs,
        std::move(regenlist.entry_func_hooks), std::move(regenlist.return_func_hooks), std::move(original_section_indices), true);

    if (!regenerated_code_handle->good()) {
        return {};
    }
    
    std::string reference_syms_error_param{};
    CodeModLoadError reference_syms_error = regenerated_code_handle->populate_reference_symbols(hook_context, reference_syms_error_param);
    if (reference_syms_error != CodeModLoadError::Good) {
        return {};
    }

    // Redirect each original function to its freshly regenerated counterpart.
    for (size_t patched_func_index = 0; patched_func_index < regenlist.func_ptrs.size(); patched_func_index++) {
        patch_func(regenlist.func_ptrs[patched_func_index], regenerated_code_handle->get_function_handle(patched_func_index));
    }

    return regenerated_code_handle;
}

std::vector<recomp::mods::ModLoadErrorDetails> recomp::mods::ModContext::regenerate_with_hooks(
    const std::vector<std::pair<HookDefinition, size_t>>& sorted_unprocessed_hooks,
    const std::unordered_map<uint32_t, uint16_t>& section_vrom_map,
    const std::unordered_map<recomp_func_t*, overlays::BasePatchedFunction>& base_patched_funcs,
    std::span<const uint8_t> decompressed_rom
) {
    // Accumulates the functions/relocs that need regenerating.
    RegeneratedList regenlist{};

    std::vector<ModLoadErrorDetails> ret = build_regen_list<false>(sorted_unprocessed_hooks, section_vrom_map, base_patched_funcs, regenlist);
    if (!ret.empty()) {
        return ret;
    }

    // Regenerate and patch the ordinary (non-base-patched) functions.
    if (!regenlist.functions.empty()) {
        regenerated_code_handle = apply_regenlist(regenlist, decompressed_rom);
        if (!regenerated_code_handle || !regenerated_code_handle->good()) {
            regenerated_code_handle.reset();
            ret.emplace_back(ModLoadErrorDetails{
                "", ModLoadError::FailedToLoadCode, error_to_string(CodeModLoadError::InternalError)
            });
            return ret;
        }
    }

    if (!regenlist.patched_hooks.empty()) {
        // Re-express each base-patched hook in terms of the patch binary's real addresses.
        std::vector<std::pair<HookDefinition, size_t>> base_patched_hooks{};
        base_patched_hooks.resize(regenlist.patched_hooks.size());
        for (size_t i = 0; i < regenlist.patched_hooks.size(); i++) {
            const auto& regenlist_entry = regenlist.patched_hooks[i];
            uint16_t patch_section_index = static_cast<uint16_t>(regenlist_entry.first.patch_section);
            uint32_t patch_section_ram_addr = overlays::get_patch_section_ram_addr(patch_section_index);
            const FuncEntry* func_entry = overlays::get_patch_function_entry(patch_section_index, regenlist_entry.first.function_index);
            base_patched_hooks[i].first = HookDefinition {
                .section_rom = overlays::get_patch_section_rom_addr(patch_section_index),
                .function_vram = patch_section_ram_addr + func_entry->offset,
                .at_return = regenlist_entry.second.first.at_return
            };
            base_patched_hooks[i].second = regenlist_entry.second.second;
        }

        // Order them by section ROM then function vram.
        std::sort(base_patched_hooks.begin(), base_patched_hooks.end(),
            [](const std::pair<recomp::mods::HookDefinition, size_t>& lhs, const std::pair<recomp::mods::HookDefinition, size_t>& rhs) {
                if (lhs.first.section_rom == rhs.first.section_rom) {
                    return lhs.first.function_vram < rhs.first.function_vram;
                }
                else {
                    return lhs.first.section_rom < rhs.first.section_rom;
                }
            }
        );

        // Build the regenerated list against the patch binary's sections.
        std::unordered_map<uint32_t, uint16_t> patch_section_vrom_map = overlays::get_patch_vrom_to_section_map();
        RegeneratedList patch_regenlist{};
        std::vector<ModLoadErrorDetails> ret = build_regen_list<true>(base_patched_hooks, patch_section_vrom_map, {}, patch_regenlist);
        if (!ret.empty()) {
            return ret;
        }

        // Regenerate and patch the base-patched functions.
        base_patched_code_handle = apply_regenlist(patch_regenlist, overlays::get_patch_binary());
        if (!base_patched_code_handle || !base_patched_code_handle->good()) {
            regenerated_code_handle.reset();
            base_patched_code_handle.reset();
            ret.emplace_back(ModLoadErrorDetails{
                "", ModLoadError::FailedToLoadCode, error_to_string(CodeModLoadError::InternalError)
            });
            return ret;
        }
    }

    return ret;
}

void recomp::mods::ModContext::check_dependencies(recomp::mods::ModHandle& mod, std::vector<std::pair<recomp::mods::ModLoadError, std::string>>& errors) {
    errors.clear();
    // A mod that has dependencies can't safely be toggled at runtime yet.
    // TODO lift this restriction.
    if (!mod.manifest.dependencies.empty()) {
        mod.disable_runtime_toggle();
    }
    for (const recomp::mods::Dependency& cur_dep : mod.manifest.dependencies) {
        // Optional dependencies never produce errors here.
        if (cur_dep.optional) {
            continue;
        }

        auto loaded_dep_it = loaded_mods_by_id.find(cur_dep.mod_id);
        if (loaded_dep_it == loaded_mods_by_id.end()) {
            // A required dependency that isn't loaded is a hard error.
            errors.emplace_back(ModLoadError::MissingDependency, cur_dep.mod_id);
            continue;
        }

        ModHandle& dep_mod = opened_mods[loaded_dep_it->second];
        // The loaded version must be at least the required version.
        if (cur_dep.version > dep_mod.manifest.version) {
            std::stringstream error_param_stream{};
            error_param_stream << "requires mod \"" << cur_dep.mod_id << "\" " <<
                (int)cur_dep.version.major << "." << (int)cur_dep.version.minor << "." << (int)cur_dep.version.patch << ", got " <<
                (int)dep_mod.manifest.version.major << "." << (int)dep_mod.manifest.version.minor << "." << (int)dep_mod.manifest.version.patch << "";
            errors.emplace_back(ModLoadError::WrongDependencyVersion, error_param_stream.str());
        }

        // A required dependency must stay loaded, so lock it against runtime toggling.
        dep_mod.disable_runtime_toggle();
    }
}

recomp::mods::CodeModLoadError recomp::mods::ModContext::init_mod_code(uint8_t* rdram, const std::unordered_map<uint32_t, uint16_t>& section_vrom_map, ModHandle& mod, int32_t load_address, bool hooks_available, uint32_t& ram_used, std::string& error_param) {
    // Read both the symbol blob and the code binary named by the manifest.
    bool binary_syms_exists = false;
    std::vector<char> syms_data = mod.manifest.file_handle->read_file(std::string{ modpaths::binary_syms_path }, binary_syms_exists);

    bool binary_exists = false;
    std::vector<char> binary_data = mod.manifest.file_handle->read_file(std::string{ modpaths::binary_path }, binary_exists);

    // Symbols and binary must come as a matched pair.
    if (binary_syms_exists && !binary_exists) {
        return CodeModLoadError::HasSymsButNoBinary;
    }

    if (binary_exists && !binary_syms_exists) {
        return CodeModLoadError::HasBinaryButNoSyms;
    }

    std::span<uint8_t> binary_span {reinterpret_cast<uint8_t*>(binary_data.data()), binary_data.size() };

    // Feed the symbol data into this mod's recompiler context.
    N64Recomp::ModSymbolsError symbol_load_error = N64Recomp::parse_mod_symbols(syms_data, binary_span, section_vrom_map, *mod.recompiler_context);
    if (symbol_load_error != N64Recomp::ModSymbolsError::Good) {
        return CodeModLoadError::FailedToParseSyms;
    }

    // A mod that defines hooks can't load when hook support is unavailable.
    if (!hooks_available && !mod.recompiler_context->hooks.empty()) {
        return CodeModLoadError::HooksUnavailable;
    }

    // Set all reference sections as relocatable, since the only relocations present in a mod's context
    // are ones that target relocatable sections.
    mod.recompiler_context->set_all_reference_sections_relocatable();
    // Disable validation of reference symbols (so we can skip populating them). Validation will still happen
    // later on in the live recompilation process.
    mod.recompiler_context->skip_validating_reference_symbols = true;

    // Populate the mod's export map.
    mod.populate_exports();

    // Populate the mod's event map and set its base event index.
    mod.populate_events();

    // Every non-special dependency named by the symbols must also appear in the manifest.
    for (const auto& [cur_dep_id, cur_dep_index] : mod.recompiler_context->dependencies_by_name) {
        // The reserved base-recomp and self dependencies don't live in the manifest.
        if (cur_dep_id == N64Recomp::DependencyBaseRecomp || cur_dep_id == N64Recomp::DependencySelf) {
            continue;
        }

        auto find_manifest_dep_it = mod.manifest.dependencies_by_id.find(cur_dep_id);
        if (find_manifest_dep_it == mod.manifest.dependencies_by_id.end()) {

            return CodeModLoadError::MissingDependencyInManifest;
        }
    }

    const std::vector<N64Recomp::Section>& mod_sections = mod.recompiler_context->sections;
    mod.section_load_addresses.resize(mod_sections.size());

    // Lay each section out in RDRAM in turn, reserving its bss right after its data.
    int32_t cur_section_addr = load_address;
    for (size_t section_index = 0; section_index < mod_sections.size(); section_index++) {
        const auto& section = mod_sections[section_index];
        // Fixed-address sections aren't relocated into mod memory; keep their declared address.
        if (section.fixed_address) {
            mod.section_load_addresses[section_index] = section.ram_addr;
        }
        else {
            // Copy the section's bytes from the binary into RDRAM.
            for (size_t i = 0; i < section.size; i++) {
                MEM_B(i, (gpr)cur_section_addr) = binary_data[section.rom_addr + i];
            }
            mod.section_load_addresses[section_index] = cur_section_addr;
            // The bss immediately follows the loaded data.
            cur_section_addr += section.size;
            // Clear the bss region.
            for (size_t i = 0; i < section.bss_size; i++) {
                MEM_B(i, (gpr)cur_section_addr) = 0;
            }
            // Advance past the bss.
            cur_section_addr += section.bss_size;
            // Round the next placement up to a 16-byte boundary.
            cur_section_addr = (cur_section_addr + 15) & ~15;
            // Leave a guard gap so out-of-bounds accesses from a buggy mod stay contained.
            cur_section_addr += 0x400;
        }
    }

    // With every section placed, make a second pass to fix up R_MIPS_32 relocations.
    for (size_t section_index = 0; section_index < mod_sections.size(); section_index++) {
        const auto& section = mod_sections[section_index];
        uint32_t cur_section_original_vram = section.ram_addr;
        uint32_t cur_section_loaded_vram = mod.section_load_addresses[section_index];

        for (const auto& reloc : section.relocs) {
            if (reloc.type == N64Recomp::RelocType::R_MIPS_32 && !reloc.reference_symbol) {
                if (reloc.target_section >= mod_sections.size()) {
                    return CodeModLoadError::FailedToParseSyms;
                }
                // Translate the reloc's address into the loaded image and read the stored word.
                int32_t reloc_word_addr = reloc.address - cur_section_original_vram + cur_section_loaded_vram;
                uint32_t reloc_word = MEM_W(0, reloc_word_addr);

                // Look up where the pointed-to section originally was versus where it landed.
                uint32_t target_section_original_vram = mod_sections[reloc.target_section].ram_addr;
                uint32_t target_section_loaded_vram = mod.section_load_addresses[reloc.target_section];

                // Shift the stored word by the section's relocation delta and store it back.
                reloc_word += (target_section_loaded_vram - target_section_original_vram);
                MEM_W(0, reloc_word_addr) = reloc_word;
            }
        }
    }

    ram_used = cur_section_addr - load_address;

    // Reserve event indices for this mod's events.
    num_events += mod.num_events();

    // Register a unique slot for every distinct hook definition this mod declares.
    for (const N64Recomp::FunctionHook& hook : mod.recompiler_context->hooks) {
        HookDefinition def {
            .section_rom = hook.original_section_vrom,
            .function_vram = hook.original_vram,
            .at_return = (hook.flags & N64Recomp::HookFlags::AtReturn) == N64Recomp::HookFlags::AtReturn
        };
        // Only allocate a slot if this exact definition hasn't been seen yet.
        if (hook_slots.find(def) == hook_slots.end()) {
            hook_slots.emplace(def, hook_slots.size());
        }
    }

    // Keep a copy of the binary in the recompiler context for use during code loading.
    // TODO move rather than copy once the signedness mismatch in the element types is resolved.
    mod.recompiler_context->rom.assign(binary_span.begin(), binary_span.end());

    return CodeModLoadError::Good;
}

recomp::mods::CodeModLoadError recomp::mods::ModContext::load_mod_code(uint8_t* rdram, recomp::mods::ModHandle& mod, uint32_t base_event_index, std::string& error_param) {
    // Per-function hook maps for this mod: mod function index -> hook slot index.
    std::unordered_map<size_t, size_t> entry_func_hooks{};
    std::unordered_map<size_t, size_t> return_func_hooks{};

    // When a mod replaces a function that is also hooked, the replacement function
    // itself carries those hooks, so wire each replaced function's entry/return slots.
    for (const auto& replacement : mod.recompiler_context->replacements) {
        // Entry hook slot for the replaced function, if one exists.
        HookDefinition entry_def {
            .section_rom = replacement.original_section_vrom,
            .function_vram = replacement.original_vram,
            .at_return = false
        };
        auto entry_slot_it = hook_slots.find(entry_def);
        if (entry_slot_it != hook_slots.end()) {
            entry_func_hooks.emplace(replacement.func_index, entry_slot_it->second);
            processed_hook_slots[entry_slot_it->second] = true;
        }

        // Return hook slot for the replaced function, if one exists.
        HookDefinition return_def {
            .section_rom = replacement.original_section_vrom,
            .function_vram = replacement.original_vram,
            .at_return = true
        };
        auto return_slot_it = hook_slots.find(return_def);
        if (return_slot_it != hook_slots.end()) {
            return_func_hooks.emplace(replacement.func_index, return_slot_it->second);
            processed_hook_slots[return_slot_it->second] = true;
        }
    }

    // Inputs shared by either code handle implementation.
    std::string cur_error_param;
    CodeModLoadError cur_error;
    ModCodeHandleInputs handle_inputs{
        .base_event_index = base_event_index,
        .recomp_trigger_event = recomp_trigger_event,
        .get_function = get_function,
        .cop0_status_write = cop0_status_write,
        .cop0_status_read = cop0_status_read,
        .switch_error = switch_error,
        .do_break = do_break,
        .reference_section_addresses = section_addresses,
    };

    // A filename ending in ".offline.nrm" selects the dynamic-library handle. This
    // path is a developer aid (easier to debug than the live recompiler) rather than
    // something end users are expected to use.
    if (mod.manifest.mod_root_path.filename().string().ends_with(".offline.nrm")) {
        // The offline path can't synthesize hooks, so reject it when any replaced function is also hooked.
        if (!entry_func_hooks.empty() || !return_func_hooks.empty()) {
            return CodeModLoadError::OfflineModHooked;
        }

        std::filesystem::path dll_path = mod.manifest.mod_root_path;
        dll_path.replace_extension(DynamicLibrary::PlatformExtension);
        mod.code_handle = std::make_unique<DynamicLibraryCodeHandle>(dll_path, *mod.recompiler_context, handle_inputs);
        if (!mod.code_handle->good()) {
            mod.code_handle.reset();
            error_param = dll_path.string();
            return CodeModLoadError::FailedToLoadNativeCode;
        }

        cur_error = validate_api_version(mod.code_handle->get_api_version(), cur_error_param);

        if (cur_error != CodeModLoadError::Good) {
            if (cur_error_param.empty()) {
                error_param = dll_path.filename().string();
            }
            else {
                error_param = dll_path.filename().string() + ":" + std::move(cur_error_param);
            }
            return cur_error;
        }
    }
    // Otherwise use the live recompiler handle (the default for shipped mods).
    else {
        mod.code_handle = std::make_unique<LiveRecompilerCodeHandle>(*mod.recompiler_context, handle_inputs,
            std::move(entry_func_hooks), std::move(return_func_hooks), std::vector<size_t>{}, false);

        if (!mod.code_handle->good()) {
            mod.code_handle.reset();
            error_param = {};
            return CodeModLoadError::FailedToRecompile;
        }
    }

    // Bring in and validate every native library the mod declares.
    std::filesystem::path parent_path = mod.manifest.mod_root_path.parent_path();
    for (const recomp::mods::NativeLibraryManifest& cur_lib_manifest: mod.manifest.native_libraries) {
        cur_error = mod.load_native_library(cur_lib_manifest, cur_error_param);
        if (cur_error != CodeModLoadError::Good) {
            error_param = std::move(cur_error_param);
            return cur_error;
        }
    }

    const std::vector<N64Recomp::Section>& mod_sections = mod.recompiler_context->sections;

    // Publish each of the mod's functions into the global function lookup table.
    for (size_t func_index = 0; func_index < mod.recompiler_context->functions.size(); func_index++) {
        const auto& func = mod.recompiler_context->functions[func_index];
        if (func.section_index >= mod_sections.size()) {
            return CodeModLoadError::FailedToParseSyms;
        }
        // Translate the function's vram into its actual loaded address.
        int32_t func_address = func.vram - mod_sections[func.section_index].ram_addr + mod.section_load_addresses[func.section_index];

        // Register the function handle at its loaded address.
        recomp::mods::GenericFunction func_handle = mod.code_handle->get_function_handle(func_index);
        std::visit(overloaded{
            [func_address](recomp_func_t* native_func) {
                recomp::overlays::add_loaded_function(func_address, native_func);
            }
            }, func_handle);
    }

    return CodeModLoadError::Good;
}

recomp::mods::CodeModLoadError recomp::mods::ModContext::resolve_code_dependencies(recomp::mods::ModHandle& mod, size_t mod_index, const std::unordered_map<recomp_func_t*, overlays::BasePatchedFunction>& base_patched_funcs, std::string& error_param) {
    // Resolve reference symbols first.
    std::string reference_syms_error_param{};
    CodeModLoadError reference_syms_error = mod.code_handle->populate_reference_symbols(*mod.recompiler_context, reference_syms_error_param);

    if (reference_syms_error != CodeModLoadError::Good) {
        error_param = std::move(reference_syms_error_param);
        return reference_syms_error;
    }

    // Flatten the dependency-name map into an index-ordered list for quick lookups.
    std::vector<std::string> dependencies_ordered{};
    dependencies_ordered.resize(mod.recompiler_context->dependencies_by_name.size());

    for (const auto& [dependency, dependency_index] : mod.recompiler_context->dependencies_by_name) {
        dependencies_ordered[dependency_index] = dependency;
    }

    // Bind every imported symbol to a concrete function.
    for (size_t import_index = 0; import_index < mod.recompiler_context->import_symbols.size(); import_index++) {
        const N64Recomp::ImportSymbol& imported_func = mod.recompiler_context->import_symbols[import_index];
        const std::string& dependency_id = dependencies_ordered[imported_func.dependency_index];

        GenericFunction func_handle{};
        bool did_find_func = false;

        if (dependency_id == N64Recomp::DependencyBaseRecomp) {
            // Try a plain base export first, then an extended one (wrapped in a shim).
            recomp_func_t* func_ptr = recomp::overlays::get_base_export(imported_func.base.name);
            did_find_func = func_ptr != nullptr;
            if (!did_find_func) {
                recomp_func_ext_t* func_ext_ptr = recomp::overlays::get_ext_base_export(imported_func.base.name);
                did_find_func = func_ext_ptr != nullptr;
                if (did_find_func) {
                    func_ptr = shim_functions.emplace_back(std::make_unique<N64Recomp::ShimFunction>(func_ext_ptr, mod_index)).get()->get_func();
                }
            }
            func_handle = func_ptr;
        }
        else if (dependency_id == N64Recomp::DependencySelf) {
            // The mod imports one of its own exports.
            did_find_func = mod.get_export_function(imported_func.base.name, func_handle);
        }
        else {
            auto find_mod_it = loaded_mods_by_id.find(dependency_id);
            if (find_mod_it != loaded_mods_by_id.end()) {
                // The dependency is loaded, so pull the export from it.
                const auto& dependency = opened_mods[find_mod_it->second];
                did_find_func = dependency.get_export_function(imported_func.base.name, func_handle);
            }
            else {
                auto find_optional_it = mod.manifest.dependencies_by_id.find(dependency_id);
                if (find_optional_it != mod.manifest.dependencies_by_id.end()) {
                    // An unmet but optional dependency: point the import at a shim that
                    // errors out only if the import is actually called. The argument packs
                    // the import index in the high word and the mod index in the low word.
                    uintptr_t shim_argument = ((import_index & 0xFFFFFFFFu) << 32) | (mod_index & 0xFFFFFFFFu);
                    func_handle = shim_functions.emplace_back(std::make_unique<N64Recomp::ShimFunction>(unmet_dependency_handler, shim_argument)).get()->get_func();
                    did_find_func = true;
                }
                else {
                    error_param = "Failed to find import dependency while loading code: " + dependency_id;
                    // This should never happen, as dependencies are scanned before mod code is loaded and the symbol dependency list
                    // is validated against the manifest's. 
                    return CodeModLoadError::InternalError;
                }
            }
        }

        if (!did_find_func) {
            error_param = dependency_id + ":" + imported_func.base.name;
            return CodeModLoadError::InvalidImport;
        }

        mod.code_handle->set_imported_function(import_index, func_handle);
    }

    // Register callbacks.
    for (const N64Recomp::Callback& callback : mod.recompiler_context->callbacks) {
        const N64Recomp::DependencyEvent& dependency_event = mod.recompiler_context->dependency_events[callback.dependency_event_index];
        const std::string& dependency_id = dependencies_ordered[dependency_event.dependency_index];
        GenericFunction func = mod.code_handle->get_function_handle(callback.function_index);
        size_t event_index = 0;
        bool did_find_event = false;
        bool optional = false;

        if (dependency_id == N64Recomp::DependencyBaseRecomp) {
            event_index = recomp::overlays::get_base_event_index(dependency_event.event_name);
            if (event_index != (size_t)-1) {
                did_find_event = true;
            }
        }
        else if (dependency_id == N64Recomp::DependencySelf) {
            did_find_event = mod.get_global_event_index(dependency_event.event_name, event_index);
        }
        else {
            auto find_mod_it = loaded_mods_by_id.find(dependency_id);
            if (find_mod_it == loaded_mods_by_id.end()) {
                // The dependency isn't loaded; that's only acceptable if it's optional.
                auto find_manifest_dep = mod.manifest.dependencies_by_id.find(dependency_id);
                // It should always be in the manifest (validated earlier); treat absence as non-optional.
                if (find_manifest_dep != mod.manifest.dependencies_by_id.end()) {
                    const auto& manifest_dep = mod.manifest.dependencies[find_manifest_dep->second];
                    if (manifest_dep.optional) {
                        optional = true;
                    }
                }

                if (!optional) {
                    error_param = "Failed to find callback dependency while loading code: " + dependency_id;
                    // Unreachable in practice: dependencies are validated against the manifest before code loads.
                    return CodeModLoadError::InternalError;
                }
            }
            else {
                // The dependency is loaded, so resolve the event against it.
                const auto& dependency_mod = opened_mods[find_mod_it->second];
                did_find_event = dependency_mod.get_global_event_index(dependency_event.event_name, event_index);
            }
        }

        if (did_find_event) {
            recomp::mods::register_event_callback(event_index, mod_index, func);
        }
        else if (!optional) {
            error_param = dependency_id + ":" + dependency_event.event_name;
            return CodeModLoadError::InvalidCallbackEvent;
        }
    }

    // Register the mod's hook callbacks.
    for (const auto& cur_hook : mod.recompiler_context->hooks) {
        HookDefinition def {
            .section_rom = cur_hook.original_section_vrom,
            .function_vram = cur_hook.original_vram,
            .at_return = (cur_hook.flags & N64Recomp::HookFlags::AtReturn) == N64Recomp::HookFlags::AtReturn
        };

        // The slot must already exist from the earlier hook scan.
        auto find_it = hook_slots.find(def);
        if (find_it == hook_slots.end()) {
            error_param = "Failed to register hook";
            // Unreachable in practice: every hook got a slot during init_mod_code.
            return CodeModLoadError::InternalError;
        }

        // Attach this mod's function to the hook slot.
        GenericFunction func = mod.code_handle->get_function_handle(cur_hook.func_index);
        recomp::mods::register_hook(find_it->second, mod_index, func);
    }

    // Hand the loaded section addresses to the code handle.
    for (size_t section_index = 0; section_index < mod.section_load_addresses.size(); section_index++) {
        mod.code_handle->set_local_section_address(section_index, mod.section_load_addresses[section_index]);
    }

    // Install each function replacement the mod requests.
    for (const auto& replacement : mod.recompiler_context->replacements) {
        recomp_func_t* to_replace = recomp::overlays::get_func_by_section_rom_function_vram(replacement.original_section_vrom, replacement.original_vram);

        if (to_replace == nullptr) {
            std::stringstream error_param_stream{};
            error_param_stream << std::hex <<
                "section: 0x" << replacement.original_section_vrom <<
                " func: 0x" << std::setfill('0') << std::setw(8) << replacement.original_vram;
            error_param = error_param_stream.str();
            return CodeModLoadError::InvalidFunctionReplacement;
        }

        // Reject replacing a function the base recomp already patched, unless the replacement is marked Force.
        if ((replacement.flags & N64Recomp::ReplacementFlags::Force) == N64Recomp::ReplacementFlags(0)) {
            auto find_it = base_patched_funcs.find(to_replace);
            if (find_it != base_patched_funcs.end()) {
                std::stringstream error_param_stream{};
                error_param_stream << std::hex <<
                    "section: 0x" << replacement.original_section_vrom <<
                    " func: 0x" << std::setfill('0') << std::setw(8) << replacement.original_vram;
                error_param = error_param_stream.str();
                return CodeModLoadError::BaseRecompConflict;
            }
        }

        // Reject a second mod trying to replace the same function.
        auto find_patch_it = patched_funcs.find(to_replace);
        if (find_patch_it != patched_funcs.end()) {
            error_param = find_patch_it->second.mod_id;
            return CodeModLoadError::ModConflict;
        }

        // Stash the original bytes so the patch can be reverted when the mod unloads.
        PatchData& cur_replacement_data = patched_funcs[to_replace];
        memcpy(cur_replacement_data.replaced_bytes.data(), reinterpret_cast<void*>(to_replace), cur_replacement_data.replaced_bytes.size());
        cur_replacement_data.mod_id = mod.manifest.mod_id;

        // Overwrite the original with a trampoline to the replacement.
        patch_func(to_replace, mod.code_handle->get_function_handle(replacement.func_index));
    }

    return CodeModLoadError::Good;
}

void recomp::mods::ModContext::unload_mods() {
    for (auto& [replacement_func, replacement_data] : patched_funcs) {
        unpatch_func(reinterpret_cast<void*>(replacement_func), replacement_data);
    }
    patched_funcs.clear();
    loaded_mods_by_id.clear();
    hook_slots.clear();
    processed_hook_slots.clear();
    shim_functions.clear();
    recomp::mods::reset_events();
    recomp::mods::reset_hooks();
    num_events = recomp::overlays::num_base_events();
    active_game = (size_t)-1;
}

void recomp::mods::unmet_dependency_handler(uint8_t* rdram, recomp_context* ctx, uintptr_t arg) {
    // Undo the packing done when the shim was created: low word is the mod index,
    // high word is the import index.
    size_t caller_mod_index = (arg >>  0) & uint64_t(0xFFFFFFFF);
    size_t import_index =     (arg >> 32) & uint64_t(0xFFFFFFFF);

    std::string mod_name = recomp::mods::get_mod_display_name(caller_mod_index);
    std::pair<std::string, std::string> import_info = recomp::mods::get_mod_import_info(caller_mod_index, import_index);
    
    ultramodern::error_handling::message_box(
        (
            "Fatal error in mod \"" + mod_name + "\": Called function \"" + import_info.second + "\" in unmet optional dependency \"" + import_info.first + "\".\n"
        ).c_str()
    );
    ULTRAMODERN_QUICK_EXIT();
}
