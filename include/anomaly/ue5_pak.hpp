#pragma once

#include "anomaly/symbol_resolver.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace anomaly {

// Runtime pak mounting for a cooked UE5 build.
//
// UE5.6 mounts a container with one call: FPakPlatformFile::Mount opens the .pak, derives the
// IoStore container path from it (.utoc/.ucas), mounts that through the file dispatcher backend,
// and registers the container header with the package store. Unmounting reverses the same steps.
// Driving the engine's own members keeps the pak list, the read-order sort, the container registry
// and the mount notifications inside the engine instead of duplicating them here.
//
// The instance those members are called on is the engine's "PakFile" platform file. It is reached
// through the platform file chain: FPlatformFileManager::Get() -> the manager singleton -> its
// topmost platform file -> GetLowerLevel()/GetName() until the name is "PakFile". The chain is
// walked with the IPlatformFile vtable, so the manager's topmost offset and both vtable offsets are
// Profile layout values rather than literals in this file.
inline constexpr std::string_view kUe5PakFeature = "ue5.pak";
inline constexpr std::string_view kUe5PakValidator = "ue5-pak-mount-v1";
inline constexpr std::string_view kUe5PakPlatformFileManagerSymbol =
    "ue5.FPlatformFileManager.Get";
inline constexpr std::string_view kUe5PakMountSymbol = "ue5.FPakPlatformFile.Mount";
inline constexpr std::string_view kUe5PakMountWithStatusSymbol = "ue5.FPakPlatformFile.MountWithStatus";
inline constexpr std::string_view kUe5PakUnmountSymbol = "ue5.FPakPlatformFile.Unmount";
inline constexpr std::string_view kUe5PakReleaseSymbol = "ue5.FPakFile.Release";
inline constexpr std::string_view kUe5PakCacheLockKey = "pak.cacheLockOffset";
// The game adds its own container gate that rejects an unsigned pak with
// `SignatureError "Missing signature"`; the symbol resolves to the single conditional byte that
// decides whether that branch is taken.
inline constexpr std::string_view kUe5PakSignatureGateSymbol = "ue5.PakSignatureGate";
// The game's own container-signature verifier. It decrypts and compares a TOC/block signature that
// a mod container does not carry, and reports `SignatureError` before the container is registered.
inline constexpr std::string_view kUe5PakSignatureVerifySymbol = "ue5.PakContainerSignatureVerify";
// FPakFile::Find(const FString& FullPath, FPakEntry* OutEntry): asks ONE container whether it owns a
// path. The platform file's directory iteration merges every mounted container and the loose file
// system, so this is what turns a name into "ours" or "not ours".
inline constexpr std::string_view kUe5PakFindSymbol = "ue5.FPakFile.Find";
// The block-cache verifier the game schedules for cache reads. It dereferences a per-container table
// with no null check, so a container mounted after start-up without a .sig signature table takes the
// process down on the first verified read of its element.
inline constexpr std::string_view kUe5PakCacheVerifySymbol = "ue5.PakBlockCacheVerify";
// The completion half of that verifier: it takes the same four arguments, locks the cache, adopts the
// request's data buffer into the slice slot, queues the request and decrements the cache's in-flight count.
// The verifier reaches it as its own tail (`return sub_...`), so a detour that skips the verification
// has to run it or the request is never completed and the caller waits forever.
inline constexpr std::string_view kUe5PakCacheVerifyCompleteSymbol = "ue5.PakBlockCacheComplete";
inline constexpr std::string_view kUe5PakManagerTopmostKey = "pak.managerTopmostOffset";
inline constexpr std::string_view kUe5PakLowerLevelKey = "pak.getLowerLevelVtableOffset";
inline constexpr std::string_view kUe5PakNameKey = "pak.getNameVtableOffset";
inline constexpr std::string_view kUe5PakSignedFlagKey = "pak.signedFlagOffset";
// Native output-entry layout and the optional diagnostic read layout.
inline constexpr std::string_view kUe5PakListEntryKey = "pak.listEntryPakFileOffset";
inline constexpr std::string_view kUe5PakMountedListKey = "pak.mountedListOffset";
inline constexpr std::string_view kUe5PakMountedLockKey = "pak.mountedListLockOffset";
inline constexpr std::string_view kUe5PakFilenameKey = "pak.pakFileFilenameOffset";
inline constexpr std::string_view kUe5PakMountPointKey = "pak.pakFileMountPointOffset";
inline constexpr std::string_view kUe5PakOpenReadKey = "pak.openReadVtableOffset";
inline constexpr std::string_view kUe5PakIterateDirectoryKey = "pak.iterateDirectoryVtableOffset";
inline constexpr std::string_view kUe5PakHandleReadKey = "pak.fileHandleReadVtableOffset";
inline constexpr std::string_view kUe5PakHandleSizeKey = "pak.fileHandleSizeVtableOffset";
inline constexpr std::string_view kUe5PakHandlePakFileKey = "pak.fileHandlePakFileOffset";
inline constexpr std::string_view kUe5PakHandleDestroyKey = "pak.fileHandleDestroyVtableOffset";
// Where the verifier keeps its records and per-container elements. The verifier itself uses these
// offsets to turn a request into the element it dereferences, so the detour reads the same fields.
inline constexpr std::string_view kUe5PakCacheSliceStrideKey = "pak.cacheSliceStride";
inline constexpr std::string_view kUe5PakCacheRecordIndexKey = "pak.cacheRecordIndexOffset";
inline constexpr std::string_view kUe5PakCacheOwnerMaskKey = "pak.cacheOwnerMaskOffset";
inline constexpr std::string_view kUe5PakCacheRecordsKey = "pak.cacheRecordsOffset";
inline constexpr std::string_view kUe5PakRecordStrideKey = "pak.recordStride";
inline constexpr std::string_view kUe5PakCacheElementsKey = "pak.cacheElementsOffset";
inline constexpr std::string_view kUe5PakElementStrideKey = "pak.elementStride";
inline constexpr std::string_view kUe5PakElementTableKey = "pak.elementSignatureTableOffset";
// Inside an element, +0x00 is the handle the builder opened for the container and +0x08 the container
// pointer itself (the builder stores its own container argument there), which is what names the mod.
inline constexpr std::string_view kUe5PakElementHandleKey = "pak.elementHandleOffset";
inline constexpr std::string_view kUe5PakElementContainerKey = "pak.elementContainerOffset";

// Optional diagnostic read layout. A read cannot create a missing .sig signature table.
struct Ue5PakPrimeLayout {
    // FPakFile::Find(const FString& FullPath, FPakEntry* OutEntry): the container's own answer to
    // "do you hold this path". Names the platform file enumerated belong to whichever container or
    // loose file the mount point reaches, so this is the only reliable way to stay on our container.
    std::uintptr_t find{};
    // FPakFile::MountPoint is an FString, whose first member is the character buffer; the directory
    // to enumerate is read as that pointer.
    std::uint32_t pak_file_mount_point_offset{};
    // IPlatformFile::OpenRead(const TCHAR* Filename, bool bAllowWrite).
    std::uint32_t open_read_vtable_offset{};
    // IPlatformFile::IterateDirectory(const TCHAR* Directory, FDirectoryVisitor& Visitor).
    std::uint32_t iterate_directory_vtable_offset{};
    // IFileHandle::Read(uint8* Destination, int64 BytesToRead).
    std::uint32_t file_handle_read_vtable_offset{};
    // IFileHandle::Size(): the byte count the read path checks a request against, and the one number
    // that says whether a container's index entry actually describes data in this container.
    std::uint32_t file_handle_size_vtable_offset{};
    // The container the open resolved to, sitting in the handle the platform file handed out. A
    // successfully read handle that belongs to another container primes that container, not ours.
    std::uint32_t file_handle_pak_file_offset{};
    // IFileHandle's scalar deleting destructor, the only way to release a handle the engine gave
    // out as a raw pointer.
    std::uint32_t file_handle_destroy_vtable_offset{};
    // False when the Profile does not declare this block. Mounting still works; the mount reports
    // that its container was not primed instead of guessing offsets.
    bool configured{};
};

// Offsets the block-cache verifier itself uses to turn one request into the per-container element it
// dereferences. The detour re-reads the same fields, so they belong to the Profile like every other
// game layout value.
struct Ue5PakCacheVerifyLayout {
    std::uint32_t cache_lock_offset{};
    // Per-slice request slot: slice = cache + cache_slice_stride * request_index.
    std::uint32_t cache_slice_stride{};
    // Field inside that slice holding the record position, masked by the cache's owner mask.
    std::uint32_t cache_record_index_offset{};
    // The owner mask stored in the cache object.
    std::uint32_t cache_owner_mask_offset{};
    // Records array base, one record per lookup, and its stride.
    std::uint32_t cache_records_offset{};
    std::uint32_t record_stride{};
    // Per-container element array base, its stride, and where the signature/block table sits inside
    // an element. That table is what the verifier dereferences without a null check.
    std::uint32_t cache_elements_offset{};
    std::uint32_t element_stride{};
    std::uint32_t element_signature_table_offset{};
    // The element's own identity: the handle the builder opened for the container, and the container
    // pointer the builder stored as the element's key. The container pointer is what names the mod.
    std::uint32_t element_handle_offset{};
    std::uint32_t element_container_offset{};
    bool configured{};
};

// One attribution sample taken from the verifier's arguments before it touches anything. Reading it is
// guarded: a stale argument must produce a log line, never a fault.
struct Ue5PakCacheVerifySample final {
    // The cache object the verifier was called on.
    std::uintptr_t cache{};
    // The 48-byte record the verifier would have used (0 when it could not be read).
    std::uint64_t record{};
    // Bits 48..63 of the record: the per-container element index.
    std::uint32_t index{};
    // Bits 16..47 of the record: the per-container tag the game logs with it.
    std::uint32_t tag{};
    // The fourth argument the trampoline forwarded (the cache slice the request belongs to).
    std::uint32_t request{};
    // 0 = the per-container table is null, 1 = set, 2 = the field could not be read at all.
    std::uint8_t element{};
    // The element the verifier would have used. Logged for the null case, which is the crash case.
    std::uintptr_t element_pointer{};
    // The container that owns the element (the element's key) and the handle the builder opened for it.
    // The container pointer is what ties a request back to one mounted mod.
    std::uintptr_t container{};
    std::uintptr_t handle{};
};

// Called from the verifier's own thread for every request, before the original body runs.
using Ue5PakCacheVerifySink =
    void(__fastcall*)(void* context, const Ue5PakCacheVerifySample& sample) noexcept;

enum class Ue5PakCacheVerifyStatus : std::uint8_t {
    Detoured,
    AlreadyInstalled,
    // The symbol or its layout is missing, so nothing was installed and nothing was logged.
    Unavailable,
    Unreadable,
};

// Installs the diagnostic detour on the verifier entry. It reports each request's attribution to
// `sink` and routes the call: a container whose element has a table keeps the engine's whole body,
// one whose element has none gets only the completion half (`complete`), which is what keeps the read
// alive without hanging the caller. Removal requires disabling and draining the hook.
Ue5PakCacheVerifyStatus InstallUe5PakBlockCacheVerifyDetour(const Ue5PakCacheVerifyLayout& layout,
    std::uintptr_t verify, std::uintptr_t complete, Ue5PakCacheVerifySink sink, void* context,
    std::uintptr_t* target = nullptr) noexcept;
void RemoveUe5PakBlockCacheVerifyDetour() noexcept;
// Engine I/O queued before an unmount may still enter the detour. A resident Core can detach
// the session-owned sink while retaining the process-owned routing hook for those requests.
void DetachUe5PakBlockCacheVerifySink() noexcept;

// One priming attempt's verdict. `stage` is a stable id for grepping ("ok" or the step that
// stopped it), and `detail` carries the path form that was tried, the path itself and the numbers
// the engine reported (entry size, raw return values), so a single launch localises the break.
struct Ue5PakPrimeReport {
    bool primed{};
    std::string stage{"unavailable"};
    std::string detail;
};

// Bookkeeping for the priming walk, kept here because it is the one part of the pass with a
// contract worth testing without a game: a name may only occupy a file slot once the container's
// own index claimed it. Names that belong to another container or to the loose file system must
// cost nothing, otherwise a shared mount point lets them fill the budget before the container's own
// entries are ever reached.
struct Ue5PakPrimeTally {
    // Bounded work: a container that cannot be read must not turn into an unbounded probe.
    static constexpr std::size_t kMaximumFiles = 32;
    static constexpr std::size_t kMaximumAttempts = 24;

    // Names the container claimed, names it refused, and lookups that could not be made at all.
    std::size_t owned{};
    std::size_t foreign{};
    std::size_t lookup_failed{};
    // Candidate paths the walk kept, and the opens spent on them.
    std::size_t slots{};
    std::size_t attempts{};
    // Owned candidates whose entry reported a size, i.e. the ones the read path can serve.
    std::size_t readable{};

    // `verdict` is FPakFile::EFindResult: 0 NotFound, 1 Found, 2 FoundDeleted. Returns true when the
    // entry takes one of the candidate slots - only "Found" ever can.
    [[nodiscard]] bool AdmitFile(std::uint8_t verdict) noexcept {
        if (verdict != 1) {
            if (verdict == 0 || verdict == 2) {
                ++foreign;
            } else {
                ++lookup_failed;
            }
            return false;
        }
        ++owned;
        if (slots >= kMaximumFiles) return false;
        ++slots;
        return true;
    }
    [[nodiscard]] bool FilesExhausted() const noexcept { return slots >= kMaximumFiles; }
    [[nodiscard]] bool TakeAttempt() noexcept {
        if (attempts >= kMaximumAttempts) return false;
        ++attempts;
        return true;
    }
    [[nodiscard]] bool AttemptsExhausted() const noexcept {
        return attempts >= kMaximumAttempts;
    }
};

// Resolved entry points plus the platform-file chain layout the walk needs. Every field comes from
// the active Profile; none of them is a layout this module guesses.
struct Ue5PakBinding {
    std::uintptr_t platform_file_manager{};
    std::uintptr_t mount{};
    // Mount(const FPakMountArgs&, FIoStatus*, FPakListEntry*) — the same entry the five-argument
    // overload forwards to. It reports the engine's own refusal reason, so a failed mount is
    // explainable instead of just "rejected".
    std::uintptr_t mount_with_status{};
    std::uintptr_t unmount{};
    std::uintptr_t release{};
    // Optional: the byte that turns the game's "Missing signature" rejection into an accept.
    std::uintptr_t signature_gate{};
    // Optional: the verifier that must report success for an unsigned container.
    std::uintptr_t signature_verify{};
    // Optional: the block-cache verifier entry the diagnostic detour attaches to.
    std::uintptr_t cache_verify{};
    // Optional: the completion half of that verifier, which the detour runs when it skips verification.
    std::uintptr_t cache_verify_complete{};
    std::uint32_t manager_topmost_offset{};
    std::uint32_t lower_level_vtable_offset{};
    std::uint32_t name_vtable_offset{};
    // Offset of FPakPlatformFile::bSigned inside the instance. Mount() copies it into every
    // FPakFile it creates, and a cleared flag is what makes an unsigned container acceptable.
    std::uint32_t signed_flag_offset{};
    // Optional, and only used after a successful mount: the layout of the priming read.
    // FPakListEntry output ABI; validated before mounting.
    std::uint32_t list_entry_pak_file_offset{};
    std::uint32_t mounted_list_offset{};
    std::uint32_t mounted_list_lock_offset{};
    std::uint32_t pak_file_filename_offset{};
    Ue5PakPrimeLayout prime;
    // Optional: the layout the block-cache verifier detour reads its attribution from.
    Ue5PakCacheVerifyLayout cache_verify_layout;

    [[nodiscard]] bool Complete() const noexcept {
        return platform_file_manager != 0 && mount != 0 && mount_with_status != 0 &&
            unmount != 0 && release != 0 &&
            manager_topmost_offset % sizeof(void*) == 0 &&
            lower_level_vtable_offset % sizeof(void*) == 0 &&
            name_vtable_offset % sizeof(void*) == 0 && signed_flag_offset != 0 &&
            mounted_list_offset != 0 && mounted_list_lock_offset != 0 && pak_file_filename_offset != 0;
    }
};

struct Ue5PakMountRequest {
    // Absolute path of the .pak. The engine derives the IoStore container from the same path, so a
    // cooked mod ships "<name>.pak" next to "<name>.utoc"/"<name>.ucas".
    std::wstring pak_file;
    // Read order; a higher value wins for duplicate entries. The game's own paks land at 0..4
    // (higher for patch paks), so a mod needs a clearly larger order to override content.
    std::uint32_t order{};
    // Empty lets the engine derive the mount point from the container contents.
    std::wstring mount_point;
    // A mod that ships its files in the .pak has no IoStore chunks to register.
    bool pak_only{};
};
// Registers "ue5-pak-mount-v1" with the Profile feature validator registry.
void RegisterUe5PakValidator(FeatureLayoutValidatorRegistry& validators);

// Reads the three resolved symbols and the chain layout out of a completed resolution. Returns
// false and fills `error` when the feature is not available for this build, so callers degrade
// instead of probing addresses.
bool BindUe5Pak(const BuildProfile& profile, const ProfileResolutionSnapshot& resolution,
    Ue5PakBinding& binding, std::string& error, const SymbolMemory* memory = nullptr);

// Walks the platform file chain and returns the engine's FPakPlatformFile instance. Returns 0
// while the engine has not built the chain yet, when no layer reports the name "PakFile", or when
// an entry does not look like a readable platform file. Callers retry instead of caching a zero.
std::uintptr_t FindUe5PakPlatformFile(
    const Ue5PakBinding& binding, const SymbolMemory& memory) noexcept;

// Outcome of one mount attempt. `error_code` is the engine's EIoErrorCode (0 = Ok) and
// `error_message` is the engine's own text ("utoc not found", "Pak is invalid", ...), so a rejected
// container can be explained instead of just reported as failed.
struct Ue5PakMountResult {
    bool mounted{};
    std::int32_t error_code{-1};
    std::wstring error_message;
    // Strong reference transferred by the native output entry; caller must release it.
    std::uintptr_t container{};
};

struct Ue5MountedPakFile {
    std::wstring filename;
    // Identity only; this snapshot does not transfer a reference or permit dereferencing it.
    std::uintptr_t container{};
};
// Copies filenames under the engine's PakListCritical lock, including startup mounts.
std::optional<std::vector<Ue5MountedPakFile>> MountedUe5PakFiles(
    const Ue5PakBinding& binding, std::uintptr_t pak_platform_file, const SymbolMemory& memory);

// Native calls have an SEH boundary; a fault can still leave engine-side partial changes.
Ue5PakMountResult MountUe5Pak(const Ue5PakBinding& binding, std::uintptr_t pak_platform_file,
    const Ue5PakMountRequest& request) noexcept;
bool UnmountUe5Pak(const Ue5PakBinding& binding, std::uintptr_t pak_platform_file,
    const std::wstring& pak_file) noexcept;
// Drops the strong reference transferred through Mount's FPakListEntry output.
void ReleaseUe5PakContainer(const Ue5PakBinding& binding, std::uintptr_t container) noexcept;

// Re-runs the priming read for an already mounted container: enumerate one readable path inside it,
// open it through the platform file, read one block, release the handle. Fault-guarded and
// diagnostic only; it never creates a signature table, unmounts or mounts anything.
Ue5PakPrimeReport PrimeUe5PakContainer(const Ue5PakBinding& binding,
    std::uintptr_t pak_platform_file, std::uintptr_t container) noexcept;

// Clears FPakPlatformFile::bSigned on the live instance, which is what FPakFile::CreatePakReader
// turns into "this container needs a signature". One byte instead of a per-build pattern scan:
// Mount() reads the flag for every container it opens, so containers mounted after this call are
// accepted without a .sig, while the game's own already-mounted paks keep whatever they were
// verified with. The write is only performed when the current byte is a plausible bool.
bool ClearUe5PakSignatureRequirement(const Ue5PakBinding& binding,
    std::uintptr_t pak_platform_file, const SymbolMemory& memory) noexcept;

// Turns the game's own container gate into an unconditional accept: the instruction there is
// `test bl, 4 / jnz accept`, and flipping the JNZ to a JMP keeps unsigned containers mountable. The
// byte is only patched when it is exactly that conditional jump, so a wrong Profile cannot corrupt
// unrelated code. Returns false when the gate is unavailable or already patched.
enum class Ue5PakGateStatus : std::uint8_t {
    NoSymbol,
    Unreadable,
    UnexpectedByte,
    Patched,
    AlreadyPatched,
};

// Turns the game's own container gate into an unconditional accept: the instruction there is
// `test bl, 4 / jnz accept`, and flipping the JNZ to a JMP keeps unsigned containers mountable. The
// byte is only patched when it is exactly that conditional jump, so a wrong Profile cannot corrupt
// unrelated code. `observed` receives what was actually read, which is what makes a refusal
// diagnosable.
Ue5PakGateStatus DisableUe5PakSignatureGate(
    const Ue5PakBinding& binding, std::uint8_t* observed = nullptr) noexcept;

// Detours the container-signature verifier so it reports "verified" without decrypting anything.
// The verifier's only product is an FIoStatus, so replacing it cannot affect how a container is
// read. The hook lives for the rest of the process: it is installed once and never removed while
// the game can still mount containers.
bool InstallUe5PakSignatureVerificationBypass(std::uintptr_t verifier) noexcept;
void RemoveUe5PakSignatureVerificationBypass() noexcept;

}  // namespace anomaly
