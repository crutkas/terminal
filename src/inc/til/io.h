// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <aclapi.h>
#include <sddl.h>
#include <winternl.h>
#include <wil/token_helpers.h>

namespace til // Terminal Implementation Library. Also: "Today I Learned"
{
    namespace io
    {
        namespace details
        {
            inline constexpr std::string_view Utf8Bom{ "\xEF\xBB\xBF", 3 };

            // Function Description:
            // - Checks the permissions on this file, to make sure it can only be opened
            //   for writing by admins. We will be checking to see if the file is owned
            //   by the Builtin\Administrators group. If it's not, then it was likely
            //   tampered with.
            // Arguments:
            // - handle: a HANDLE to the file to check
            // Return Value:
            // - true if it had the expected permissions; otherwise, false.
            _TIL_INLINEPREFIX bool isOwnedByAdministrators(const HANDLE& handle)
            {
                // If the file is owned by the administrators group, trust the
                // administrators instead of checking the DACL permissions. It's simpler
                // and more flexible.

                wil::unique_hlocal_security_descriptor sd;
                PSID psidOwner{ nullptr };
                // The psidOwner pointer references the security descriptor, so it
                // doesn't have to be freed separate from sd.
                const auto status = GetSecurityInfo(handle,
                                                    SE_FILE_OBJECT,
                                                    OWNER_SECURITY_INFORMATION,
                                                    &psidOwner,
                                                    nullptr,
                                                    nullptr,
                                                    nullptr,
                                                    wil::out_param_ptr<PSECURITY_DESCRIPTOR*>(sd));
                THROW_IF_WIN32_ERROR(status);

                wil::unique_any_psid psidAdmins{ nullptr };
                THROW_IF_WIN32_BOOL_FALSE(
                    ConvertStringSidToSidW(L"BA", wil::out_param_ptr<PSID*>(psidAdmins)));

                return EqualSid(psidOwner, psidAdmins.get());
            }
        } // details

        // Tries to read a file somewhat atomically without locking it.
        // Returns an empty string if the file couldn't be opened.
        _TIL_INLINEPREFIX std::string read_file_as_utf8_string_if_exists(const std::filesystem::path& path, const bool elevatedOnly = false, FILETIME* lastWriteTime = nullptr)
        {
            // From some casual observations we can determine that:
            // * ReadFile() always returns the requested amount of data (unless the file is smaller)
            // * It's unlikely that the file was changed between GetFileSize() and ReadFile()
            // -> Lets add a retry-loop just in case, to not fail if the file size changed while reading.
            for (auto i = 0; i < 3; ++i)
            {
                wil::unique_hfile file{ CreateFileW(path.c_str(),
                                                    GENERIC_READ,
                                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                    nullptr,
                                                    OPEN_EXISTING,
                                                    FILE_ATTRIBUTE_NORMAL,
                                                    nullptr) };

                if (!file)
                {
                    const auto gle = GetLastError();
                    if (gle == ERROR_FILE_NOT_FOUND)
                    {
                        return {};
                    }
                    THROW_WIN32(gle);
                }

                // Open the file _first_, then check if it has the right
                // permissions. This prevents a "Time-of-check to time-of-use"
                // vulnerability where a malicious exe could delete the file and
                // replace it between us checking the permissions, and reading the
                // contents. We've got a handle to the file now, which means we're
                // going to read the contents of that instance of the file
                // regardless. If someone replaces the file on us before we get to
                // the GetSecurityInfo call below, then only the subsequent call to
                // read_file_as_utf8_string will notice it.
                if (elevatedOnly)
                {
                    const auto hadExpectedPermissions{ details::isOwnedByAdministrators(file.get()) };
                    if (!hadExpectedPermissions)
                    {
                        // Close the handle
                        file.reset();

                        // delete the file. It's been compromised.
                        LOG_LAST_ERROR_IF(!DeleteFile(path.c_str()));

                        // Exit early, because obviously there's nothing to read from the deleted file.
                        return {};
                    }
                }

                const auto fileSize = GetFileSize(file.get(), nullptr);
                THROW_LAST_ERROR_IF(fileSize == INVALID_FILE_SIZE);

                // By making our buffer just slightly larger we can detect if
                // the file size changed and we've failed to read the full file.
                std::string buffer(static_cast<size_t>(fileSize) + 1, '\0');
                DWORD bytesRead = 0;
                THROW_IF_WIN32_BOOL_FALSE(ReadFile(file.get(), buffer.data(), gsl::narrow<DWORD>(buffer.size()), &bytesRead, nullptr));

                // This implementation isn't atomic as we'd need to use an exclusive file lock.
                // But this would be annoying for users as it forces them to close the file in their editor.
                // The next best alternative is to at least try to detect file changes and retry the read.
                if (bytesRead != fileSize)
                {
                    // This continue is unlikely to be hit (see the prior for loop comment).
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }

                // As mentioned before our buffer was allocated oversized.
                buffer.resize(bytesRead);

                if (til::starts_with(buffer, details::Utf8Bom))
                {
                    // Yeah this memmove()s the entire content.
                    // But I don't really want to deal with UTF8 BOMs any more than necessary,
                    // as basically not a single editor writes a BOM for UTF8.
                    buffer.erase(0, details::Utf8Bom.size());
                }

                if (lastWriteTime)
                {
                    THROW_IF_WIN32_BOOL_FALSE(GetFileTime(file.get(), nullptr, nullptr, lastWriteTime));
                }

                return buffer;
            }

            THROW_WIN32_MSG(ERROR_READ_FAULT, "file size changed while reading");
        }

        _TIL_INLINEPREFIX void write_utf8_string_to_file(const std::filesystem::path& path, const std::string_view& content, const bool elevatedOnly = false, FILETIME* lastWriteTime = nullptr)
        {
            SECURITY_ATTRIBUTES sa;
            // stash the security descriptor here, so it will stay in context until
            // after the call to CreateFile. If it gets cleaned up before that, then
            // CreateFile will fail
            wil::unique_hlocal_security_descriptor sd;
            if (elevatedOnly)
            {
                // Initialize the security descriptor so only admins can write the
                // file. We'll initialize the SECURITY_DESCRIPTOR with a
                // single entry (ACE) -- a mandatory label (i.e. a
                // LABEL_SECURITY_INFORMATION) that sets the file integrity level to
                // "high",  with a no-write-up policy.
                //
                // When accessed from a security context at a lower integrity level,
                // the no-write-up policy filters out rights that aren't in the
                // object type's generic read and execute set (for the file type,
                // that's FILE_GENERIC_READ | FILE_GENERIC_EXECUTE).
                //
                // Another option we considered here was manually setting the ACLs
                // on this file such that Builtin\Admins could read&write the file,
                // and all users could only read.
                //
                // Big thanks to @eryksun in GH#11222 for helping with this. This
                // alternative method was chosen because it's considerably simpler.

                // The required security descriptor can be created easily from the
                // SDDL string: "S:(ML;;NW;;;HI)"
                // (i.e. SACL:mandatory label;;no write up;;;high integrity level)
                unsigned long cb;
                THROW_IF_WIN32_BOOL_FALSE(
                    ConvertStringSecurityDescriptorToSecurityDescriptor(L"S:(ML;;NW;;;HI)",
                                                                        SDDL_REVISION_1,
                                                                        wil::out_param_ptr<PSECURITY_DESCRIPTOR*>(sd),
                                                                        &cb));

                // Initialize a security attributes structure.
                sa.nLength = sizeof(SECURITY_ATTRIBUTES);
                sa.lpSecurityDescriptor = sd.get();
                sa.bInheritHandle = false;

                // If we're running in an elevated context, when this file is
                // created, it will automatically be owned by
                // Builtin\Administrators, which will pass the above
                // isOwnedByAdministrators check.
                //
                // Programs running in an elevated context will be free to write the
                // file, and unelevated processes will be able to read the file. An
                // unelevated process could always delete the file and rename a new
                // file in its place (a la the way `vim.exe` saves files), but if
                // they do that, the new file _won't_ be owned by Administrators,
                // failing the above check.
            }

            wil::unique_hfile file{ CreateFileW(path.c_str(),
                                                GENERIC_WRITE,
                                                FILE_SHARE_READ | FILE_SHARE_DELETE,
                                                elevatedOnly ? &sa : nullptr,
                                                CREATE_ALWAYS,
                                                FILE_ATTRIBUTE_NORMAL,
                                                nullptr) };
            THROW_LAST_ERROR_IF(!file);

            const auto fileSize = gsl::narrow<DWORD>(content.size());
            DWORD bytesWritten = 0;
            THROW_IF_WIN32_BOOL_FALSE(WriteFile(file.get(), content.data(), fileSize, &bytesWritten, nullptr));

            if (bytesWritten != fileSize)
            {
                THROW_WIN32_MSG(ERROR_WRITE_FAULT, "failed to write whole file");
            }

            if (lastWriteTime)
            {
                THROW_IF_WIN32_BOOL_FALSE(GetFileTime(file.get(), nullptr, nullptr, lastWriteTime));
            }
        }

        _TIL_INLINEPREFIX void write_utf8_string_to_file_atomic(const std::filesystem::path& path, const std::string_view& content, FILETIME* lastWriteTime = nullptr)
        {
            // GH#10787: rename() will replace symbolic links themselves and not the path they point at.
            // It's thus important that we first resolve them before generating temporary path.
            std::error_code ec;
            const auto resolvedPath = std::filesystem::is_symlink(path) ? std::filesystem::canonical(path, ec) : path;
            if (ec)
            {
                if (ec.value() != ERROR_FILE_NOT_FOUND)
                {
                    THROW_WIN32_MSG(ec.value(), "failed to compute canonical path");
                }

                // The original file is a symbolic link, but the target doesn't exist.
                // Consider two fall-backs:
                //   * resolve the link manually, which might be less accurate and more prone to race conditions
                //   * write to the file directly, which lets the system resolve the symbolic link but leaves the write non-atomic
                // The latter is chosen, as this is an edge case and our 'atomic' writes are only best-effort.
                write_utf8_string_to_file(path, content, false, lastWriteTime);
                return;
            }

            auto tmpPath = resolvedPath;
            tmpPath += L".tmp";

            // Writing to a file isn't atomic, but...
            write_utf8_string_to_file(tmpPath, content, false, lastWriteTime);

            // renaming one is (supposed to be) atomic.
            // Wait... "supposed to be"!? Well it's technically not always atomic,
            // but it's pretty darn close to it, so... better than nothing.
            std::filesystem::rename(tmpPath, resolvedPath, ec);
            if (ec)
            {
                THROW_WIN32_MSG(ec.value(), "failed to write to file");
            }
        }
    } // io

    // Outcome of read_file_as_bytes. not_found: the path named no existing file.
    // invalid: the request itself was malformed -- an empty path, an unsupported path
    // form, or a byte range that cannot be satisfied. read_error: the file exists but
    // could not be read, because it is not a regular file, sits on the wrong kind of
    // volume, or the read failed. Callers map these onto whatever their own protocol
    // reports; this says only what happened on disk.
    enum class read_file_result : uint8_t
    {
        ok,
        not_found,
        invalid,
        read_error,
    };

    // Reads up to a bounded number of bytes of a LOCAL file into 'out', starting at
    // 'offset'. A caller that has been handed a path by something untrusted -- a
    // terminal client, say -- wants all of the checks below and should not have to
    // remember them, so they live here rather than at any one call site.
    //
    // Security model:
    //  * Only LOCAL, FIXED-drive, drive-absolute files are read. A path that is not of
    //    the form "X:\..." (relative, drive-relative, or rooted) is rejected, as is any
    //    UNC / Win32-device path ("\\server\share", "\\.\dev", "\\?\..."). After the
    //    handle is open the canonical path is re-checked: a UNC final path
    //    ("\\?\UNC\...") or a non-DRIVE_FIXED volume (remote/removable/optical) is
    //    rejected. This blocks SSRF / NTLM-hash theft via "\\attacker\share" and device
    //    access, even when a junction or mapped drive is used to disguise the target.
    //  * The read is hard-bounded to 32 MiB regardless of the caller's 'size', so a
    //    hostile length can never force a large allocation.
    //  * When 'deleteAfter' is requested the file is removed via its OPEN handle
    //    (FILE_DISPOSITION) — never by re-opening a path, closing the TOCTOU window —
    //    and ONLY when that handle's canonical path is under the system temp directory
    //    AND its name contains 'deleteNameMarker'. The marker is the caller's, because
    //    only the caller knows what it named the files it is entitled to remove; an
    //    empty one deletes nothing, so a caller cannot opt out of the check by omission.
    _TIL_INLINEPREFIX read_file_result read_file_as_bytes(const std::wstring_view path, uint64_t offset, uint64_t size, bool deleteAfter, const std::wstring_view deleteNameMarker, std::vector<uint8_t>& out) noexcept
    {
        out.clear();

        // A hard ceiling no caller can raise, so a bad 'size' cannot turn into a bad
        // allocation. It also keeps a single ReadFile within DWORD range.
        constexpr uint64_t maxBytes = 32ull * 1024 * 1024;

        if (path.empty())
        {
            return read_file_result::invalid;
        }

        const auto isSep = [](const wchar_t c) noexcept { return c == L'\\' || c == L'/'; };

        // Reject UNC and device-namespace paths before opening: any path whose first two
        // characters are separators ("\\server\share", "\\.\dev", "\\?\...", "//host/..").
        if (path.size() >= 2 && isSep(path[0]) && isSep(path[1]))
        {
            return read_file_result::invalid;
        }

        // Require a drive-absolute local path ("X:\..."): reject relative and drive-
        // relative paths so a client cannot make the terminal read a file resolved
        // against the terminal's own working directory.
        const auto driveLetter = path[0];
        const auto isAlpha = (driveLetter >= L'a' && driveLetter <= L'z') || (driveLetter >= L'A' && driveLetter <= L'Z');
        if (path.size() < 3 || !isAlpha || path[1] != L':' || !isSep(path[2]))
        {
            return read_file_result::invalid;
        }

        // Defense in depth: reject a non-fixed drive letter BEFORE opening, so a mapped
        // network drive (X: -> \\server\share) can't trigger an outbound SMB/NTLM auth
        // inside CreateFileW. The post-open canonical check still catches junction/symlink
        // redirects to other volumes; this closes the pre-handshake window for the common case.
        const wchar_t driveRoot[]{ driveLetter, L':', L'\\', L'\0' };
        if (GetDriveTypeW(driveRoot) != DRIVE_FIXED)
        {
            return read_file_result::invalid;
        }

        std::wstring pathStr;
        const auto isReservedDosDevice = [](const std::wstring_view candidate) noexcept {
            auto leaf = candidate.substr(candidate.find_last_of(L'\\') + 1);
            while (!leaf.empty() && (leaf.back() == L' ' || leaf.back() == L'.'))
            {
                leaf.remove_suffix(1);
            }
            if (const auto suffix = leaf.find_first_of(L".:"); suffix != std::wstring_view::npos)
            {
                leaf = leaf.substr(0, suffix);
            }
            while (!leaf.empty() && leaf.back() == L' ')
            {
                leaf.remove_suffix(1);
            }
            const auto equalsLeaf = [&](const std::wstring_view value) noexcept {
                return leaf.size() == value.size() &&
                       CompareStringOrdinal(leaf.data(), static_cast<int>(leaf.size()), value.data(), static_cast<int>(value.size()), TRUE) == CSTR_EQUAL;
            };
            const auto isNumberedDevice = [&](const std::wstring_view prefix) noexcept {
                if (leaf.size() != prefix.size() + 1)
                {
                    return false;
                }
                const auto suffix = leaf.back();
                return ((suffix >= L'1' && suffix <= L'9') || suffix == L'\u00B9' || suffix == L'\u00B2' || suffix == L'\u00B3') &&
                       CompareStringOrdinal(leaf.data(), static_cast<int>(prefix.size()), prefix.data(), static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL;
            };
            return equalsLeaf(L"CON") || equalsLeaf(L"PRN") || equalsLeaf(L"AUX") || equalsLeaf(L"NUL") ||
                   equalsLeaf(L"CLOCK$") || equalsLeaf(L"CONIN$") || equalsLeaf(L"CONOUT$") ||
                   isNumberedDevice(L"COM") || isNumberedDevice(L"LPT");
        };
        try
        {
            pathStr.assign(path);
            std::replace(pathStr.begin(), pathStr.end(), L'/', L'\\');
            if (isReservedDosDevice(pathStr))
            {
                return read_file_result::read_error;
            }

            // Apply the same lexical normalization as CreateFileW without touching the
            // filesystem. This collapses ".", "..", duplicate separators, and trailing
            // spaces/periods before the normalized path is opened relative to the drive.
            const auto needed = GetFullPathNameW(pathStr.c_str(), 0, nullptr, nullptr);
            if (needed == 0)
            {
                return read_file_result::invalid;
            }
            std::wstring normalized(needed, L'\0');
            const auto written = GetFullPathNameW(pathStr.c_str(), needed, normalized.data(), nullptr);
            if (written == 0 || written >= needed)
            {
                return read_file_result::invalid;
            }
            normalized.resize(written);
            pathStr.swap(normalized);
        }
        catch (...)
        {
            return read_file_result::read_error;
        }

        if (pathStr.size() < 3 ||
            CompareStringOrdinal(pathStr.data(), 1, &driveLetter, 1, TRUE) != CSTR_EQUAL ||
            pathStr[1] != L':' || pathStr[2] != L'\\')
        {
            return read_file_result::invalid;
        }

        const auto relativePath = std::wstring_view{ pathStr }.substr(3);
        if (relativePath.empty() || relativePath.size() > USHRT_MAX / sizeof(wchar_t))
        {
            return read_file_result::invalid;
        }

        // Resolve only the already-validated fixed drive root through Win32. The remaining
        // untrusted path is opened relative to this handle so OBJ_DONT_REPARSE does not
        // reject the Object Manager's normal \??\C: drive-letter symbolic link.
        wil::unique_hfile drive{ CreateFileW(driveRoot,
                                             FILE_READ_ATTRIBUTES,
                                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                             nullptr,
                                             OPEN_EXISTING,
                                             FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                             nullptr) };
        if (!drive)
        {
            return read_file_result::read_error;
        }

        UNICODE_STRING objectName{
            .Length = static_cast<USHORT>(relativePath.size() * sizeof(wchar_t)),
            .MaximumLength = static_cast<USHORT>(relativePath.size() * sizeof(wchar_t)),
            .Buffer = const_cast<wchar_t*>(relativePath.data()),
        };
        OBJECT_ATTRIBUTES objectAttributes{
            .Length = sizeof(OBJECT_ATTRIBUTES),
            .RootDirectory = drive.get(),
            .ObjectName = &objectName,
            .Attributes = OBJ_CASE_INSENSITIVE | OBJ_DONT_REPARSE,
        };

        // Open without following any reparse point in the path. The post-open canonical
        // checks below are too late to prevent an intermediate junction or symlink from
        // initiating an SMB/NTLM handshake, so this intentionally rejects all reparse
        // traversal before the target is touched.
        const auto openFile = [&](const ACCESS_MASK access, wil::unique_hfile& file) noexcept {
            IO_STATUS_BLOCK status{};
            return NtCreateFile(file.addressof(),
                                access | SYNCHRONIZE,
                                &objectAttributes,
                                &status,
                                nullptr,
                                FILE_ATTRIBUTE_NORMAL,
                                FILE_SHARE_READ | FILE_SHARE_DELETE,
                                FILE_OPEN,
                                FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT,
                                nullptr,
                                0);
        };

        // Request DELETE only when we might actually remove the file, so an ordinary read
        // never needs delete rights. FILE_SHARE_DELETE lets the file be unlinked while open.
        wil::unique_hfile file;
        auto status = openFile(GENERIC_READ | (deleteAfter ? static_cast<ACCESS_MASK>(DELETE) : 0ul), file);
        if (status < 0 && deleteAfter)
        {
            // The file may not grant DELETE (e.g. a read-only ACL); fall back to a pure
            // read so rendering still works. We then simply cannot delete it.
            deleteAfter = false;
            status = openFile(GENERIC_READ, file);
        }
        if (status < 0)
        {
            const auto err = RtlNtStatusToDosError(status);
            return (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) ? read_file_result::not_found : read_file_result::read_error;
        }

        // Only REGULAR files may be read;
        // device/pipe/socket "special" files must be refused. GetFileType on the open handle is
        // FILE_TYPE_DISK only for a real on-disk file; a character device (e.g. a reserved name
        // like NUL/CON resolved against a drive), a pipe, or an unknown type is rejected. The path
        // checks above already block the device namespace and non-fixed volumes; this is the
        // explicit, auditable form of the spec's "regular files only" rule and closes the
        // reserved-name edge.
        if (GetFileType(file.get()) != FILE_TYPE_DISK)
        {
            return read_file_result::read_error;
        }

        // Resolves the fully-normalized on-disk path for an open handle (following
        // symlinks/junctions and 8.3 short names), or empty on failure. With
        // VOLUME_NAME_DOS every result is prefixed with "\\?\" (e.g. "\\?\C:\dir\file",
        // or "\\?\UNC\server\share\file" for a UNC target).
        const auto finalPath = [](HANDLE handle) noexcept -> std::wstring {
            const auto needed = GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            if (needed == 0)
            {
                return {};
            }
            std::wstring resolved;
            try
            {
                resolved.resize(needed);
            }
            catch (...)
            {
                return {};
            }
            const auto written = GetFinalPathNameByHandleW(handle, resolved.data(), needed, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            if (written == 0 || written >= needed)
            {
                return {};
            }
            resolved.resize(written);
            return resolved;
        };

        const auto canonical = finalPath(file.get());
        if (canonical.empty())
        {
            return read_file_result::read_error;
        }

        // Reject a UNC final path. A drive-absolute client path can still resolve through
        // a junction/mapped drive to a UNC location, so this must be re-checked here.
        static constexpr std::wstring_view uncPrefix{ L"\\\\?\\UNC\\" };
        if (canonical.size() >= uncPrefix.size() &&
            CompareStringOrdinal(canonical.c_str(), static_cast<int>(uncPrefix.size()), uncPrefix.data(), static_cast<int>(uncPrefix.size()), TRUE) == CSTR_EQUAL)
        {
            return read_file_result::read_error;
        }

        // Require a fixed local volume: reject network/removable/optical drives
        // (DRIVE_REMOTE/REMOVABLE/CDROM, plus UNKNOWN/NO_ROOT_DIR).
        wchar_t volumeRoot[MAX_PATH]{};
        if (!GetVolumePathNameW(canonical.c_str(), volumeRoot, ARRAYSIZE(volumeRoot)) || GetDriveTypeW(volumeRoot) != DRIVE_FIXED)
        {
            return read_file_result::read_error;
        }

        LARGE_INTEGER fileSize{};
        if (!GetFileSizeEx(file.get(), &fileSize) || fileSize.QuadPart < 0)
        {
            return read_file_result::read_error;
        }
        const auto total = static_cast<uint64_t>(fileSize.QuadPart);
        if (offset > total)
        {
            return read_file_result::invalid; // offset past end of file
        }

        // Bytes to read: what remains after the offset, clamped by 'size' when nonzero,
        // and always by the hard 32 MiB cap.
        uint64_t toRead = total - offset;
        if (size != 0)
        {
            toRead = std::min(toRead, size);
        }
        toRead = std::min(toRead, maxBytes);

        if (offset != 0)
        {
            LARGE_INTEGER move{};
            move.QuadPart = static_cast<LONGLONG>(offset);
            if (!SetFilePointerEx(file.get(), move, nullptr, FILE_BEGIN))
            {
                return read_file_result::read_error;
            }
        }

        try
        {
            out.resize(static_cast<size_t>(toRead));
        }
        catch (...)
        {
            out.clear();
            return read_file_result::read_error;
        }

        DWORD read = 0;
        if (toRead != 0 && !ReadFile(file.get(), out.data(), static_cast<DWORD>(toRead), &read, nullptr))
        {
            out.clear();
            return read_file_result::read_error;
        }
        out.resize(read);

        // Decide deletion only after a successful read, using the canonical path of the
        // exact inode behind our handle.
        if (deleteAfter)
        {
            // The canonical system temp directory, normalized through a handle so it is in
            // the same "\\?\" form as 'canonical', with a trailing backslash. Prefers
            // GetTempPath2W (Win11, per-process) and falls back to GetTempPathW.
            const auto tempDir = [&finalPath]() noexcept -> std::wstring {
                wchar_t raw[MAX_PATH + 2]{};
                using PfnGetTempPath2W = DWORD(WINAPI*)(DWORD, LPWSTR);
                static const auto pfnGetTempPath2W = []() noexcept {
                    const auto k32 = GetModuleHandleW(L"kernel32.dll");
#pragma warning(suppress : 26490) // Don't use reinterpret_cast (type.1) -- required for GetProcAddress.
                    return k32 ? reinterpret_cast<PfnGetTempPath2W>(GetProcAddress(k32, "GetTempPath2W")) : nullptr;
                }();
                const auto len = pfnGetTempPath2W ? pfnGetTempPath2W(ARRAYSIZE(raw), raw) : GetTempPathW(ARRAYSIZE(raw), raw);
                if (len == 0 || len >= ARRAYSIZE(raw))
                {
                    return {};
                }
                wil::unique_hfile dir{ CreateFileW(raw, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr) };
                if (!dir)
                {
                    return {};
                }
                auto resolved = finalPath(dir.get());
                if (!resolved.empty() && resolved.back() != L'\\')
                {
                    try
                    {
                        resolved.push_back(L'\\');
                    }
                    catch (...)
                    {
                        return {};
                    }
                }
                return resolved;
            }();

            const auto underTemp = !tempDir.empty() && canonical.size() >= tempDir.size() &&
                                   CompareStringOrdinal(canonical.c_str(), static_cast<int>(tempDir.size()), tempDir.c_str(), static_cast<int>(tempDir.size()), TRUE) == CSTR_EQUAL;

            // Require the caller's marker in the file name (case-insensitively), so we
            // only ever auto-delete files the caller created, never an arbitrary file
            // something else happened to leave under temp. No marker, no deletion.
            const auto marker = deleteNameMarker;
            const auto separator = canonical.find_last_of(L'\\');
            const auto filenameOffset = separator == std::wstring::npos ? 0 : separator + 1;
            auto hasMarker = false;
            for (auto i = filenameOffset; !marker.empty() && !hasMarker && i + marker.size() <= canonical.size(); ++i)
            {
                hasMarker = CompareStringOrdinal(canonical.c_str() + i, static_cast<int>(marker.size()), marker.data(), static_cast<int>(marker.size()), TRUE) == CSTR_EQUAL;
            }

            if (underTemp && hasMarker)
            {
                // Delete the exact open inode by setting its disposition, then closing the
                // handle below. No path is re-resolved, so there is no window for a
                // junction/symlink swap between validation and deletion (TOCTOU-safe).
                FILE_DISPOSITION_INFO disposition{};
                disposition.DeleteFile = TRUE;
                std::ignore = SetFileInformationByHandle(file.get(), FileDispositionInfo, &disposition, sizeof(disposition));
            }
        }

        return read_file_result::ok; // the handle closes here; a set disposition removes the inode.
    }

} // til
