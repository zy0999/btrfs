/* Copyright (c) Mark Harmstone 2021
 *
 * This file is part of WinBtrfs.
 *
 * WinBtrfs is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public Licence as published by
 * the Free Software Foundation, either version 3 of the Licence, or
 * (at your option) any later version.
 *
 * WinBtrfs is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public Licence for more details.
 *
 * You should have received a copy of the GNU Lesser General Public Licence
 * along with WinBtrfs.  If not, see <http://www.gnu.org/licenses/>. */

#include "test.h"
#include <wincon.h>
#include <winsvc.h>
#include <winver.h>
#include <functional>

using namespace std;

enum fs_type fstype;

static unsigned int num_tests_run, num_tests_passed;

unique_handle create_file(const u16string_view& path, ACCESS_MASK access, ULONG atts, ULONG share,
                          ULONG dispo, ULONG options, ULONG_PTR exp_info, optional<uint64_t> allocation) {
    NTSTATUS Status;
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    LARGE_INTEGER alloc_size;

    oa.Length = sizeof(oa);
    oa.RootDirectory = nullptr; // FIXME - test

    us.Length = us.MaximumLength = path.length() * sizeof(char16_t);
    us.Buffer = (WCHAR*)path.data();
    oa.ObjectName = &us;

    oa.Attributes = OBJ_CASE_INSENSITIVE; // FIXME - test
    oa.SecurityDescriptor = nullptr; // FIXME - test
    oa.SecurityQualityOfService = nullptr; // FIXME - test(?)

    if (allocation)
        alloc_size.QuadPart = allocation.value();

    // FIXME - EaBuffer and EaLength

    iosb.Information = 0xdeadbeef;

    Status = NtCreateFile(&h, access, &oa, &iosb, allocation ? &alloc_size : nullptr,
                          atts, share, dispo, options, nullptr, 0);

    if (Status != STATUS_SUCCESS) {
        if (NT_SUCCESS(Status)) // STATUS_OPLOCK_BREAK_IN_PROGRESS etc.
            NtClose(h);

        throw ntstatus_error(Status);
    }

    if (iosb.Information != exp_info)
        throw formatted_error("iosb.Information was {}, expected {}", iosb.Information, exp_info);

    return unique_handle(h);
}

template<typename T>
T query_information(HANDLE h) {
    IO_STATUS_BLOCK iosb;
    NTSTATUS Status;
    T t;
    FILE_INFORMATION_CLASS fic;

    if constexpr (is_same_v<T, FILE_BASIC_INFORMATION>)
        fic = FileBasicInformation;
    else if constexpr (is_same_v<T, FILE_STANDARD_INFORMATION>)
        fic = FileStandardInformation;
    else if constexpr (is_same_v<T, FILE_ACCESS_INFORMATION>)
        fic = FileAccessInformation;
    else if constexpr (is_same_v<T, FILE_MODE_INFORMATION>)
        fic = FileModeInformation;
    else if constexpr (is_same_v<T, FILE_ALIGNMENT_INFORMATION>)
        fic = FileAlignmentInformation;
    else if constexpr (is_same_v<T, FILE_POSITION_INFORMATION>)
        fic = FilePositionInformation;
    else if constexpr (is_same_v<T, FILE_INTERNAL_INFORMATION>)
        fic = FileInternalInformation;
    else
        throw runtime_error("Unrecognized file information class.");

    Status = NtQueryInformationFile(h, &iosb, &t, sizeof(t), fic);

    if (Status != STATUS_SUCCESS)
        throw ntstatus_error(Status);

    if (iosb.Information != sizeof(t))
        throw formatted_error("iosb.Information was {}, expected {}", iosb.Information, sizeof(t));

    return t;
}

template FILE_BASIC_INFORMATION query_information<FILE_BASIC_INFORMATION>(HANDLE h);
template FILE_STANDARD_INFORMATION query_information<FILE_STANDARD_INFORMATION>(HANDLE h);
template FILE_ACCESS_INFORMATION query_information<FILE_ACCESS_INFORMATION>(HANDLE h);
template FILE_MODE_INFORMATION query_information<FILE_MODE_INFORMATION>(HANDLE h);
template FILE_ALIGNMENT_INFORMATION query_information<FILE_ALIGNMENT_INFORMATION>(HANDLE h);
template FILE_POSITION_INFORMATION query_information<FILE_POSITION_INFORMATION>(HANDLE h);
template FILE_INTERNAL_INFORMATION query_information<FILE_INTERNAL_INFORMATION>(HANDLE h);

template<typename T>
vector<varbuf<T>> query_dir(const u16string& dir, u16string_view filter) {
    NTSTATUS Status;
    IO_STATUS_BLOCK iosb;
    unique_handle dh;
    vector<uint8_t> buf(sizeof(T) + 7);
    bool first = true;
    vector<varbuf<T>> ret;
    FILE_INFORMATION_CLASS fic;
    UNICODE_STRING us;
    size_t off;

    if constexpr (is_same_v<T, FILE_DIRECTORY_INFORMATION>)
        fic = FileDirectoryInformation;
    else if constexpr (is_same_v<T, FILE_BOTH_DIR_INFORMATION>)
        fic = FileBothDirectoryInformation;
    else if constexpr (is_same_v<T, FILE_FULL_DIR_INFORMATION>)
        fic = FileFullDirectoryInformation;
    else if constexpr (is_same_v<T, FILE_ID_BOTH_DIR_INFORMATION>)
        fic = FileIdBothDirectoryInformation;
    else if constexpr (is_same_v<T, FILE_ID_FULL_DIR_INFORMATION>)
        fic = FileIdFullDirectoryInformation;
    else if constexpr (is_same_v<T, FILE_ID_EXTD_DIR_INFORMATION>)
        fic = FileIdExtdDirectoryInformation;
    else if constexpr (is_same_v<T, FILE_ID_EXTD_BOTH_DIR_INFORMATION>)
        fic = FileIdExtdBothDirectoryInformation;
    else if constexpr (is_same_v<T, FILE_NAMES_INFORMATION>)
        fic = FileNamesInformation;
    else
        throw runtime_error("Unrecognized file information class.");

    // buffer needs to be aligned to 8 bytes
    off = 8 - ((uintptr_t)buf.data() % 8);

    if (off == 8)
        off = 0;

    if (!filter.empty()) {
        us.Buffer = (WCHAR*)filter.data();
        us.Length = us.MaximumLength = filter.size() * sizeof(char16_t);
    }

    dh = create_file(dir, SYNCHRONIZE | FILE_LIST_DIRECTORY, 0, 0, FILE_OPEN,
                     FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
                     FILE_OPENED);

    while (true) {
        Status = NtQueryDirectoryFile(dh.get(), nullptr, nullptr, nullptr, &iosb,
                                      buf.data() + off, buf.size() - off, fic, false,
                                      !filter.empty() ? &us : nullptr, first);

        if (Status == STATUS_BUFFER_OVERFLOW) {
            size_t new_size;

            new_size = offsetof(T, FileName) + (256 * sizeof(WCHAR));
            new_size += ((T*)(buf.data() + off))->FileNameLength * sizeof(WCHAR);

            buf.resize(new_size + 7);

            off = 8 - ((uintptr_t)buf.data() % 8);

            if (off == 8)
                off = 0;

            Status = NtQueryDirectoryFile(dh.get(), nullptr, nullptr, nullptr, &iosb,
                                          buf.data() + off, buf.size() - off, fic, false,
                                          !filter.empty() ? &us : nullptr, first);
        }

        if (Status == STATUS_NO_MORE_FILES)
            break;

        if (Status != STATUS_SUCCESS)
            throw ntstatus_error(Status);

        auto ptr = (T*)buf.data();

        do {
            varbuf<T> item;

            item.buf.resize(offsetof(T, FileName) + (ptr->FileNameLength * sizeof(WCHAR)));
            memcpy(item.buf.data(), ptr, item.buf.size());

            ret.emplace_back(item);

            if (ptr->NextEntryOffset == 0)
                break;

            ptr = (T*)((uint8_t*)ptr + ptr->NextEntryOffset);
        } while (true);

        first = false;
    }

    return ret;
}

template<typename T>
concept has_CreationTime = requires { T::CreationTime; };

template<typename T>
concept has_LastAccessTime = requires { T::LastAccessTime; };

template<typename T>
concept has_LastWriteTime = requires { T::LastWriteTime; };

template<typename T>
concept has_ChangeTime = requires { T::ChangeTime; };

template<typename T>
concept has_EndOfFile = requires { T::EndOfFile; };

template<typename T>
concept has_AllocationSize = requires { T::AllocationSize; };

template<typename T>
concept has_FileAttributes = requires { T::FileAttributes; };

template<typename T>
concept has_FileNameLength = requires { T::FileNameLength; };

template<typename T>
void check_dir_entry(const u16string& dir, const u16string_view& name,
                     const FILE_BASIC_INFORMATION& fbi, const FILE_STANDARD_INFORMATION& fsi) {
    auto items = query_dir<T>(dir, name);

    if (items.size() != 1)
        throw formatted_error("{} entries returned, expected 1.", items.size());

    auto& fdi = *static_cast<const T*>(items.front());

    if constexpr (has_CreationTime<T>) {
        if (fdi.CreationTime.QuadPart != fbi.CreationTime.QuadPart)
            throw formatted_error("CreationTime was {}, expected {}.", fdi.CreationTime.QuadPart, fbi.CreationTime.QuadPart);
    }

    if constexpr (has_LastAccessTime<T>) {
        if (fdi.LastAccessTime.QuadPart != fbi.LastAccessTime.QuadPart)
            throw formatted_error("LastAccessTime was {}, expected {}.", fdi.LastAccessTime.QuadPart, fbi.LastAccessTime.QuadPart);
    }

    if constexpr (has_LastWriteTime<T>) {
        if (fdi.LastWriteTime.QuadPart != fbi.LastWriteTime.QuadPart)
            throw formatted_error("LastWriteTime was {}, expected {}.", fdi.LastWriteTime.QuadPart, fbi.LastWriteTime.QuadPart);
    }

    if constexpr (has_ChangeTime<T>) {
        if (fdi.ChangeTime.QuadPart != fbi.ChangeTime.QuadPart)
            throw formatted_error("ChangeTime was {}, expected {}.", fdi.ChangeTime.QuadPart, fbi.ChangeTime.QuadPart);
    }

    if constexpr (has_EndOfFile<T>) {
        if (fdi.EndOfFile.QuadPart != fsi.EndOfFile.QuadPart)
            throw formatted_error("EndOfFile was {}, expected {}.", fdi.EndOfFile.QuadPart, fsi.EndOfFile.QuadPart);
    }

    if constexpr (has_AllocationSize<T>) {
        if (fdi.AllocationSize.QuadPart != fsi.AllocationSize.QuadPart)
            throw formatted_error("AllocationSize was {}, expected {}.", fdi.AllocationSize.QuadPart, fsi.AllocationSize.QuadPart);
    }

    if constexpr (has_FileAttributes<T>) {
        if (fdi.FileAttributes != fbi.FileAttributes)
            throw formatted_error("FileAttributes was {}, expected {}.", fdi.FileAttributes, fbi.FileAttributes);
    }

    if constexpr (has_FileNameLength<T>) {
        if (fdi.FileNameLength != name.size() * sizeof(char16_t))
            throw formatted_error("FileNameLength was {}, expected {}.", fdi.FileNameLength, name.size() * sizeof(char16_t));

        if (name != u16string_view((char16_t*)fdi.FileName, fdi.FileNameLength / sizeof(char16_t)))
            throw runtime_error("FileName did not match.");
    }

    // FIXME - EaSize
    // FIXME - ShortNameLength / ShortName
    // FIXME - FileId (two different possible lengths)
    // FIXME - ReparsePointTag
}

template void check_dir_entry<FILE_DIRECTORY_INFORMATION>(const u16string& dir, const u16string_view& name,
                                                          const FILE_BASIC_INFORMATION& fbi, const FILE_STANDARD_INFORMATION& fsi);
template void check_dir_entry<FILE_BOTH_DIR_INFORMATION>(const u16string& dir, const u16string_view& name,
                                                         const FILE_BASIC_INFORMATION& fbi, const FILE_STANDARD_INFORMATION& fsi);
template void check_dir_entry<FILE_FULL_DIR_INFORMATION>(const u16string& dir, const u16string_view& name,
                                                         const FILE_BASIC_INFORMATION& fbi, const FILE_STANDARD_INFORMATION& fsi);
template void check_dir_entry<FILE_ID_BOTH_DIR_INFORMATION>(const u16string& dir, const u16string_view& name,
                                                            const FILE_BASIC_INFORMATION& fbi, const FILE_STANDARD_INFORMATION& fsi);
template void check_dir_entry<FILE_ID_FULL_DIR_INFORMATION>(const u16string& dir, const u16string_view& name,
                                                            const FILE_BASIC_INFORMATION& fbi, const FILE_STANDARD_INFORMATION& fsi);
template void check_dir_entry<FILE_ID_EXTD_DIR_INFORMATION>(const u16string& dir, const u16string_view& name,
                                                            const FILE_BASIC_INFORMATION& fbi, const FILE_STANDARD_INFORMATION& fsi);
template void check_dir_entry<FILE_ID_EXTD_BOTH_DIR_INFORMATION>(const u16string& dir, const u16string_view& name,
                                                                 const FILE_BASIC_INFORMATION& fbi, const FILE_STANDARD_INFORMATION& fsi);
template void check_dir_entry<FILE_NAMES_INFORMATION>(const u16string& dir, const u16string_view& name,
                                                      const FILE_BASIC_INFORMATION& fbi, const FILE_STANDARD_INFORMATION& fsi);

void test(const string& msg, const function<void()>& func) {
    string err;
    CONSOLE_SCREEN_BUFFER_INFO csbi;

    num_tests_run++;

    try {
        func();
    } catch (const exception& e) {
        err = e.what();
    } catch (...) {
        err = "Uncaught exception.";
    }

    // FIXME - aligned output?

    fmt::print("{}, ", msg);

    auto col = GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi);

    if (col)
        SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), err.empty() ? FOREGROUND_GREEN : (FOREGROUND_RED | FOREGROUND_INTENSITY));

    fmt::print("{}", err.empty() ? "PASS" : "FAIL");

    if (col)
        SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), csbi.wAttributes);

    if (!err.empty())
        fmt::print(" ({})", err);
    else
        num_tests_passed++;

    fmt::print("\n");
}

void exp_status(const function<void()>& func, NTSTATUS Status) {
    try {
        func();
    } catch (const ntstatus_error& e) {
        if (e.Status != Status)
            throw formatted_error("Status was {}, expected {}", ntstatus_to_string(e.Status), ntstatus_to_string(Status));
        else
            return;
    }

    if (Status != STATUS_SUCCESS)
        throw formatted_error("Status was STATUS_SUCCESS, expected {}", ntstatus_to_string(Status));
}

u16string query_file_name_information(HANDLE h) {
    IO_STATUS_BLOCK iosb;
    NTSTATUS Status;
    FILE_NAME_INFORMATION fni;

    fni.FileNameLength = 0;

    Status = NtQueryInformationFile(h, &iosb, &fni, sizeof(fni), FileNameInformation);

    if (Status != STATUS_SUCCESS && Status != STATUS_BUFFER_OVERFLOW)
        throw ntstatus_error(Status);

    vector<uint8_t> buf(offsetof(FILE_NAME_INFORMATION, FileName) + fni.FileNameLength);

    auto& fni2 = *reinterpret_cast<FILE_NAME_INFORMATION*>(buf.data());

    fni2.FileNameLength = buf.size() - offsetof(FILE_NAME_INFORMATION, FileName);

    Status = NtQueryInformationFile(h, &iosb, &fni2, buf.size(), FileNameInformation);

    if (Status != STATUS_SUCCESS)
        throw ntstatus_error(Status);

    if (iosb.Information != buf.size())
        throw formatted_error("iosb.Information was {}, expected {}", iosb.Information, buf.size());

    u16string ret;

    ret.resize(fni.FileNameLength / sizeof(char16_t));

    memcpy(ret.data(), fni2.FileName, fni.FileNameLength);

    return ret;
}

static unique_handle open_process_token(HANDLE process, ACCESS_MASK access) {
    NTSTATUS Status;
    HANDLE h;

    Status = NtOpenProcessToken(process, access, &h);

    if (Status != STATUS_SUCCESS)
        throw ntstatus_error(Status);

    return unique_handle(h);
}

void disable_token_privileges(HANDLE token) {
    NTSTATUS Status;

    Status = NtAdjustPrivilegesToken(token, true, nullptr, 0, nullptr, nullptr);

    if (Status != STATUS_SUCCESS)
        throw ntstatus_error(Status);
}

string u16string_to_string(const u16string_view& sv) {
    if (sv.empty())
        return "";

    auto len = WideCharToMultiByte(CP_ACP, 0, (WCHAR*)sv.data(), sv.length(), nullptr, 0, nullptr, nullptr);
    if (len == 0)
        throw formatted_error("WideCharToMultiByte failed (error {})", GetLastError());

    string s(len, 0);

    if (WideCharToMultiByte(CP_ACP, 0, (WCHAR*)sv.data(), sv.length(), s.data(), s.length(), nullptr, nullptr) == 0)
        throw formatted_error("WideCharToMultiByte failed (error {})", GetLastError());

    return s;
}

static void do_tests(const u16string_view& name, const u16string& dir) {
    auto token = open_process_token(NtCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY);

    disable_token_privileges(token.get());

    static const struct {
        u16string_view name;
        function<void()> func;
    } testfuncs[] = {
        { u"create", [&]() { test_create(dir); } },
        { u"supersede", [&]() { test_supersede(dir); } },
        { u"overwrite", [&]() { test_overwrite(dir); } },
        { u"open_id", [&]() { test_open_id(token.get(), dir); } },
        { u"io", [&]() { test_io(token.get(), dir); } },
        { u"mmap", [&]() { test_mmap(dir); } },
        { u"rename", [&]() { test_rename(dir); } },
        { u"rename_ex", [&]() { test_rename_ex(token.get(), dir); } },
        { u"delete", [&]() { test_delete(dir); } },
        { u"delete_ex", [&]() { test_delete_ex(token.get(), dir); } },
        { u"links", [&]() { test_links(token.get(), dir); } },
        { u"links_ex", [&]() { test_links_ex(token.get(), dir); } },
        { u"oplock_i", [&]() { test_oplocks_i(token.get(), dir); } },
        { u"oplock_ii", [&]() { test_oplocks_ii(token.get(), dir); } },
        { u"oplock_batch", [&]() { test_oplocks_batch(token.get(), dir); } },
        { u"oplock_filter", [&]() { test_oplocks_filter(token.get(), dir); } },
        { u"oplock_r", [&]() { test_oplocks_r(token.get(), dir); } },
        { u"oplock_rw", [&]() { test_oplocks_rw(token.get(), dir); } },
        { u"oplock_rh", [&]() { test_oplocks_rh(token.get(), dir); } },
        { u"oplock_rwh", [&]() { test_oplocks_rwh(token.get(), dir); } }
    };

    bool first = true;
    unsigned int total_tests_run = 0, total_tests_passed = 0;

    for (const auto& tf : testfuncs) {
        if (name == u"all" || tf.name == name) {
            CONSOLE_SCREEN_BUFFER_INFO csbi;

            auto col = GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi);

            if (col) {
                SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE),
                                        FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
            }

            if (!first)
                fmt::print("\n");

            fmt::print("Running test {}\n", u16string_to_string(tf.name));

            if (col)
                SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), csbi.wAttributes);

            num_tests_run = 0;
            num_tests_passed = 0;

            tf.func();

            total_tests_run += num_tests_run;
            total_tests_passed += num_tests_passed;

            fmt::print("Passed {}/{}\n", num_tests_passed, num_tests_run);

            first = false;

            if (name != u"all")
                break;
        }
    }

    // FIXME - check with case-sensitive flag set

    // FIXME - reparse points (opening, opening following link, creating, setting, querying tag)

    // FIXME - ADSes (including prohibited names)

    // FIXME - test what happens when we use filename in path as if it were a directory (creating, renaming, linking)

    // FIXME - EAs
    // FIXME - FILE_NO_EA_KNOWLEDGE

    // FIXME - setting file information

    // FIXME - querying SD
    // FIXME - setting SD
    // FIXME - inheriting SD
    // FIXME - open files asking for too many permissions
    // FIXME - MAXIMUM_ALLOWED

    // FIXME - querying directory (inc. specific files)
    // FIXME - directory notifications

    // FIXME - IOCTLs and FSCTLs

    // FIXME - querying volume info
    // FIXME - setting volume label

    // FIXME - locking

    // FIXME - object IDs

    // FIXME - traverse checking

    // FIXME - IO completions?

    // FIXME - share access

    // FIXME - reflink copies
    // FIXME - creating subvols
    // FIXME - snapshots
    // FIXME - sending and receiving(?)
    // FIXME - using mknod etc. to test mapping between Linux and Windows concepts?

    if (name != u"all" && first)
        throw runtime_error("Test not supported.");

    if (name == u"all")
        fmt::print("\nTotal passed {}/{}\n", total_tests_passed, total_tests_run);
}

static u16string to_u16string(time_t n) {
    u16string s;

    while (n > 0) {
        s += (n % 10) + u'0';
        n /= 10;
    }

    return u16string(s.rbegin(), s.rend());
}

static bool fs_driver_path(HANDLE h, const u16string_view& driver) {
    NTSTATUS Status;
    IO_STATUS_BLOCK iosb;
    vector<uint8_t> buf(offsetof(FILE_FS_DRIVER_PATH_INFORMATION, DriverName) + (driver.size() * sizeof(char16_t)));

    auto& ffdpi = *(FILE_FS_DRIVER_PATH_INFORMATION*)buf.data();

    ffdpi.DriverInPath = false;
    ffdpi.DriverNameLength = driver.size() * sizeof(char16_t);
    memcpy(&ffdpi.DriverName, driver.data(), ffdpi.DriverNameLength);

    Status = NtQueryVolumeInformationFile(h, &iosb, &ffdpi, buf.size(), FileFsDriverPathInformation);

    if (Status == STATUS_OBJECT_NAME_NOT_FOUND) // driver not loaded
        return false;

    if (Status != STATUS_SUCCESS)
        throw ntstatus_error(Status);

    return ffdpi.DriverInPath;
}

class sc_handle_closer {
public:
    typedef SC_HANDLE pointer;

    void operator()(SC_HANDLE h) {
        CloseServiceHandle(h);
    }
};

static optional<u16string> get_environment_variable(const u16string& name) {
   auto len = GetEnvironmentVariableW((WCHAR*)name.c_str(), nullptr, 0);

   if (len == 0) {
       if (GetLastError() == ERROR_ENVVAR_NOT_FOUND)
           return nullopt;

       return u"";
   }

   u16string ret(len, 0);

   if (GetEnvironmentVariableW((WCHAR*)name.c_str(), (WCHAR*)ret.data(), len) == 0)
       throw formatted_error("GetEnvironmentVariable failed (error {})", GetLastError());

   while (!ret.empty() && ret.back() == 0) {
       ret.pop_back();
   }

   return ret;
}

static u16string get_driver_path(const u16string& driver) {
    unique_ptr<SC_HANDLE, sc_handle_closer> sc_manager, service;

    sc_manager.reset(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!sc_manager)
        throw formatted_error("OpenSCManager failed (error {})", GetLastError());

    service.reset(OpenServiceW(sc_manager.get(), (WCHAR*)driver.c_str(), SERVICE_QUERY_CONFIG));
    if (!service)
        throw formatted_error("OpenService failed (error {})", GetLastError());

    vector<uint8_t> buf(sizeof(QUERY_SERVICE_CONFIGW));
    DWORD needed;

    if (!QueryServiceConfigW(service.get(), (QUERY_SERVICE_CONFIGW*)buf.data(), buf.size(), &needed)) {
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            throw formatted_error("QueryServiceConfig failed (error {})", GetLastError());

        buf.resize(needed);

        if (!QueryServiceConfigW(service.get(), (QUERY_SERVICE_CONFIGW*)buf.data(), buf.size(), &needed))
            throw formatted_error("QueryServiceConfig failed (error {})", GetLastError());
    }

    auto& qsc = *(QUERY_SERVICE_CONFIGW*)buf.data();

    u16string path = (char16_t*)qsc.lpBinaryPathName;

    if (path.empty()) // if the bootloader has sorted it out
        path = u"\\SystemRoot\\System32\\drivers\\" + driver + u".sys";

    if (path.substr(0, 12) == u"\\SystemRoot\\") { // FIXME - case-insensitive?
        auto sr = get_environment_variable(u"SystemRoot");

        // FIXME - get from \\SystemRoot symlink instead?

        if (!sr.has_value())
            throw runtime_error("SystemRoot environment variable not set.");

        path = sr.value() + u"\\" + path.substr(12);
    }

    if (path.substr(0, 4) == u"\\??\\")
        path = path.substr(4);

    return path;
}

static string get_version(const u16string& fn) {
    DWORD dummy;

    auto len = GetFileVersionInfoSizeW((WCHAR*)fn.c_str(), &dummy);
    if (len == 0)
        throw formatted_error("GetFileVersionInfoSize failed (error {})", GetLastError());

    vector<uint8_t> buf(len);

    if (!GetFileVersionInfoW((WCHAR*)fn.c_str(), 0, buf.size(), buf.data()))
        throw formatted_error("GetFileVersionInfo failed (error {})", GetLastError());

    VS_FIXEDFILEINFO* ver;
    UINT verlen;

    if (!VerQueryValueW(buf.data(), L"\\", (void**)&ver, &verlen))
        throw runtime_error("VerQueryValue failed");

    return fmt::format("{}.{}.{}.{}", ver->dwFileVersionMS >> 16, ver->dwFileVersionMS & 0xffff,
                       ver->dwFileVersionLS >> 16, ver->dwFileVersionLS & 0xffff);
}

static string driver_string(const u16string& driver) {
    try {
        auto path = get_driver_path(driver);

        auto version = get_version(path);

        return u16string_to_string(path) + ", " + version;
    } catch (const exception& e) {
        return e.what();
    }
}

int wmain(int argc, wchar_t* argv[]) {
    if (argc < 2) {
        fmt::print(stderr, "Usage: test.exe <dir>\n       test.exe <test> <dir>");
        return 1;
    }

    try {
        u16string_view dirarg = (char16_t*)(argc < 3 ? argv[1] : argv[2]);

        u16string ntdir = u"\\??\\"s + u16string(dirarg);
        ntdir += u"\\" + to_u16string(time(nullptr));

        unique_handle dirh;

        try {
            dirh = create_file(ntdir, GENERIC_WRITE, 0, 0, FILE_CREATE, FILE_DIRECTORY_FILE, FILE_CREATED);
        } catch (const exception& e) {
            throw runtime_error("Error creating directory: "s + e.what());
        }

        bool type_lookup_failed = false;

        fstype = fs_type::unknown;

        try {
            /* See lie_about_fs_type() for why we can't use FileFsAttributeInformation. */

            if (fs_driver_path(dirh.get(), u"\\FileSystem\\NTFS"))
                fstype = fs_type::ntfs;
            else if (fs_driver_path(dirh.get(), u"\\Driver\\btrfs"))
                fstype = fs_type::btrfs;
        } catch (const exception& e) {
            fmt::print(stderr, "Error getting filesystem type: {}\n", e.what());
            type_lookup_failed = true;
        }

        dirh.reset();

        if (!type_lookup_failed) {
            switch (fstype) {
                case fs_type::ntfs:
                    fmt::print("Testing on NTFS ({}).\n", driver_string(u"ntfs"));
                    break;

                case fs_type::btrfs:
                    fmt::print("Testing on Btrfs ({}).\n", driver_string(u"btrfs"));
                    break;

                default:
                    fmt::print("Testing on unknown filesystem.\n");
                    break;
            }
        }

        u16string_view testarg = argc < 3 ? u"all" : (char16_t*)argv[1];

        do_tests(testarg, ntdir);
    } catch (const exception& e) {
        fmt::print(stderr, "{}\n", e.what());
        return 1;
    }

    return 0;
}
