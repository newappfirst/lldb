//===-- ProcessMonitor.cpp ------------------------------------ -*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

// C Includes
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <elf.h>

// C++ Includes
// Other libraries and framework includes
#include "lldb/Core/Debugger.h"
#include "lldb/Core/Error.h"
#include "lldb/Core/RegisterValue.h"
#include "lldb/Core/Scalar.h"
#include "lldb/Host/Host.h"
#include "lldb/Host/HostNativeThread.h"
#include "lldb/Host/HostThread.h"
#include "lldb/Host/ThreadLauncher.h"
#include "lldb/Target/Thread.h"
#include "lldb/Target/RegisterContext.h"
#include "lldb/Target/UnixSignals.h"
#include "lldb/Utility/PseudoTerminal.h"

#include "Plugins/Process/POSIX/CrashReason.h"
#include "Plugins/Process/POSIX/POSIXThread.h"
#include "ProcessLinux.h"
#include "Plugins/Process/POSIX/ProcessPOSIXLog.h"
#include "ProcessMonitor.h"
#include "Procfs.h"

// System includes - They have to be included after framework includes because they define some
// macros which collide with variable names in other modules

#include "lldb/Host/linux/Personality.h"
#include "lldb/Host/linux/Ptrace.h"
#include "lldb/Host/linux/Signalfd.h"
#include "lldb/Host/android/Android.h"

#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>

#define LLDB_PERSONALITY_GET_CURRENT_SETTINGS  0xffffffff

// Support hardware breakpoints in case it has not been defined
#ifndef TRAP_HWBKPT
  #define TRAP_HWBKPT 4
#endif

// Try to define a macro to encapsulate the tgkill syscall
// fall back on kill() if tgkill isn't available
#define tgkill(pid, tid, sig) \
    syscall(SYS_tgkill, static_cast<::pid_t>(pid), static_cast<::pid_t>(tid), sig)

using namespace lldb_private;
using namespace lldb_private::process_linux;

static Operation* EXIT_OPERATION = nullptr;

// FIXME: this code is host-dependent with respect to types and
// endianness and needs to be fixed.  For example, lldb::addr_t is
// hard-coded to uint64_t, but on a 32-bit Linux host, ptrace requires
// 32-bit pointer arguments.  This code uses casts to work around the
// problem.

// We disable the tracing of ptrace calls for integration builds to
// avoid the additional indirection and checks.
#ifndef LLDB_CONFIGURATION_BUILDANDINTEGRATION

static void
DisplayBytes (lldb_private::StreamString &s, void *bytes, uint32_t count)
{
    uint8_t *ptr = (uint8_t *)bytes;
    const uint32_t loop_count = std::min<uint32_t>(DEBUG_PTRACE_MAXBYTES, count);
    for(uint32_t i=0; i<loop_count; i++)
    {
        s.Printf ("[%x]", *ptr);
        ptr++;
    }
}

static void PtraceDisplayBytes(int &req, void *data, size_t data_size)
{
    StreamString buf;
    Log *verbose_log (ProcessPOSIXLog::GetLogIfAllCategoriesSet (
                                        POSIX_LOG_PTRACE | POSIX_LOG_VERBOSE));

    if (verbose_log)
    {
        switch(req)
        {
        case PTRACE_POKETEXT:
            {
                DisplayBytes(buf, &data, 8);
                verbose_log->Printf("PTRACE_POKETEXT %s", buf.GetData());
                break;
            }
        case PTRACE_POKEDATA:
            {
                DisplayBytes(buf, &data, 8);
                verbose_log->Printf("PTRACE_POKEDATA %s", buf.GetData());
                break;
            }
        case PTRACE_POKEUSER:
            {
                DisplayBytes(buf, &data, 8);
                verbose_log->Printf("PTRACE_POKEUSER %s", buf.GetData());
                break;
            }
#if !defined (__arm64__) && !defined (__aarch64__)
        case PTRACE_SETREGS:
            {
                DisplayBytes(buf, data, data_size);
                verbose_log->Printf("PTRACE_SETREGS %s", buf.GetData());
                break;
            }
        case PTRACE_SETFPREGS:
            {
                DisplayBytes(buf, data, data_size);
                verbose_log->Printf("PTRACE_SETFPREGS %s", buf.GetData());
                break;
            }
#endif
        case PTRACE_SETSIGINFO:
            {
                DisplayBytes(buf, data, sizeof(siginfo_t));
                verbose_log->Printf("PTRACE_SETSIGINFO %s", buf.GetData());
                break;
            }
        case PTRACE_SETREGSET:
            {
                // Extract iov_base from data, which is a pointer to the struct IOVEC
                DisplayBytes(buf, *(void **)data, data_size);
                verbose_log->Printf("PTRACE_SETREGSET %s", buf.GetData());
                break;
            }
        default:
            {
            }
        }
    }
}

// Wrapper for ptrace to catch errors and log calls.
// Note that ptrace sets errno on error because -1 can be a valid result (i.e. for PTRACE_PEEK*)
extern long
PtraceWrapper(int req, lldb::pid_t pid, void *addr, void *data, size_t data_size,
              const char* reqName, const char* file, int line)
{
    long int result;

    Log *log (ProcessPOSIXLog::GetLogIfAllCategoriesSet (POSIX_LOG_PTRACE));

    PtraceDisplayBytes(req, data, data_size);

    errno = 0;
    if (req == PTRACE_GETREGSET || req == PTRACE_SETREGSET)
        result = ptrace(static_cast<__ptrace_request>(req), static_cast<pid_t>(pid), *(unsigned int *)addr, data);
    else
        result = ptrace(static_cast<__ptrace_request>(req), static_cast<pid_t>(pid), addr, data);

    if (log)
        log->Printf("ptrace(%s, %" PRIu64 ", %p, %p, %zu)=%lX called from file %s line %d",
                    reqName, pid, addr, data, data_size, result, file, line);

    PtraceDisplayBytes(req, data, data_size);

    if (log && errno != 0)
    {
        const char* str;
        switch (errno)
        {
        case ESRCH:  str = "ESRCH"; break;
        case EINVAL: str = "EINVAL"; break;
        case EBUSY:  str = "EBUSY"; break;
        case EPERM:  str = "EPERM"; break;
        default:     str = "<unknown>";
        }
        log->Printf("ptrace() failed; errno=%d (%s)", errno, str);
    }

    return result;
}

// Wrapper for ptrace when logging is not required.
// Sets errno to 0 prior to calling ptrace.
extern long
PtraceWrapper(int req, pid_t pid, void *addr, void *data, size_t data_size)
{
    long result = 0;
    errno = 0;
    if (req == PTRACE_GETREGSET || req == PTRACE_SETREGSET)
        result = ptrace(static_cast<__ptrace_request>(req), pid, *(unsigned int *)addr, data);
    else
        result = ptrace(static_cast<__ptrace_request>(req), pid, addr, data);
    return result;
}

#define PTRACE(req, pid, addr, data, data_size) \
    PtraceWrapper((req), (pid), (addr), (data), (data_size), #req, __FILE__, __LINE__)
#else
    PtraceWrapper((req), (pid), (addr), (data), (data_size))
#endif

//------------------------------------------------------------------------------
// Static implementations of ProcessMonitor::ReadMemory and
// ProcessMonitor::WriteMemory.  This enables mutual recursion between these
// functions without needed to go thru the thread funnel.

static size_t
DoReadMemory(lldb::pid_t pid,
             lldb::addr_t vm_addr, void *buf, size_t size, Error &error)
{
    // ptrace word size is determined by the host, not the child
    static const unsigned word_size = sizeof(void*);
    unsigned char *dst = static_cast<unsigned char*>(buf);
    size_t bytes_read;
    size_t remainder;
    long data;

    Log *log (ProcessPOSIXLog::GetLogIfAllCategoriesSet (POSIX_LOG_ALL));
    if (log)
        ProcessPOSIXLog::IncNestLevel();
    if (log && ProcessPOSIXLog::AtTopNestLevel() && log->GetMask().Test(POSIX_LOG_MEMORY))
        log->Printf ("ProcessMonitor::%s(%" PRIu64 ", %d, %p, %p, %zd, _)", __FUNCTION__,
                     pid, word_size, (void*)vm_addr, buf, size);

    assert(sizeof(data) >= word_size);
    for (bytes_read = 0; bytes_read < size; bytes_read += remainder)
    {
        errno = 0;
        data = PTRACE(PTRACE_PEEKDATA, pid, (void*)vm_addr, NULL, 0);
        if (errno)
        {
            error.SetErrorToErrno();
            if (log)
                ProcessPOSIXLog::DecNestLevel();
            return bytes_read;
        }

        remainder = size - bytes_read;
        remainder = remainder > word_size ? word_size : remainder;

        // Copy the data into our buffer
        for (unsigned i = 0; i < remainder; ++i)
            dst[i] = ((data >> i*8) & 0xFF);

        if (log && ProcessPOSIXLog::AtTopNestLevel() &&
            (log->GetMask().Test(POSIX_LOG_MEMORY_DATA_LONG) ||
             (log->GetMask().Test(POSIX_LOG_MEMORY_DATA_SHORT) &&
              size <= POSIX_LOG_MEMORY_SHORT_BYTES)))
            {
                uintptr_t print_dst = 0;
                // Format bytes from data by moving into print_dst for log output
                for (unsigned i = 0; i < remainder; ++i)
                    print_dst |= (((data >> i*8) & 0xFF) << i*8);
                log->Printf ("ProcessMonitor::%s() [%p]:0x%lx (0x%lx)", __FUNCTION__,
                             (void*)vm_addr, print_dst, (unsigned long)data);
            }

        vm_addr += word_size;
        dst += word_size;
    }

    if (log)
        ProcessPOSIXLog::DecNestLevel();
    return bytes_read;
}

static size_t
DoWriteMemory(lldb::pid_t pid,
              lldb::addr_t vm_addr, const void *buf, size_t size, Error &error)
{
    // ptrace word size is determined by the host, not the child
    static const unsigned word_size = sizeof(void*);
    const unsigned char *src = static_cast<const unsigned char*>(buf);
    size_t bytes_written = 0;
    size_t remainder;

    Log *log (ProcessPOSIXLog::GetLogIfAllCategoriesSet (POSIX_LOG_ALL));
    if (log)
        ProcessPOSIXLog::IncNestLevel();
    if (log && ProcessPOSIXLog::AtTopNestLevel() && log->GetMask().Test(POSIX_LOG_MEMORY))
        log->Printf ("ProcessMonitor::%s(%" PRIu64 ", %d, %p, %p, %zd, _)", __FUNCTION__,
                     pid, word_size, (void*)vm_addr, buf, size);

    for (bytes_written = 0; bytes_written < size; bytes_written += remainder)
    {
        remainder = size - bytes_written;
        remainder = remainder > word_size ? word_size : remainder;

        if (remainder == word_size)
        {
            unsigned long data = 0;
            assert(sizeof(data) >= word_size);
            for (unsigned i = 0; i < word_size; ++i)
                data |= (unsigned long)src[i] << i*8;

            if (log && ProcessPOSIXLog::AtTopNestLevel() &&
                (log->GetMask().Test(POSIX_LOG_MEMORY_DATA_LONG) ||
                 (log->GetMask().Test(POSIX_LOG_MEMORY_DATA_SHORT) &&
                  size <= POSIX_LOG_MEMORY_SHORT_BYTES)))
                 log->Printf ("ProcessMonitor::%s() [%p]:0x%lx (0x%lx)", __FUNCTION__,
                              (void*)vm_addr, *(unsigned long*)src, data);

            if (PTRACE(PTRACE_POKEDATA, pid, (void*)vm_addr, (void*)data, 0))
            {
                error.SetErrorToErrno();
                if (log)
                    ProcessPOSIXLog::DecNestLevel();
                return bytes_written;
            }
        }
        else
        {
            unsigned char buff[8];
            if (DoReadMemory(pid, vm_addr,
                             buff, word_size, error) != word_size)
            {
                if (log)
                    ProcessPOSIXLog::DecNestLevel();
                return bytes_written;
            }

            memcpy(buff, src, remainder);

            if (DoWriteMemory(pid, vm_addr,
                              buff, word_size, error) != word_size)
            {
                if (log)
                    ProcessPOSIXLog::DecNestLevel();
                return bytes_written;
            }

            if (log && ProcessPOSIXLog::AtTopNestLevel() &&
                (log->GetMask().Test(POSIX_LOG_MEMORY_DATA_LONG) ||
                 (log->GetMask().Test(POSIX_LOG_MEMORY_DATA_SHORT) &&
                  size <= POSIX_LOG_MEMORY_SHORT_BYTES)))
                 log->Printf ("ProcessMonitor::%s() [%p]:0x%lx (0x%lx)", __FUNCTION__,
                              (void*)vm_addr, *(const unsigned long*)src, *(const unsigned long*)buff);
        }

        vm_addr += word_size;
        src += word_size;
    }
    if (log)
        ProcessPOSIXLog::DecNestLevel();
    return bytes_written;
}

// Simple helper function to ensure flags are enabled on the given file
// descriptor.
static bool
EnsureFDFlags(int fd, int flags, Error &error)
{
    int status;

    if ((status = fcntl(fd, F_GETFL)) == -1)
    {
        error.SetErrorToErrno();
        return false;
    }

    if (fcntl(fd, F_SETFL, status | flags) == -1)
    {
        error.SetErrorToErrno();
        return false;
    }

    return true;
}

//------------------------------------------------------------------------------
/// @class Operation
/// @brief Represents a ProcessMonitor operation.
///
/// Under Linux, it is not possible to ptrace() from any other thread but the
/// one that spawned or attached to the process from the start.  Therefore, when
/// a ProcessMonitor is asked to deliver or change the state of an inferior
/// process the operation must be "funneled" to a specific thread to perform the
/// task.  The Operation class provides an abstract base for all services the
/// ProcessMonitor must perform via the single virtual function Execute, thus
/// encapsulating the code that needs to run in the privileged context.
class Operation
{
public:
    virtual ~Operation() {}
    virtual void Execute(ProcessMonitor *monitor) = 0;
};

//------------------------------------------------------------------------------
/// @class ReadOperation
/// @brief Implements ProcessMonitor::ReadMemory.
class ReadOperation : public Operation
{
public:
    ReadOperation(lldb::addr_t addr, void *buff, size_t size,
                  Error &error, size_t &result)
        : m_addr(addr), m_buff(buff), m_size(size),
          m_error(error), m_result(result)
        { }

    void Execute(ProcessMonitor *monitor) override;

private:
    lldb::addr_t m_addr;
    void *m_buff;
    size_t m_size;
    Error &m_error;
    size_t &m_result;
};

void
ReadOperation::Execute(ProcessMonitor *monitor)
{
    lldb::pid_t pid = monitor->GetPID();

    m_result = DoReadMemory(pid, m_addr, m_buff, m_size, m_error);
}

//------------------------------------------------------------------------------
/// @class WriteOperation
/// @brief Implements ProcessMonitor::WriteMemory.
class WriteOperation : public Operation
{
public:
    WriteOperation(lldb::addr_t addr, const void *buff, size_t size,
                   Error &error, size_t &result)
        : m_addr(addr), m_buff(buff), m_size(size),
          m_error(error), m_result(result)
        { }

    void Execute(ProcessMonitor *monitor) override;

private:
    lldb::addr_t m_addr;
    const void *m_buff;
    size_t m_size;
    Error &m_error;
    size_t &m_result;
};

void
WriteOperation::Execute(ProcessMonitor *monitor)
{
    lldb::pid_t pid = monitor->GetPID();

    m_result = DoWriteMemory(pid, m_addr, m_buff, m_size, m_error);
}


//------------------------------------------------------------------------------
/// @class ReadRegOperation
/// @brief Implements ProcessMonitor::ReadRegisterValue.
class ReadRegOperation : public Operation
{
public:
    ReadRegOperation(lldb::tid_t tid, unsigned offset, const char *reg_name,
                     RegisterValue &value, bool &result)
        : m_tid(tid), m_offset(offset), m_reg_name(reg_name),
          m_value(value), m_result(result)
        { }

    void Execute(ProcessMonitor *monitor) override;

private:
    lldb::tid_t m_tid;
    uintptr_t m_offset;
    const char *m_reg_name;
    RegisterValue &m_value;
    bool &m_result;
};

void
ReadRegOperation::Execute(ProcessMonitor *monitor)
{
#if defined (__arm64__) || defined (__aarch64__)
    if (m_offset > sizeof(struct user_pt_regs))
    {
        uintptr_t offset = m_offset - sizeof(struct user_pt_regs);
        if (offset > sizeof(struct user_fpsimd_state))
        {
            m_result = false;
        }
        else
        {
            elf_fpregset_t regs;
            int regset = NT_FPREGSET;
            struct iovec ioVec;

            ioVec.iov_base = &regs;
            ioVec.iov_len = sizeof regs;
            if (PTRACE(PTRACE_GETREGSET, m_tid, &regset, &ioVec, sizeof regs) < 0)
                m_result = false;
            else
            {
                m_result = true;
                m_value.SetBytes((void *)(((unsigned char *)(&regs)) + offset), 16, monitor->GetProcess().GetByteOrder());
            }
        }
    }
    else
    {
        elf_gregset_t regs;
        int regset = NT_PRSTATUS;
        struct iovec ioVec;

        ioVec.iov_base = &regs;
        ioVec.iov_len = sizeof regs;
        if (PTRACE(PTRACE_GETREGSET, m_tid, &regset, &ioVec, sizeof regs) < 0)
            m_result = false;
        else
        {
            m_result = true;
            m_value.SetBytes((void *)(((unsigned char *)(regs)) + m_offset), 8, monitor->GetProcess().GetByteOrder());
        }
    }
#else
    Log *log (ProcessPOSIXLog::GetLogIfAllCategoriesSet (POSIX_LOG_REGISTERS));

    // Set errno to zero so that we can detect a failed peek.
    errno = 0;
    lldb::addr_t data = PTRACE(PTRACE_PEEKUSER, m_tid, (void*)m_offset, NULL, 0);
    if (errno)
        m_result = false;
    else
    {
        m_value = data;
        m_result = true;
    }
    if (log)
        log->Printf ("ProcessMonitor::%s() reg %s: 0x%" PRIx64, __FUNCTION__,
                     m_reg_name, data);
#endif
}

#if defined (__arm64__) || defined (__aarch64__)
    //------------------------------------------------------------------------------
    /// @class ReadDBGROperation
    /// @brief Implements NativeProcessLinux::ReadDBGR.
    class ReadDBGROperation : public Operation
    {
    public:
        ReadDBGROperation(lldb::tid_t tid, unsigned int &count_wp, unsigned int &count_bp)
            : m_tid(tid),
              m_count_wp(count_wp),
              m_count_bp(count_bp)
            { }

        void Execute(ProcessMonitor *monitor) override;

    private:
        lldb::tid_t m_tid;
        unsigned int &m_count_wp;
        unsigned int &m_count_bp;
    };

    void
    ReadDBGROperation::Execute(ProcessMonitor *monitor)
    {
       int regset = NT_ARM_HW_WATCH;
       struct iovec ioVec;
       struct user_hwdebug_state dreg_state;

       ioVec.iov_base = &dreg_state;
       ioVec.iov_len = sizeof (dreg_state);

       PTRACE(PTRACE_GETREGSET, m_tid, &regset, &ioVec, ioVec.iov_len);

       m_count_wp = dreg_state.dbg_info & 0xff;
       regset = NT_ARM_HW_BREAK;

       PTRACE(PTRACE_GETREGSET, m_tid, &regset, &ioVec, ioVec.iov_len);
       m_count_bp = dreg_state.dbg_info & 0xff;
    }
#endif

#if defined (__arm64__) || defined (__aarch64__)
    //------------------------------------------------------------------------------
    /// @class WriteDBGROperation
    /// @brief Implements NativeProcessLinux::WriteFPR.
    class WriteDBGROperation : public Operation
    {
    public:
        WriteDBGROperation(lldb::tid_t tid, lldb::addr_t *addr_buf, uint32_t *cntrl_buf, int type, int count)
            : m_tid(tid),
              m_addr_buf(addr_buf),
              m_cntrl_buf(cntrl_buf),
              m_type(type),
              m_count(count)
            { }

        void Execute(ProcessMonitor *monitor) override;

    private:
        lldb::tid_t m_tid;
        lldb::addr_t *m_addr_buf;
        uint32_t *m_cntrl_buf;
        int m_type;
        int m_count;
    };

    void
    WriteDBGROperation::Execute(ProcessMonitor *monitor)
    {
        struct iovec ioVec;
        struct user_hwdebug_state dreg_state;

        memset (&dreg_state, 0, sizeof (dreg_state));
        ioVec.iov_base = &dreg_state;
        ioVec.iov_len = sizeof(dreg_state);

        if (m_type == 0)
            m_type = NT_ARM_HW_WATCH;
        else
            m_type = NT_ARM_HW_BREAK;

        for (int i = 0; i < m_count; i++)
        {
            dreg_state.dbg_regs[i].addr = m_addr_buf[i];
            dreg_state.dbg_regs[i].ctrl = m_cntrl_buf[i];
        }

        PTRACE(PTRACE_SETREGSET, m_tid, &m_type, &ioVec, ioVec.iov_len);
    }
#endif//------------------------------------------------------------------------------
/// @class WriteRegOperation
/// @brief Implements ProcessMonitor::WriteRegisterValue.
class WriteRegOperation : public Operation
{
public:
    WriteRegOperation(lldb::tid_t tid, unsigned offset, const char *reg_name,
                      const RegisterValue &value, bool &result)
        : m_tid(tid), m_offset(offset), m_reg_name(reg_name),
          m_value(value), m_result(result)
        { }

    void Execute(ProcessMonitor *monitor) override;

private:
    lldb::tid_t m_tid;
    uintptr_t m_offset;
    const char *m_reg_name;
    const RegisterValue &m_value;
    bool &m_result;
};

void
WriteRegOperation::Execute(ProcessMonitor *monitor)
{
#if defined (__arm64__) || defined (__aarch64__)
    if (m_offset > sizeof(struct user_pt_regs))
    {
        uintptr_t offset = m_offset - sizeof(struct user_pt_regs);
        if (offset > sizeof(struct user_fpsimd_state))
        {
            m_result = false;
        }
        else
        {
            elf_fpregset_t regs;
            int regset = NT_FPREGSET;
            struct iovec ioVec;

            ioVec.iov_base = &regs;
            ioVec.iov_len = sizeof regs;
            if (PTRACE(PTRACE_GETREGSET, m_tid, &regset, &ioVec, sizeof regs) < 0)
                m_result = false;
            else
            {
                ::memcpy((void *)(((unsigned char *)(&regs)) + offset), m_value.GetBytes(), 16);
                if (PTRACE(PTRACE_SETREGSET, m_tid, &regset, &ioVec, sizeof regs) < 0)
                    m_result = false;
                else
                    m_result = true;
            }
        }
    }
    else
    {
        elf_gregset_t regs;
        int regset = NT_PRSTATUS;
        struct iovec ioVec;

        ioVec.iov_base = &regs;
        ioVec.iov_len = sizeof regs;
        if (PTRACE(PTRACE_GETREGSET, m_tid, &regset, &ioVec, sizeof regs) < 0)
            m_result = false;
        else
        {
            ::memcpy((void *)(((unsigned char *)(&regs)) + m_offset), m_value.GetBytes(), 8);
            if (PTRACE(PTRACE_SETREGSET, m_tid, &regset, &ioVec, sizeof regs) < 0)
                m_result = false;
            else
                m_result = true;
        }
    }
#else
    void* buf;
    Log *log (ProcessPOSIXLog::GetLogIfAllCategoriesSet (POSIX_LOG_REGISTERS));

    buf = (void*) m_value.GetAsUInt64();

    if (log)
        log->Printf ("ProcessMonitor::%s() reg %s: %p", __FUNCTION__, m_reg_name, buf);
    if (PTRACE(PTRACE_POKEUSER, m_tid, (void*)m_offset, buf, 0))
        m_result = false;
    else
        m_result = true;
#endif
}

//------------------------------------------------------------------------------
/// @class ReadGPROperation
/// @brief Implements ProcessMonitor::ReadGPR.
class ReadGPROperation : public Operation
{
public:
    ReadGPROperation(lldb::tid_t tid, void *buf, size_t buf_size, bool &result)
        : m_tid(tid), m_buf(buf), m_buf_size(buf_size), m_result(result)
        { }

    void Execute(ProcessMonitor *monitor) override;

private:
    lldb::tid_t m_tid;
    void *m_buf;
    size_t m_buf_size;
    bool &m_result;
};

void
ReadGPROperation::Execute(ProcessMonitor *monitor)
{
#if defined (__arm64__) || defined (__aarch64__)
    int regset = NT_PRSTATUS;
    struct iovec ioVec;

    ioVec.iov_base = m_buf;
    ioVec.iov_len = m_buf_size;
    if (PTRACE(PTRACE_GETREGSET, m_tid, &regset, &ioVec, m_buf_size) < 0)
        m_result = false;
    else
        m_result = true;
#else
    if (PTRACE(PTRACE_GETREGS, m_tid, NULL, m_buf, m_buf_size) < 0)
        m_result = false;
    else
        m_result = true;
#endif
}

//------------------------------------------------------------------------------
/// @class ReadFPROperation
/// @brief Implements ProcessMonitor::ReadFPR.
class ReadFPROperation : public Operation
{
public:
    ReadFPROperation(lldb::tid_t tid, void *buf, size_t buf_size, bool &result)
        : m_tid(tid), m_buf(buf), m_buf_size(buf_size), m_result(result)
        { }

    void Execute(ProcessMonitor *monitor) override;

private:
    lldb::tid_t m_tid;
    void *m_buf;
    size_t m_buf_size;
    bool &m_result;
};

void
ReadFPROperation::Execute(ProcessMonitor *monitor)
{
#if defined (__arm64__) || defined (__aarch64__)
    int regset = NT_FPREGSET;
    struct iovec ioVec;

    ioVec.iov_base = m_buf;
    ioVec.iov_len = m_buf_size;
    if (PTRACE(PTRACE_GETREGSET, m_tid, &regset, &ioVec, m_buf_size) < 0)
        m_result = false;
    else
        m_result = true;
#else
    if (PTRACE(PTRACE_GETFPREGS, m_tid, NULL, m_buf, m_buf_size) < 0)
        m_result = false;
    else
        m_result = true;
#endif
}

//------------------------------------------------------------------------------
/// @class ReadRegisterSetOperation
/// @brief Implements ProcessMonitor::ReadRegisterSet.
class ReadRegisterSetOperation : public Operation
{
public:
    ReadRegisterSetOperation(lldb::tid_t tid, void *buf, size_t buf_size, unsigned int regset, bool &result)
        : m_tid(tid), m_buf(buf), m_buf_size(buf_size), m_regset(regset), m_result(result)
        { }

    void Execute(ProcessMonitor *monitor) override;

private:
    lldb::tid_t m_tid;
    void *m_buf;
    size_t m_buf_size;
    const unsigned int m_regset;
    bool &m_result;
};

void
ReadRegisterSetOperation::Execute(ProcessMonitor *monitor)
{
    if (PTRACE(PTRACE_GETREGSET, m_tid, (void *)&m_regset, m_buf, m_buf_size) < 0)
        m_result = false;
    else
        m_result = true;
}

//------------------------------------------------------------------------------
/// @class WriteGPROperation
/// @brief Implements ProcessMonitor::WriteGPR.
class WriteGPROperation : public Operation
{
public:
    WriteGPROperation(lldb::tid_t tid, void *buf, size_t buf_size, bool &result)
        : m_tid(tid), m_buf(buf), m_buf_size(buf_size), m_result(result)
        { }

    void Execute(ProcessMonitor *monitor) override;

private:
    lldb::tid_t m_tid;
    void *m_buf;
    size_t m_buf_size;
    bool &m_result;
};

void
WriteGPROperation::Execute(ProcessMonitor *monitor)
{
#if defined (__arm64__) || defined (__aarch64__)
    int regset = NT_PRSTATUS;
    struct iovec ioVec;

    ioVec.iov_base = m_buf;
    ioVec.iov_len = m_buf_size;
    if (PTRACE(PTRACE_SETREGSET, m_tid, &regset, &ioVec, m_buf_size) < 0)
        m_result = false;
    else
        m_result = true;
#else
    if (PTRACE(PTRACE_SETREGS, m_tid, NULL, m_buf, m_buf_size) < 0)
        m_result = false;
    else
        m_result = true;
#endif
}

//------------------------------------------------------------------------------
/// @class WriteFPROperation
/// @brief Implements ProcessMonitor::WriteFPR.
class WriteFPROperation : public Operation
{
public:
    WriteFPROperation(lldb::tid_t tid, void *buf, size_t buf_size, bool &result)
        : m_tid(tid), m_buf(buf), m_buf_size(buf_size), m_result(result)
        { }

    void Execute(ProcessMonitor *monitor) override;

private:
    lldb::tid_t m_tid;
    void *m_buf;
    size_t m_buf_size;
    bool &m_result;
};

void
WriteFPROperation::Execute(ProcessMonitor *monitor)
{
#if defined (__arm64__) || defined (__aarch64__)
    int regset = NT_FPREGSET;
    struct iovec ioVec;

    ioVec.iov_base = m_buf;
    ioVec.iov_len = m_buf_size;
    if (PTRACE(PTRACE_SETREGSET, m_tid, &regset, &ioVec, m_buf_size) < 0)
        m_result = false;
    else
        m_result = true;
#else
    if (PTRACE(PTRACE_SETFPREGS, m_tid, NULL, m_buf, m_buf_size) < 0)
        m_result = false;
    else
        m_result = true;
#endif
}

//------------------------------------------------------------------------------
/// @class WriteRegisterSetOperation
/// @brief Implements ProcessMonitor::WriteRegisterSet.
class WriteRegisterSetOperation : public Operation
{
public:
    WriteRegisterSetOperation(lldb::tid_t tid, void *buf, size_t buf_size, unsigned int regset, bool &result)
        : m_tid(tid), m_buf(buf), m_buf_size(buf_size), m_regset(regset), m_result(result)
        { }

    void Execute(ProcessMonitor *monitor) override;

private:
    lldb::tid_t m_tid;
    void *m_buf;
    size_t m_buf_size;
    const unsigned int m_regset;
    bool &m_result;
};

void
WriteRegisterSetOperation::Execute(ProcessMonitor *monitor)
{
    if (PTRACE(PTRACE_SETREGSET, m_tid, (void *)&m_regset, m_buf, m_buf_size) < 0)
        m_result = false;
    else
        m_result = true;
}

//------------------------------------------------------------------------------
/// @class ReadThreadPointerOperation
/// @brief Implements ProcessMonitor::ReadThreadPointer.
class ReadThreadPointerOperation : public Operation
{
public:
    ReadThreadPointerOperation(lldb::tid_t tid, lldb::addr_t *addr, bool &result)
        : m_tid(tid), m_addr(addr), m_result(result)
        { }

    void Execute(ProcessMonitor *monitor) override;

private:
    lldb::tid_t m_tid;
    lldb::addr_t *m_addr;
    bool &m_result;
};

void
ReadThreadPointerOperation::Execute(ProcessMonitor *monitor)
{
    Log *log (ProcessPOSIXLog::GetLogIfAllCategoriesSet (POSIX_LOG_REGISTERS));
    if (log)
        log->Printf ("ProcessMonitor::%s()", __FUNCTION__);

    // The process for getting the thread area on Linux is
    // somewhat... obscure. There's several different ways depending on
    // what arch you're on, and what kernel version you have.

    const ArchSpec& arch = monitor->GetProcess().GetTarget().GetArchitecture();
    switch(arch.GetMachine())
    {
    case llvm::Triple::aarch64:
    {
         int regset = LLDB_PTRACE_NT_ARM_TLS;
         struct iovec ioVec;

         ioVec.iov_base = m_addr;
         ioVec.iov_len = sizeof(lldb::addr_t);
         if (PTRACE(PTRACE_GETREGSET, m_tid, &regset, &ioVec, ioVec.iov_len) < 0)
           m_result = false;
         else
           m_result = true;
         break;
    }
#if defined(__i386__) || defined(__x86_64__)
    // Note that struct user below has a field named i387 which is x86-specific.
    // Therefore, this case should be compiled only for x86-based systems.
    case llvm::Triple::x86:
    {
        // Find the GS register location for our host architecture.
        size_t gs_user_offset = offsetof(struct user, regs);
#ifdef __x86_64__
        gs_user_offset += offsetof(struct user_regs_struct, gs);
#endif
#ifdef __i386__
        gs_user_offset += offsetof(struct user_regs_struct, xgs);
#endif

        // Read the GS register value to get the selector.
        errno = 0;
        long gs = PTRACE(PTRACE_PEEKUSER, m_tid, (void*)gs_user_offset, NULL, 0);
        if (errno)
        {
            m_result = false;
            break;
        }

        // Read the LDT base for that selector.
        uint32_t tmp[4];
        m_result = (PTRACE(PTRACE_GET_THREAD_AREA, m_tid, (void *)(gs >> 3), &tmp, 0) == 0);
        *m_addr = tmp[1];
        break;
    }
#endif
    case llvm::Triple::x86_64:
        // Read the FS register base.
        m_result = (PTRACE(PTRACE_ARCH_PRCTL, m_tid, m_addr, (void *)ARCH_GET_FS, 0) == 0);
        break;
    default:
        m_result = false;
        break;
    }
}

//------------------------------------------------------------------------------
/// @class ResumeOperation
/// @brief Implements ProcessMonitor::Resume.
class ResumeOperation : public Operation
{
public:
    ResumeOperation(lldb::tid_t tid, uint32_t signo, bool &result) :
        m_tid(tid), m_signo(signo), m_result(result) { }

    void Execute(ProcessMonitor *monitor) override;

private:
    lldb::tid_t m_tid;
    uint32_t m_signo;
    bool &m_result;
};

void
ResumeOperation::Execute(ProcessMonitor *monitor)
{
    intptr_t data = 0;

    if (m_signo != LLDB_INVALID_SIGNAL_NUMBER)
        data = m_signo;

    if (PTRACE(PTRACE_CONT, m_tid, NULL, (void*)data, 0))
    {
        Log *log (ProcessPOSIXLog::GetLogIfAllCategoriesSet (POSIX_LOG_PROCESS));

        if (log)
            log->Printf ("ResumeOperation (%"  PRIu64 ") failed: %s", m_tid, strerror(errno));
        m_result = false;
    }
    else
        m_result = true;
}

//------------------------------------------------------------------------------
/// @class SingleStepOperation
/// @brief Implements ProcessMonitor::SingleStep.
class SingleStepOperation : public Operation
{
public:
    SingleStepOperation(lldb::tid_t tid, uint32_t signo, bool &result)
        : m_tid(tid), m_signo(signo), m_result(result) { }

    void Execute(ProcessMonitor *monitor) override;

private:
    lldb::tid_t m_tid;
    uint32_t m_signo;
    bool &m_result;
};

void
SingleStepOperation::Execute(ProcessMonitor *monitor)
{
    intptr_t data = 0;

    if (m_signo != LLDB_INVALID_SIGNAL_NUMBER)
        data = m_signo;

    if (PTRACE(PTRACE_SINGLESTEP, m_tid, NULL, (void*)data, 0))
        m_result = false;
    else
        m_result = true;
}

//------------------------------------------------------------------------------
/// @class SiginfoOperation
/// @brief Implements ProcessMonitor::GetSignalInfo.
class SiginfoOperation : public Operation
{
public:
    SiginfoOperation(lldb::tid_t tid, void *info, bool &result, int &ptrace_err)
        : m_tid(tid), m_info(info), m_result(result), m_err(ptrace_err) { }

    void Execute(ProcessMonitor *monitor) override;

private:
    lldb::tid_t m_tid;
    void *m_info;
    bool &m_result;
    int &m_err;
};

void
SiginfoOperation::Execute(ProcessMonitor *monitor)
{
    if (PTRACE(PTRACE_GETSIGINFO, m_tid, NULL, m_info, 0)) {
        m_result = false;
        m_err = errno;
    }
    else
        m_result = true;
}

//------------------------------------------------------------------------------
/// @class EventMessageOperation
/// @brief Implements ProcessMonitor::GetEventMessage.
class EventMessageOperation : public Operation
{
public:
    EventMessageOperation(lldb::tid_t tid, unsigned long *message, bool &result)
        : m_tid(tid), m_message(message), m_result(result) { }

    void Execute(ProcessMonitor *monitor) override;

private:
    lldb::tid_t m_tid;
    unsigned long *m_message;
    bool &m_result;
};

void
EventMessageOperation::Execute(ProcessMonitor *monitor)
{
    if (PTRACE(PTRACE_GETEVENTMSG, m_tid, NULL, m_message, 0))
        m_result = false;
    else
        m_result = true;
}

//------------------------------------------------------------------------------
/// @class DetachOperation
/// @brief Implements ProcessMonitor::Detach.
class DetachOperation : public Operation
{
public:
    DetachOperation(lldb::tid_t tid, Error &result) : m_tid(tid), m_error(result) { }

    void Execute(ProcessMonitor *monitor) override;

private:
    lldb::tid_t m_tid;
    Error &m_error;
};

void
DetachOperation::Execute(ProcessMonitor *monitor)
{
    if (ptrace(PT_DETACH, m_tid, NULL, 0) < 0)
        m_error.SetErrorToErrno();
}

ProcessMonitor::OperationArgs::OperationArgs(ProcessMonitor *monitor)
    : m_monitor(monitor)
{
    sem_init(&m_semaphore, 0, 0);
}

ProcessMonitor::OperationArgs::~OperationArgs()
{
    sem_destroy(&m_semaphore);
}

ProcessMonitor::LaunchArgs::LaunchArgs(ProcessMonitor *monitor,
                                       lldb_private::Module *module,
                                       char const **argv,
                                       char const **envp,
                                       const FileSpec &stdin_file_spec,
                                       const FileSpec &stdout_file_spec,
                                       const FileSpec &stderr_file_spec,
                                       const FileSpec &working_dir,
                                       const lldb_private::ProcessLaunchInfo &launch_info)
    : OperationArgs(monitor),
      m_module(module),
      m_argv(argv),
      m_envp(envp),
      m_stdin_file_spec(stdin_file_spec),
      m_stdout_file_spec(stdout_file_spec),
      m_stderr_file_spec(stderr_file_spec),
      m_working_dir(working_dir),
      m_launch_info(launch_info)
{
}

ProcessMonitor::LaunchArgs::~LaunchArgs()
{ }

ProcessMonitor::AttachArgs::AttachArgs(ProcessMonitor *monitor,
                                       lldb::pid_t pid)
    : OperationArgs(monitor), m_pid(pid) { }

ProcessMonitor::AttachArgs::~AttachArgs()
{ }

//------------------------------------------------------------------------------
/// The basic design of the ProcessMonitor is built around two threads.
///
/// One thread (@see SignalThread) simply blocks on a call to waitpid() looking
/// for changes in the debugee state.  When a change is detected a
/// ProcessMessage is sent to the associated ProcessLinux instance.  This thread
/// "drives" state changes in the debugger.
///
/// The second thread (@see OperationThread) is responsible for two things 1)
/// launching or attaching to the inferior process, and then 2) servicing
/// operations such as register reads/writes, stepping, etc.  See the comments
/// on the Operation class for more info as to why this is needed.
ProcessMonitor::ProcessMonitor(ProcessPOSIX *process,
                               Module *module,
                               const char *argv[],
                               const char *envp[],
                               const FileSpec &stdin_file_spec,
                               const FileSpec &stdout_file_spec,
                               const FileSpec &stderr_file_spec,
                               const FileSpec &working_dir,
                               const lldb_private::ProcessLaunchInfo &launch_info,
                               lldb_private::Error &error)
    : m_process(static_cast<ProcessLinux *>(process)),
      m_operation_thread(LLDB_INVALID_HOST_THREAD),
      m_monitor_thread(LLDB_INVALID_HOST_THREAD),
      m_pid(LLDB_INVALID_PROCESS_ID),
      m_terminal_fd(-1),
      m_operation(0)
{
    std::unique_ptr<LaunchArgs> args(new LaunchArgs(this, module, argv, envp,
                                                    stdin_file_spec,
                                                    stdout_file_spec,
                                                    stderr_file_spec,
                                                    working_dir,
                                                    launch_info));

    sem_init(&m_operation_pending, 0, 0);
    sem_init(&m_operation_done, 0, 0);

    StartLaunchOpThread(args.get(), error);
    if (!error.Success())
        return;

WAIT_AGAIN:
    // Wait for the operation thread to initialize.
    if (sem_wait(&args->m_semaphore))
    {
        if (errno == EINTR)
            goto WAIT_AGAIN;
        else
        {
            error.SetErrorToErrno();
            return;
        }
    }

    // Check that the launch was a success.
    if (!args->m_error.Success())
    {
        StopOpThread();
        error = args->m_error;
        return;
    }

    // Finally, start monitoring the child process for change in state.
    m_monitor_thread = Host::StartMonitoringChildProcess(
        ProcessMonitor::MonitorCallback, this, GetPID(), true);
    if (!m_monitor_thread.IsJoinable())
    {
        error.SetErrorToGenericError();
        error.SetErrorString("Process launch failed.");
        return;
    }
}

ProcessMonitor::ProcessMonitor(ProcessPOSIX *process,
                               lldb::pid_t pid,
                               lldb_private::Error &error)
  : m_process(static_cast<ProcessLinux *>(process)),
      m_operation_thread(LLDB_INVALID_HOST_THREAD),
      m_monitor_thread(LLDB_INVALID_HOST_THREAD),
      m_pid(LLDB_INVALID_PROCESS_ID),
      m_terminal_fd(-1),
      m_operation(0)
{
    sem_init(&m_operation_pending, 0, 0);
    sem_init(&m_operation_done, 0, 0);

    std::unique_ptr<AttachArgs> args(new AttachArgs(this, pid));

    StartAttachOpThread(args.get(), error);
    if (!error.Success())
        return;

WAIT_AGAIN:
    // Wait for the operation thread to initialize.
    if (sem_wait(&args->m_semaphore))
    {
        if (errno == EINTR)
            goto WAIT_AGAIN;
        else
        {
            error.SetErrorToErrno();
            return;
        }
    }

    // Check that the attach was a success.
    if (!args->m_error.Success())
    {
        StopOpThread();
        error = args->m_error;
        return;
    }

    // Finally, start monitoring the child process for change in state.
    m_monitor_thread = Host::StartMonitoringChildProcess(
        ProcessMonitor::MonitorCallback, this, GetPID(), true);
    if (!m_monitor_thread.IsJoinable())
    {
        error.SetErrorToGenericError();
        error.SetErrorString("Process attach failed.");
        return;
    }
}

ProcessMonitor::~ProcessMonitor()
{
    StopMonitor();
}

//------------------------------------------------------------------------------
// Thread setup and tear down.
void
ProcessMonitor::StartLaunchOpThread(LaunchArgs *args, Error &error)
{
    static const char *g_thread_name = "lldb.process.linux.operation";

    if (m_operation_thread.IsJoinable())
        return;

    m_operation_thread = ThreadLauncher::LaunchThread(g_thread_name, LaunchOpThread, args, &error);
}

void *
ProcessMonitor::LaunchOpThread(void *arg)
{
    LaunchArgs *args = static_cast<LaunchArgs*>(arg);

    if (!Launch(args)) {
        sem_post(&args->m_semaphore);
        return NULL;
    }

    ServeOperation(args);
    return NULL;
}

bool
ProcessMonitor::Launch(LaunchArgs *args)
{
    assert (args && "null args");
    if (!args)
        return false;

    ProcessMonitor *monitor = args->m_monitor;
    ProcessLinux &process = monitor->GetProcess();
    const char **argv = args->m_argv;
    const char **envp = args->m_envp;
    const FileSpec &stdin_file_spec = args->m_stdin_file_spec;
    const FileSpec &stdout_file_spec = args->m_stdout_file_spec;
    const FileSpec &stderr_file_spec = args->m_stderr_file_spec;
    const FileSpec &working_dir = args->m_working_dir;

    lldb_utility::PseudoTerminal terminal;
    const size_t err_len = 1024;
    char err_str[err_len];
    lldb::pid_t pid;

    lldb::ThreadSP inferior;
    Log *log (ProcessPOSIXLog::GetLogIfAllCategoriesSet (POSIX_LOG_PROCESS));

    // Propagate the environment if one is not supplied.
    if (envp == NULL || envp[0] == NULL)
        envp = const_cast<const char **>(environ);

    if ((pid = terminal.Fork(err_str, err_len)) == static_cast<lldb::pid_t>(-1))
    {
        args->m_error.SetErrorToGenericError();
        args->m_error.SetErrorString("Process fork failed.");
        goto FINISH;
    }

    // Recognized child exit status codes.
    enum {
        ePtraceFailed = 1,
        eDupStdinFailed,
        eDupStdoutFailed,
        eDupStderrFailed,
        eChdirFailed,
        eExecFailed,
        eSetGidFailed
    };

    // Child process.
    if (pid == 0)
    {
        // Trace this process.
        if (PTRACE(PTRACE_TRACEME, 0, NULL, NULL, 0) < 0)
            exit(ePtraceFailed);

        // terminal has already dupped the tty descriptors to stdin/out/err.
        // This closes original fd from which they were copied (and avoids
        // leaking descriptors to the debugged process.
        terminal.CloseSlaveFileDescriptor();

        // Do not inherit setgid powers.
        if (setgid(getgid()) != 0)
            exit(eSetGidFailed);

        // Let us have our own process group.
        setpgid(0, 0);

        // Dup file descriptors if needed.
        //
        // FIXME: If two or more of the paths are the same we needlessly open
        // the same file multiple times.
        if (stdin_file_spec)
            if (!DupDescriptor(stdin_file_spec, STDIN_FILENO, O_RDONLY))
                exit(eDupStdinFailed);

        if (stdout_file_spec)
            if (!DupDescriptor(stdout_file_spec, STDOUT_FILENO, O_WRONLY | O_CREAT))
                exit(eDupStdoutFailed);

        if (stderr_file_spec)
            if (!DupDescriptor(stderr_file_spec, STDERR_FILENO, O_WRONLY | O_CREAT))
                exit(eDupStderrFailed);

        // Change working directory
        if (working_dir && 0 != ::chdir(working_dir.GetCString()))
            exit(eChdirFailed);

        // Disable ASLR if requested.
        if (args->m_launch_info.GetFlags ().Test (lldb::eLaunchFlagDisableASLR))
        {
            const int old_personality = personality (LLDB_PERSONALITY_GET_CURRENT_SETTINGS);
            if (old_personality == -1)
            {
                if (log)
                    log->Printf ("ProcessMonitor::%s retrieval of Linux personality () failed: %s. Cannot disable ASLR.", __FUNCTION__, strerror (errno));
            }
            else
            {
                const int new_personality = personality (ADDR_NO_RANDOMIZE | old_personality);
                if (new_personality == -1)
                {
                    if (log)
                        log->Printf ("ProcessMonitor::%s setting of Linux personality () to disable ASLR failed, ignoring: %s", __FUNCTION__, strerror (errno));

                }
                else
                {
                    if (log)
                        log->Printf ("ProcessMonitor::%s disabling ASLR: SUCCESS", __FUNCTION__);

                }
            }
        }

        // Execute.  We should never return.
        execve(argv[0],
               const_cast<char *const *>(argv),
               const_cast<char *const *>(envp));
        exit(eExecFailed);
    }

    // Wait for the child process to to trap on its call to execve.
    lldb::pid_t wpid;
    ::pid_t raw_pid;
    int status;

    raw_pid = waitpid(pid, &status, 0);
    wpid = static_cast <lldb::pid_t> (raw_pid);
    if (raw_pid < 0)
    {
        args->m_error.SetErrorToErrno();
        goto FINISH;
    }
    else if (WIFEXITED(status))
    {
        // open, dup or execve likely failed for some reason.
        args->m_error.SetErrorToGenericError();
        switch (WEXITSTATUS(status))
        {
            case ePtraceFailed:
                args->m_error.SetErrorString("Child ptrace failed.");
                break;
            case eDupStdinFailed:
                args->m_error.SetErrorString("Child open stdin failed.");
                break;
            case eDupStdoutFailed:
                args->m_error.SetErrorString("Child open stdout failed.");
                break;
            case eDupStderrFailed:
                args->m_error.SetErrorString("Child open stderr failed.");
                break;
            case eChdirFailed:
                args->m_error.SetErrorString("Child failed to set working directory.");
                break;
            case eExecFailed:
                args->m_error.SetErrorString("Child exec failed.");
                break;
            case eSetGidFailed:
                args->m_error.SetErrorString("Child setgid failed.");
                break;
            default:
                args->m_error.SetErrorString("Child returned unknown exit status.");
                break;
        }
        goto FINISH;
    }
    assert(WIFSTOPPED(status) && wpid == pid &&
           "Could not sync with inferior process.");

    if (!SetDefaultPtraceOpts(pid))
    {
        args->m_error.SetErrorToErrno();
        goto FINISH;
    }

    // Release the master terminal descriptor and pass it off to the
    // ProcessMonitor instance.  Similarly stash the inferior pid.
    monitor->m_terminal_fd = terminal.ReleaseMasterFileDescriptor();
    monitor->m_pid = pid;

    // Set the terminal fd to be in non blocking mode (it simplifies the
    // implementation of ProcessLinux::GetSTDOUT to have a non-blocking
    // descriptor to read from).
    if (!EnsureFDFlags(monitor->m_terminal_fd, O_NONBLOCK, args->m_error))
        goto FINISH;

    // Update the process thread list with this new thread.
    // FIXME: should we be letting UpdateThreadList handle this?
    // FIXME: by using pids instead of tids, we can only support one thread.
    inferior.reset(process.CreateNewPOSIXThread(process, pid));

    if (log)
        log->Printf ("ProcessMonitor::%s() adding pid = %" PRIu64, __FUNCTION__, pid);
    process.GetThreadList().AddThread(inferior);

    process.AddThreadForInitialStopIfNeeded(pid);

    // Let our process instance know the thread has stopped.
    process.SendMessage(ProcessMessage::Trace(pid));

FINISH:
    return args->m_error.Success();
}

void
ProcessMonitor::StartAttachOpThread(AttachArgs *args, lldb_private::Error &error)
{
    static const char *g_thread_name = "lldb.process.linux.operation";

    if (m_operation_thread.IsJoinable())
        return;

    m_operation_thread = ThreadLauncher::LaunchThread(g_thread_name, AttachOpThread, args, &error);
}

void *
ProcessMonitor::AttachOpThread(void *arg)
{
    AttachArgs *args = static_cast<AttachArgs*>(arg);

    if (!Attach(args)) {
        sem_post(&args->m_semaphore);
        return NULL;
    }

    ServeOperation(args);
    return NULL;
}

bool
ProcessMonitor::Attach(AttachArgs *args)
{
    lldb::pid_t pid = args->m_pid;

    ProcessMonitor *monitor = args->m_monitor;
    ProcessLinux &process = monitor->GetProcess();
    lldb::ThreadSP inferior;
    Log *log (ProcessPOSIXLog::GetLogIfAllCategoriesSet (POSIX_LOG_PROCESS));

    // Use a map to keep track of the threads which we have attached/need to attach.
    Host::TidMap tids_to_attach;
    if (pid <= 1)
    {
        args->m_error.SetErrorToGenericError();
        args->m_error.SetErrorString("Attaching to process 1 is not allowed.");
        goto FINISH;
    }

    while (Host::FindProcessThreads(pid, tids_to_attach))
    {
        for (Host::TidMap::iterator it = tids_to_attach.begin();
             it != tids_to_attach.end(); ++it)
        {
            if (it->second == false)
            {
                lldb::tid_t tid = it->first;

                // Attach to the requested process.
                // An attach will cause the thread to stop with a SIGSTOP.
                if (PTRACE(PTRACE_ATTACH, tid, NULL, NULL, 0) < 0)
                {
                    // No such thread. The thread may have exited.
                    // More error handling may be needed.
                    if (errno == ESRCH)
                    {
                        tids_to_attach.erase(it);
                        continue;
                    }
                    else
                    {
                        args->m_error.SetErrorToErrno();
                        goto FINISH;
                    }
                }

                ::pid_t wpid;
                // Need to use __WALL otherwise we receive an error with errno=ECHLD
                // At this point we should have a thread stopped if waitpid succeeds.
                if ((wpid = waitpid(tid, NULL, __WALL)) < 0)
                {
                    // No such thread. The thread may have exited.
                    // More error handling may be needed.
                    if (errno == ESRCH)
                    {
                        tids_to_attach.erase(it);
                        continue;
                    }
                    else
                    {
                        args->m_error.SetErrorToErrno();
                        goto FINISH;
                    }
                }

                if (!SetDefaultPtraceOpts(tid))
                {
                    args->m_error.SetErrorToErrno();
                    goto FINISH;
                }

                // Update the process thread list with the attached thread.
                inferior.reset(process.CreateNewPOSIXThread(process, tid));

                if (log)
                    log->Printf ("ProcessMonitor::%s() adding tid = %" PRIu64, __FUNCTION__, tid);
                process.GetThreadList().AddThread(inferior);
                it->second = true;
                process.AddThreadForInitialStopIfNeeded(tid);
            }
        }
    }

    if (tids_to_attach.size() > 0)
    {
        monitor->m_pid = pid;
        // Let our process instance know the thread has stopped.
        process.SendMessage(ProcessMessage::Trace(pid));
    }
    else
    {
        args->m_error.SetErrorToGenericError();
        args->m_error.SetErrorString("No such process.");
    }

 FINISH:
    return args->m_error.Success();
}

bool
ProcessMonitor::SetDefaultPtraceOpts(lldb::pid_t pid)
{
    long ptrace_opts = 0;

    // Have the child raise an event on exit.  This is used to keep the child in
    // limbo until it is destroyed.
    ptrace_opts |= PTRACE_O_TRACEEXIT;

    // Have the tracer trace threads which spawn in the inferior process.
    // TODO: if we want to support tracing the inferiors' child, add the
    // appropriate ptrace flags here (PTRACE_O_TRACEFORK, PTRACE_O_TRACEVFORK)
    ptrace_opts |= PTRACE_O_TRACECLONE;

    // Have the tracer notify us before execve returns
    // (needed to disable legacy SIGTRAP generation)
    ptrace_opts |= PTRACE_O_TRACEEXEC;

    return PTRACE(PTRACE_SETOPTIONS, pid, NULL, (void*)ptrace_opts, 0) >= 0;
}

bool
ProcessMonitor::MonitorCallback(void *callback_baton,
                                lldb::pid_t pid,
                                bool exited,
                                int signal,
                                int status)
{
    ProcessMessage message;
    ProcessMonitor *monitor = static_cast<ProcessMonitor*>(callback_baton);
    ProcessLinux *process = monitor->m_process;
    assert(process);
    bool stop_monitoring;
    siginfo_t info;
    int ptrace_err;

    Log *log (ProcessPOSIXLog::GetLogIfAllCategoriesSet (POSIX_LOG_PROCESS));

    if (exited)
    {
        if (log)
            log->Printf ("ProcessMonitor::%s() got exit signal, tid = %"  PRIu64, __FUNCTION__, pid);
        message = ProcessMessage::Exit(pid, status);
        process->SendMessage(message);
        return pid == process->GetID();
    }

    if (!monitor->GetSignalInfo(pid, &info, ptrace_err)) {
        if (ptrace_err == EINVAL) {
            if (log)
                log->Printf ("ProcessMonitor::%s() resuming from group-stop", __FUNCTION__);
            // inferior process is in 'group-stop', so deliver SIGSTOP signal
            if (!monitor->Resume(pid, SIGSTOP)) {
              assert(0 && "SIGSTOP delivery failed while in 'group-stop' state");
            }
            stop_monitoring = false;
        } else {
            // ptrace(GETSIGINFO) failed (but not due to group-stop). Most likely,
            // this means the child pid is gone (or not being debugged) therefore
            // stop the monitor thread if this is the main pid.
            if (log)
                log->Printf ("ProcessMonitor::%s() GetSignalInfo failed: %s, tid = %" PRIu64 ", signal = %d, status = %d", 
                              __FUNCTION__, strerror(ptrace_err), pid, signal, status);
            stop_monitoring = pid == monitor->m_process->GetID();
            // If we are going to stop monitoring, we need to notify our process object
            if (stop_monitoring)
            {
                message = ProcessMessage::Exit(pid, status);
                process->SendMessage(message);
            }
        }
    }
    else {
        switch (info.si_signo)
        {
        case SIGTRAP:
            message = MonitorSIGTRAP(monitor, &info, pid);
            break;

        default:
            message = MonitorSignal(monitor, &info, pid);
            break;
        }

        process->SendMessage(message);
        stop_monitoring = false;
    }

    return stop_monitoring;
}

ProcessMessage
ProcessMonitor::MonitorSIGTRAP(ProcessMonitor *monitor,
                               const siginfo_t *info, lldb::pid_t pid)
{
    ProcessMessage message;

    Log *log (ProcessPOSIXLog::GetLogIfAllCategoriesSet (POSIX_LOG_PROCESS));

    assert(monitor);
    assert(info && info->si_signo == SIGTRAP && "Unexpected child signal!");

    switch (info->si_code)
    {
    default:
        assert(false && "Unexpected SIGTRAP code!");
        break;

    // TODO: these two cases are required if we want to support tracing
    // of the inferiors' children
    // case (SIGTRAP | (PTRACE_EVENT_FORK << 8)):
    // case (SIGTRAP | (PTRACE_EVENT_VFORK << 8)):

    case (SIGTRAP | (PTRACE_EVENT_CLONE << 8)):
    {
        if (log)
            log->Printf ("ProcessMonitor::%s() received thread creation event, code = %d", __FUNCTION__, info->si_code ^ SIGTRAP);

        unsigned long tid = 0;
        if (!monitor->GetEventMessage(pid, &tid))
            tid = -1;
        message = ProcessMessage::NewThread(pid, tid);
        break;
    }

    case (SIGTRAP | (PTRACE_EVENT_EXEC << 8)):
        if (log)
            log->Printf ("ProcessMonitor::%s() received exec event, code = %d", __FUNCTION__, info->si_code ^ SIGTRAP);

        message = ProcessMessage::Exec(pid);
        break;

    case (SIGTRAP | (PTRACE_EVENT_EXIT << 8)):
    {
        // The inferior process or one of its threads is about to exit.
        // Maintain the process or thread in a state of "limbo" until we are
        // explicitly commanded to detach, destroy, resume, etc.
        unsigned long data = 0;
        if (!monitor->GetEventMessage(pid, &data))
            data = -1;
        if (log)
            log->Printf ("ProcessMonitor::%s() received limbo event, data = %lx, pid = %" PRIu64, __FUNCTION__, data, pid);
        message = ProcessMessage::Limbo(pid, (data >> 8));
        break;
    }

    case 0:
    case TRAP_TRACE:
        if (log)
            log->Printf ("ProcessMonitor::%s() received trace event, pid = %" PRIu64, __FUNCTION__, pid);
        message = ProcessMessage::Trace(pid);
        break;

    case SI_KERNEL:
    case TRAP_BRKPT:
        if (log)
            log->Printf ("ProcessMonitor::%s() received breakpoint event, pid = %" PRIu64, __FUNCTION__, pid);
        message = ProcessMessage::Break(pid);
        break;

    case TRAP_HWBKPT:
        if (log)
            log->Printf ("ProcessMonitor::%s() received watchpoint event, pid = %" PRIu64, __FUNCTION__, pid);
        message = ProcessMessage::Watch(pid, (lldb::addr_t)info->si_addr);
        break;

    case SIGTRAP:
    case (SIGTRAP | 0x80):
        if (log)
            log->Printf ("ProcessMonitor::%s() received system call stop event, pid = %" PRIu64, __FUNCTION__, pid);
        // Ignore these signals until we know more about them
        monitor->Resume(pid, eResumeSignalNone);
    }

    return message;
}

ProcessMessage
ProcessMonitor::MonitorSignal(ProcessMonitor *monitor,
                              const siginfo_t *info, lldb::pid_t pid)
{
    ProcessMessage message;
    int signo = info->si_signo;

    Log *log (ProcessPOSIXLog::GetLogIfAllCategoriesSet (POSIX_LOG_PROCESS));

    // POSIX says that process behaviour is undefined after it ignores a SIGFPE,
    // SIGILL, SIGSEGV, or SIGBUS *unless* that signal was generated by a
    // kill(2) or raise(3).  Similarly for tgkill(2) on Linux.
    //
    // IOW, user generated signals never generate what we consider to be a
    // "crash".
    //
    // Similarly, ACK signals generated by this monitor.
    if (info->si_code == SI_TKILL || info->si_code == SI_USER)
    {
        if (log)
            log->Printf ("ProcessMonitor::%s() received signal %s with code %s, pid = %d",
                            __FUNCTION__,
                            monitor->m_process->GetUnixSignals().GetSignalAsCString (signo),
                            (info->si_code == SI_TKILL ? "SI_TKILL" : "SI_USER"),
                            info->si_pid);

        if (info->si_pid == getpid())
            return ProcessMessage::SignalDelivered(pid, signo);
        else
            return ProcessMessage::Signal(pid, signo);
    }

    if (log)
        log->Printf ("ProcessMonitor::%s() received signal %s", __FUNCTION__, monitor->m_process->GetUnixSignals().GetSignalAsCString (signo));

    switch (signo)
    {
    case SIGSEGV:
    case SIGILL:
    case SIGFPE:
    case SIGBUS:
        lldb::addr_t fault_addr = reinterpret_cast<lldb::addr_t>(info->si_addr);
        const auto reason = GetCrashReason(*info);
        return ProcessMessage::Crash(pid, reason, signo, fault_addr);
    }

    // Everything else is "normal" and does not require any special action on
    // our part.
    return ProcessMessage::Signal(pid, signo);
}

// On Linux, when a new thread is created, we receive to notifications,
// (1) a SIGTRAP|PTRACE_EVENT_CLONE from the main process thread with the
// child thread id as additional information, and (2) a SIGSTOP|SI_USER from
// the new child thread indicating that it has is stopped because we attached.
// We have no guarantee of the order in which these arrive, but we need both
// before we are ready to proceed.  We currently keep a list of threads which
// have sent the initial SIGSTOP|SI_USER event.  Then when we receive the
// SIGTRAP|PTRACE_EVENT_CLONE notification, if the initial stop has not occurred
// we call ProcessMonitor::WaitForInitialTIDStop() to wait for it.

bool
ProcessMonitor::WaitForInitialTIDStop(lldb::tid_t tid)
{
    Log *log (ProcessPOSIXLog::GetLogIfAllCategoriesSet (POSIX_LOG_PROCESS));
    if (log)
        log->Printf ("ProcessMonitor::%s(%" PRIu64 ") waiting for thread to stop...", __FUNCTION__, tid);

    // Wait for the thread to stop
    while (true)
    {
        int status = -1;
        if (log)
            log->Printf ("ProcessMonitor::%s(%" PRIu64 ") waitpid...", __FUNCTION__, tid);
        ::pid_t wait_pid = waitpid(tid, &status, __WALL);
        if (status == -1)
        {
            // If we got interrupted by a signal (in our process, not the
            // inferior) try again.
            if (errno == EINTR)
                continue;
            else
            {
                if (log)
                    log->Printf("ProcessMonitor::%s(%" PRIu64 ") waitpid error -- %s", __FUNCTION__, tid, strerror(errno));
                return false; // This is bad, but there's nothing we can do.
            }
        }

        if (log)
            log->Printf ("ProcessMonitor::%s(%" PRIu64 ") waitpid, status = %d", __FUNCTION__, tid, status);

        assert(static_cast<lldb::tid_t>(wait_pid) == tid);

        siginfo_t info;
        int ptrace_err;
        if (!GetSignalInfo(wait_pid, &info, ptrace_err))
        {
            if (log)
            {
                log->Printf ("ProcessMonitor::%s() GetSignalInfo failed. errno=%d (%s)", __FUNCTION__, ptrace_err, strerror(ptrace_err));
            }
            return false;
        }

        // If this is a thread exit, we won't get any more information.
        if (WIFEXITED(status))
        {
            m_process->SendMessage(ProcessMessage::Exit(wait_pid, WEXITSTATUS(status)));
            if (static_cast<lldb::tid_t>(wait_pid) == tid)
                return true;
            continue;
        }

        assert(info.si_code == SI_USER);
        assert(WSTOPSIG(status) == SIGSTOP);

        if (log)
            log->Printf ("ProcessMonitor::%s(bp) received thread stop signal", __FUNCTION__);
        m_process->AddThreadForInitialStopIfNeeded(wait_pid);
        return true;
    }
    return false;
}

bool
ProcessMonitor::StopThread(lldb::tid_t tid)
{
    Log *log (ProcessPOSIXLog::GetLogIfAllCategoriesSet (POSIX_LOG_PROCESS));

    // FIXME: Try to use tgkill or tkill
    int ret = tgkill(m_pid, tid, SIGSTOP);
    if (log)
        log->Printf ("ProcessMonitor::%s(bp) stopping thread, tid = %" PRIu64 ", ret = %d", __FUNCTION__, tid, ret);

    // This can happen if a thread exited while we were trying to stop it.  That's OK.
    // We'll get the signal for that later.
    if (ret < 0)
        return false;

    // Wait for the thread to stop
    while (true)
    {
        int status = -1;
        if (log)
            log->Printf ("ProcessMonitor::%s(bp) waitpid...", __FUNCTION__);
        ::pid_t wait_pid = ::waitpid (-1*getpgid(m_pid), &status, __WALL);
        if (log)
            log->Printf ("ProcessMonitor::%s(bp) waitpid, pid = %" PRIu64 ", status = %d",
                         __FUNCTION__, static_cast<lldb::pid_t>(wait_pid), status);

        if (wait_pid == -1)
        {
            // If we got interrupted by a signal (in our process, not the
            // inferior) try again.
            if (errno == EINTR)
                continue;
            else
                return false; // This is bad, but there's nothing we can do.
        }

        // If this is a thread exit, we won't get any more information.
        if (WIFEXITED(status))
        {
            m_process->SendMessage(ProcessMessage::Exit(wait_pid, WEXITSTATUS(status)));
            if (static_cast<lldb::tid_t>(wait_pid) == tid)
                return true;
            continue;
        }

        siginfo_t info;
        int ptrace_err;
        if (!GetSignalInfo(wait_pid, &info, ptrace_err))
        {
            // another signal causing a StopAllThreads may have been received
            // before wait_pid's group-stop was processed, handle it now
            if (ptrace_err == EINVAL)
            {
                assert(WIFSTOPPED(status) && WSTOPSIG(status) == SIGSTOP);

                if (log)
                  log->Printf ("ProcessMonitor::%s() resuming from group-stop", __FUNCTION__);
                // inferior process is in 'group-stop', so deliver SIGSTOP signal
                if (!Resume(wait_pid, SIGSTOP)) {
                  assert(0 && "SIGSTOP delivery failed while in 'group-stop' state");
                }
                continue;
            }

            if (log)
                log->Printf ("ProcessMonitor::%s() GetSignalInfo failed.", __FUNCTION__);
            return false;
        }

        // Handle events from other threads
        if (log)
            log->Printf ("ProcessMonitor::%s(bp) handling event, tid == %" PRIu64,
                         __FUNCTION__, static_cast<lldb::tid_t>(wait_pid));

        ProcessMessage message;
        if (info.si_signo == SIGTRAP)
            message = MonitorSIGTRAP(this, &info, wait_pid);
        else
            message = MonitorSignal(this, &info, wait_pid);

        POSIXThread *thread = static_cast<POSIXThread*>(m_process->GetThreadList().FindThreadByID(wait_pid).get());

        // When a new thread is created, we may get a SIGSTOP for the new thread
        // just before we get the SIGTRAP that we use to add the thread to our
        // process thread list.  We don't need to worry about that signal here.
        assert(thread || message.GetKind() == ProcessMessage::eSignalMessage);

        if (!thread)
        {
            m_process->SendMessage(message);
            continue;
        }

        switch (message.GetKind())
        {
            case ProcessMessage::eExecMessage:
                llvm_unreachable("unexpected message");
            case ProcessMessage::eAttachMessage:
            case ProcessMessage::eInvalidMessage:
                break;

            // These need special handling because we don't want to send a
            // resume even if we already sent a SIGSTOP to this thread. In
            // this case the resume will cause the thread to disappear.  It is
            // unlikely that we'll ever get eExitMessage here, but the same
            // reasoning applies.
            case ProcessMessage::eLimboMessage:
            case ProcessMessage::eExitMessage:
                if (log)
                    log->Printf ("ProcessMonitor::%s(bp) handling message", __FUNCTION__);
                // SendMessage will set the thread state as needed.
                m_process->SendMessage(message);
                // If this is the thread we're waiting for, stop waiting. Even
                // though this wasn't the signal we expected, it's the last
                // signal we'll see while this thread is alive.
                if (static_cast<lldb::tid_t>(wait_pid) == tid)
                    return true;
                break;

            case ProcessMessage::eSignalMessage:
                if (log)
                    log->Printf ("ProcessMonitor::%s(bp) handling message", __FUNCTION__);
                if (WSTOPSIG(status) == SIGSTOP)
                {
                    m_process->AddThreadForInitialStopIfNeeded(tid);
                    thread->SetState(lldb::eStateStopped);
                }
                else
                {
                    m_process->SendMessage(message);
                    // This isn't the stop we were expecting, but the thread is
                    // stopped. SendMessage will handle processing of this event,
                    // but we need to resume here to get the stop we are waiting
                    // for (otherwise the thread will stop again immediately when
                    // we try to resume).
                    if (static_cast<lldb::tid_t>(wait_pid) == tid)
                        Resume(wait_pid, eResumeSignalNone);
                }
                break;

            case ProcessMessage::eSignalDeliveredMessage:
                // This is the stop we're expecting.
                if (static_cast<lldb::tid_t>(wait_pid) == tid &&
                    WIFSTOPPED(status) &&
                    WSTOPSIG(status) == SIGSTOP &&
                    info.si_code == SI_TKILL)
                {
                    if (log)
                        log->Printf ("ProcessMonitor::%s(bp) received signal, done waiting", __FUNCTION__);
                    thread->SetState(lldb::eStateStopped);
                    return true;
                }
                // else fall-through
            case ProcessMessage::eBreakpointMessage:
            case ProcessMessage::eTraceMessage:
            case ProcessMessage::eWatchpointMessage:
            case ProcessMessage::eCrashMessage:
            case ProcessMessage::eNewThreadMessage:
                if (log)
                    log->Printf ("ProcessMonitor::%s(bp) handling message", __FUNCTION__);
                // SendMessage will set the thread state as needed.
                m_process->SendMessage(message);
                // This isn't the stop we were expecting, but the thread is
                // stopped. SendMessage will handle processing of this event,
                // but we need to resume here to get the stop we are waiting
                // for (otherwise the thread will stop again immediately when
                // we try to resume).
                if (static_cast<lldb::tid_t>(wait_pid) == tid)
                    Resume(wait_pid, eResumeSignalNone);
                break;
        }
    }
    return false;
}

void
ProcessMonitor::ServeOperation(OperationArgs *args)
{
    ProcessMonitor *monitor = args->m_monitor;

    // We are finised with the arguments and are ready to go.  Sync with the
    // parent thread and start serving operations on the inferior.
    sem_post(&args->m_semaphore);

    for(;;)
    {
        // wait for next pending operation
        if (sem_wait(&monitor->m_operation_pending))
        {
            if (errno == EINTR)
                continue;
            assert(false && "Unexpected errno from sem_wait");
        }

        monitor->m_operation->Execute(monitor);

        // notify calling thread that operation is complete
        sem_post(&monitor->m_operation_done);
    }
}

void
ProcessMonitor::DoOperation(Operation *op)
{
    Mutex::Locker lock(m_operation_mutex);

    m_operation = op;

    // notify operation thread that an operation is ready to be processed
    sem_post(&m_operation_pending);

    // wait for operation to complete
    while (sem_wait(&m_operation_done))
    {
        if (errno == EINTR)
            continue;
        assert(false && "Unexpected errno from sem_wait");
    }
}

size_t
ProcessMonitor::ReadMemory(lldb::addr_t vm_addr, void *buf, size_t size,
                           Error &error)
{
    size_t result;
    ReadOperation op(vm_addr, buf, size, error, result);
    DoOperation(&op);
    return result;
}

size_t
ProcessMonitor::WriteMemory(lldb::addr_t vm_addr, const void *buf, size_t size,
                            lldb_private::Error &error)
{
    size_t result;
    WriteOperation op(vm_addr, buf, size, error, result);
    DoOperation(&op);
    return result;
}

bool
ProcessMonitor::ReadRegisterValue(lldb::tid_t tid, unsigned offset, const char* reg_name,
                                  unsigned size, RegisterValue &value)
{
    bool result;
    ReadRegOperation op(tid, offset, reg_name, value, result);
    DoOperation(&op);
    return result;
}

#if defined (__arm64__) || defined (__aarch64__)

bool
ProcessMonitor::ReadHardwareDebugInfo (lldb::tid_t tid, unsigned int &watch_count , unsigned int &break_count)
{
    bool result = true;
    ReadDBGROperation op(tid, watch_count, break_count);
    DoOperation(&op);
    return result;
}

bool
ProcessMonitor::WriteHardwareDebugRegs (lldb::tid_t tid, lldb::addr_t *addr_buf, uint32_t *cntrl_buf, int type, int count)
{
    bool result = true;
    WriteDBGROperation op(tid, addr_buf, cntrl_buf, type, count);
    DoOperation(&op);
    return result;
}

#endif
bool
ProcessMonitor::WriteRegisterValue(lldb::tid_t tid, unsigned offset,
                                   const char* reg_name, const RegisterValue &value)
{
    bool result;
    WriteRegOperation op(tid, offset, reg_name, value, result);
    DoOperation(&op);
    return result;
}

bool
ProcessMonitor::ReadGPR(lldb::tid_t tid, void *buf, size_t buf_size)
{
    bool result;
    ReadGPROperation op(tid, buf, buf_size, result);
    DoOperation(&op);
    return result;
}

bool
ProcessMonitor::ReadFPR(lldb::tid_t tid, void *buf, size_t buf_size)
{
    bool result;
    ReadFPROperation op(tid, buf, buf_size, result);
    DoOperation(&op);
    return result;
}

bool
ProcessMonitor::ReadRegisterSet(lldb::tid_t tid, void *buf, size_t buf_size, unsigned int regset)
{
    bool result;
    ReadRegisterSetOperation op(tid, buf, buf_size, regset, result);
    DoOperation(&op);
    return result;
}

bool
ProcessMonitor::WriteGPR(lldb::tid_t tid, void *buf, size_t buf_size)
{
    bool result;
    WriteGPROperation op(tid, buf, buf_size, result);
    DoOperation(&op);
    return result;
}

bool
ProcessMonitor::WriteFPR(lldb::tid_t tid, void *buf, size_t buf_size)
{
    bool result;
    WriteFPROperation op(tid, buf, buf_size, result);
    DoOperation(&op);
    return result;
}

bool
ProcessMonitor::WriteRegisterSet(lldb::tid_t tid, void *buf, size_t buf_size, unsigned int regset)
{
    bool result;
    WriteRegisterSetOperation op(tid, buf, buf_size, regset, result);
    DoOperation(&op);
    return result;
}

bool
ProcessMonitor::ReadThreadPointer(lldb::tid_t tid, lldb::addr_t &value)
{
    bool result;
    ReadThreadPointerOperation op(tid, &value, result);
    DoOperation(&op);
    return result;
}

bool
ProcessMonitor::Resume(lldb::tid_t tid, uint32_t signo)
{
    bool result;
    Log *log (ProcessPOSIXLog::GetLogIfAllCategoriesSet (POSIX_LOG_PROCESS));

    if (log)
        log->Printf ("ProcessMonitor::%s() resuming thread = %"  PRIu64 " with signal %s", __FUNCTION__, tid,
                                 m_process->GetUnixSignals().GetSignalAsCString (signo));
    ResumeOperation op(tid, signo, result);
    DoOperation(&op);
    if (log)
        log->Printf ("ProcessMonitor::%s() resuming result = %s", __FUNCTION__, result ? "true" : "false");
    return result;
}

bool
ProcessMonitor::SingleStep(lldb::tid_t tid, uint32_t signo)
{
    bool result;
    SingleStepOperation op(tid, signo, result);
    DoOperation(&op);
    return result;
}

bool
ProcessMonitor::Kill()
{
    return kill(GetPID(), SIGKILL) == 0;
}

bool
ProcessMonitor::GetSignalInfo(lldb::tid_t tid, void *siginfo, int &ptrace_err)
{
    bool result;
    SiginfoOperation op(tid, siginfo, result, ptrace_err);
    DoOperation(&op);
    return result;
}

bool
ProcessMonitor::GetEventMessage(lldb::tid_t tid, unsigned long *message)
{
    bool result;
    EventMessageOperation op(tid, message, result);
    DoOperation(&op);
    return result;
}

lldb_private::Error
ProcessMonitor::Detach(lldb::tid_t tid)
{
    lldb_private::Error error;
    if (tid != LLDB_INVALID_THREAD_ID)
    {
        DetachOperation op(tid, error);
        DoOperation(&op);
    }
    return error;
}

bool
ProcessMonitor::DupDescriptor(const FileSpec &file_spec, int fd, int flags)
{
    int target_fd = open(file_spec.GetCString(), flags, 0666);

    if (target_fd == -1)
        return false;

    if (dup2(target_fd, fd) == -1)
        return false;

    return (close(target_fd) == -1) ? false : true;
}

void
ProcessMonitor::StopMonitoringChildProcess()
{
    if (m_monitor_thread.IsJoinable())
    {
        ::pthread_kill(m_monitor_thread.GetNativeThread().GetSystemHandle(), SIGUSR1);
        m_monitor_thread.Join(nullptr);
    }
}

void
ProcessMonitor::StopMonitor()
{
    StopMonitoringChildProcess();
    StopOpThread();
    sem_destroy(&m_operation_pending);
    sem_destroy(&m_operation_done);
    if (m_terminal_fd >= 0) {
        close(m_terminal_fd);
        m_terminal_fd = -1;
    }
}

void
ProcessMonitor::StopOpThread()
{
    if (!m_operation_thread.IsJoinable())
        return;

    DoOperation(EXIT_OPERATION);
    m_operation_thread.Join(nullptr);
}
