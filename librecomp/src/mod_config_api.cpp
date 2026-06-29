#include "librecomp/mods.hpp"
#include "librecomp/helpers.hpp"
#include "librecomp/addresses.hpp"

// Native side of the per-mod configuration / metadata API that recompiled
// mods import. Each entry point marshals between the guest's argument
// registers and the host-side mod tables, so the bodies are deliberately
// thin; the behavioral contract (type coercion, the heap-string return
// convention, the registered export names) is what must stay fixed.

namespace {

// Copy a host string into a fresh guest-heap allocation and leave its KSEG0
// address in the return register. The block is rounded up to 16 bytes and
// always NUL-terminated.
template <typename StringType>
void return_guest_string(uint8_t* rdram, recomp_context* ctx, const StringType& text) {
    const size_t with_terminator = text.size() + 1;
    const size_t block_size = (with_terminator + 15) & ~size_t{15};

    uint8_t* host_block = reinterpret_cast<uint8_t*>(recomp::alloc(rdram, block_size));
    const gpr guest_addr = static_cast<gpr>(host_block - rdram) + 0xFFFFFFFF80000000ULL;

    for (size_t i = 0; i < text.size(); i++) {
        MEM_B(i, guest_addr) = text[i];
    }
    MEM_B(text.size(), guest_addr) = 0;

    ctx->r2 = guest_addr;
}

} // namespace

void recomp_get_config_u32(uint8_t* rdram, recomp_context* ctx, size_t mod_index) {
    recomp::mods::ConfigValueVariant value =
        recomp::mods::get_mod_config_value(mod_index, _arg_string<0>(rdram, ctx));

    if (const uint32_t* stored = std::get_if<uint32_t>(&value)) {
        _return(ctx, *stored);
    }
    else if (const double* stored = std::get_if<double>(&value)) {
        // Narrow a floating-point setting to a signed integer first so the
        // truncation matches what the caller's int conversion expects.
        _return(ctx, static_cast<uint32_t>(static_cast<int32_t>(*stored)));
    }
    else {
        _return(ctx, uint32_t{0});
    }
}

void recomp_get_config_double(uint8_t* rdram, recomp_context* ctx, size_t mod_index) {
    recomp::mods::ConfigValueVariant value =
        recomp::mods::get_mod_config_value(mod_index, _arg_string<0>(rdram, ctx));

    if (const uint32_t* stored = std::get_if<uint32_t>(&value)) {
        ctx->f0.d = static_cast<double>(*stored);
    }
    else if (const double* stored = std::get_if<double>(&value)) {
        ctx->f0.d = *stored;
    }
    else {
        ctx->f0.d = 0.0;
    }
}

void recomp_get_config_string(uint8_t* rdram, recomp_context* ctx, size_t mod_index) {
    recomp::mods::ConfigValueVariant value =
        recomp::mods::get_mod_config_value(mod_index, _arg_string<0>(rdram, ctx));

    if (const std::string* stored = std::get_if<std::string>(&value)) {
        return_guest_string(rdram, ctx, *stored);
    }
    else {
        _return(ctx, NULLPTR);
    }
}

void recomp_free_config_string(uint8_t* rdram, recomp_context* ctx) {
    const gpr guest_addr = (gpr)_arg<0, PTR(char)>(rdram, ctx);
    // Reverse the KSEG0 translation used when the string was handed out.
    recomp::free(rdram, rdram + (guest_addr - 0xFFFFFFFF80000000ULL));
}

void recomp_get_mod_version(uint8_t* rdram, recomp_context* ctx, size_t mod_index) {
    const recomp::Version version = recomp::mods::get_mod_version(mod_index);

    *_arg<0, uint32_t*>(rdram, ctx) = version.major;
    *_arg<1, uint32_t*>(rdram, ctx) = version.minor;
    *_arg<2, uint32_t*>(rdram, ctx) = version.patch;
}

void recomp_change_save_file(uint8_t* rdram, recomp_context* ctx, size_t mod_index) {
    const std::string requested_name = _arg_string<0>(rdram, ctx);
    const std::string owning_mod_id = recomp::mods::get_mod_id(mod_index);

    const auto as_u8 = [](const std::string& s) {
        return std::u8string{reinterpret_cast<const char8_t*>(s.data()), s.size()};
    };

    ultramodern::change_save_file(as_u8(owning_mod_id), as_u8(requested_name));
}

void recomp_get_save_file_path(uint8_t* rdram, recomp_context* ctx) {
    const std::filesystem::path path = ultramodern::get_save_file_path();
    return_guest_string(rdram, ctx, std::filesystem::absolute(path).u8string());
}

void recomp_get_mod_folder_path(uint8_t* rdram, recomp_context* ctx) {
    const std::filesystem::path path = recomp::mods::get_mods_directory();
    return_guest_string(rdram, ctx, std::filesystem::absolute(path).u8string());
}

void recomp_get_mod_file_path(uint8_t* rdram, recomp_context* ctx, size_t mod_index) {
    const std::filesystem::path path = recomp::mods::get_mod_path(mod_index);
    return_guest_string(rdram, ctx, std::filesystem::absolute(path).u8string());
}

void recomp_is_dependency_met(uint8_t* rdram, recomp_context* ctx, size_t mod_index) {
    const std::string dependency_id = _arg_string<0>(rdram, ctx);
    const recomp::mods::DependencyStatus status =
        recomp::mods::is_dependency_met(mod_index, dependency_id);
    _return(ctx, static_cast<uint32_t>(status));
}

void recomp::mods::register_config_exports() {
    recomp::overlays::register_ext_base_export("recomp_get_config_u32", recomp_get_config_u32);
    recomp::overlays::register_ext_base_export("recomp_get_config_double", recomp_get_config_double);
    recomp::overlays::register_ext_base_export("recomp_get_config_string", recomp_get_config_string);
    recomp::overlays::register_base_export("recomp_free_config_string", recomp_free_config_string);
    recomp::overlays::register_ext_base_export("recomp_get_mod_version", recomp_get_mod_version);
    recomp::overlays::register_ext_base_export("recomp_change_save_file", recomp_change_save_file);
    recomp::overlays::register_base_export("recomp_get_save_file_path", recomp_get_save_file_path);
    recomp::overlays::register_base_export("recomp_get_mod_folder_path", recomp_get_mod_folder_path);
    recomp::overlays::register_ext_base_export("recomp_get_mod_file_path", recomp_get_mod_file_path);
    recomp::overlays::register_ext_base_export("recomp_is_dependency_met", recomp_is_dependency_met);
}
