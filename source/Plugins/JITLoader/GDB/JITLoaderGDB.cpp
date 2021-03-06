//===-- JITLoaderGDB.cpp ----------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

// C Includes

#include "lldb/Breakpoint/Breakpoint.h"
#include "lldb/Core/PluginManager.h"
#include "lldb/Core/Log.h"
#include "lldb/Core/Module.h"
#include "lldb/Core/ModuleSpec.h"
#include "lldb/Core/Section.h"
#include "lldb/Core/StreamString.h"
#include "lldb/Symbol/SymbolContext.h"
#include "lldb/Symbol/SymbolVendor.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/SectionLoadList.h"
#include "lldb/Target/Target.h"

#include "JITLoaderGDB.h"

using namespace lldb;
using namespace lldb_private;

//------------------------------------------------------------------
// Debug Interface Structures
//------------------------------------------------------------------
typedef enum
{
    JIT_NOACTION = 0,
    JIT_REGISTER_FN,
    JIT_UNREGISTER_FN
} jit_actions_t;

#pragma pack(push, 4)
template <typename ptr_t>
struct jit_code_entry
{
    ptr_t    next_entry; // pointer
    ptr_t    prev_entry; // pointer
    ptr_t    symfile_addr; // pointer
    uint64_t symfile_size;
};
template <typename ptr_t>
struct jit_descriptor
{
    uint32_t version;
    uint32_t action_flag; // Values are jit_action_t
    ptr_t    relevant_entry; // pointer
    ptr_t    first_entry; // pointer
};
#pragma pack(pop)

JITLoaderGDB::JITLoaderGDB (lldb_private::Process *process) :
    JITLoader(process),
    m_jit_objects(),
    m_jit_break_id(LLDB_INVALID_BREAK_ID),
    m_jit_descriptor_addr(LLDB_INVALID_ADDRESS)
{
}

JITLoaderGDB::~JITLoaderGDB ()
{
    if (LLDB_BREAK_ID_IS_VALID(m_jit_break_id))
        m_process->GetTarget().RemoveBreakpointByID (m_jit_break_id);
}

void JITLoaderGDB::DidAttach()
{
    Target &target = m_process->GetTarget();
    ModuleList &module_list = target.GetImages();
    SetJITBreakpoint(module_list);
}

void JITLoaderGDB::DidLaunch()
{
    Target &target = m_process->GetTarget();
    ModuleList &module_list = target.GetImages();
    SetJITBreakpoint(module_list);
}

void
JITLoaderGDB::ModulesDidLoad(ModuleList &module_list)
{
    if (!DidSetJITBreakpoint() && m_process->IsAlive())
	SetJITBreakpoint(module_list);
}

//------------------------------------------------------------------
// Setup the JIT Breakpoint
//------------------------------------------------------------------
void
JITLoaderGDB::SetJITBreakpoint(lldb_private::ModuleList &module_list)
{
    Log *log(GetLogIfAnyCategoriesSet(LIBLLDB_LOG_JIT_LOADER));

    if ( DidSetJITBreakpoint() )
        return;

    if (log)
        log->Printf("JITLoaderGDB::%s looking for JIT register hook",
                    __FUNCTION__);

    addr_t jit_addr = GetSymbolAddress(module_list,
                                       ConstString("__jit_debug_register_code"),
                                       eSymbolTypeAny);
    if (jit_addr == LLDB_INVALID_ADDRESS)
        return;

    m_jit_descriptor_addr = GetSymbolAddress(module_list,
                                             ConstString("__jit_debug_descriptor"),
                                             eSymbolTypeData);
    if (m_jit_descriptor_addr == LLDB_INVALID_ADDRESS)
    {
        if (log)
            log->Printf(
                "JITLoaderGDB::%s failed to find JIT descriptor address",
                __FUNCTION__);
        return;
    }

    if (log)
        log->Printf("JITLoaderGDB::%s setting JIT breakpoint",
                    __FUNCTION__);

    Breakpoint *bp =
        m_process->GetTarget().CreateBreakpoint(jit_addr, true, false).get();
    bp->SetCallback(JITDebugBreakpointHit, this, true);
    bp->SetBreakpointKind("jit-debug-register");
    m_jit_break_id = bp->GetID();

    ReadJITDescriptor(true);
}

bool
JITLoaderGDB::JITDebugBreakpointHit(void *baton,
                                    StoppointCallbackContext *context,
                                    user_id_t break_id, user_id_t break_loc_id)
{
    Log *log(GetLogIfAnyCategoriesSet(LIBLLDB_LOG_JIT_LOADER));
    if (log)
        log->Printf("JITLoaderGDB::%s hit JIT breakpoint",
                    __FUNCTION__);
    JITLoaderGDB *instance = static_cast<JITLoaderGDB *>(baton);
    return instance->ReadJITDescriptor(false);
}

static void updateSectionLoadAddress(const SectionList &section_list,
                                     Target &target,
                                     uint64_t symbolfile_addr,
                                     uint64_t symbolfile_size,
                                     uint64_t &vmaddrheuristic,
                                     uint64_t &min_addr,
                                     uint64_t &max_addr)
{
    const uint32_t num_sections = section_list.GetSize();
    for (uint32_t i = 0; i<num_sections; ++i)
    {
        SectionSP section_sp(section_list.GetSectionAtIndex(i));
        if (section_sp)
        {
            if(section_sp->IsFake()) {
                uint64_t lower = (uint64_t)-1;
                uint64_t upper = 0;
                updateSectionLoadAddress(section_sp->GetChildren(), target, symbolfile_addr, symbolfile_size, vmaddrheuristic,
                    lower, upper);
                if (lower < min_addr)
                    min_addr = lower;
                if (upper > max_addr)
                    max_addr = upper;
                const lldb::addr_t slide_amount = lower - section_sp->GetFileAddress();
                section_sp->Slide(slide_amount, false);
                section_sp->GetChildren().Slide(-slide_amount, false);
                section_sp->SetByteSize (upper - lower);
            } else {
                vmaddrheuristic += 2<<section_sp->GetLog2Align();
                uint64_t lower;
                if (section_sp->GetFileAddress() > vmaddrheuristic)
                    lower = section_sp->GetFileAddress();
                else {
                    lower = symbolfile_addr+section_sp->GetFileOffset();
                    section_sp->SetFileAddress(symbolfile_addr+section_sp->GetFileOffset());
                }
                target.SetSectionLoadAddress(section_sp, lower, true);
                uint64_t upper = lower + section_sp->GetByteSize();
                if (lower < min_addr)
                    min_addr = lower;
                if (upper > max_addr)
                    max_addr = upper;
                // This is an upper bound, but a good enough heuristic
                vmaddrheuristic += section_sp->GetByteSize();
            }
        }
    }
}

bool
JITLoaderGDB::ReadJITDescriptor(bool all_entries)
{
    Target &target = m_process->GetTarget();
    if (target.GetArchitecture().GetAddressByteSize() == 8)
        return ReadJITDescriptorImpl<uint64_t>(all_entries);
    else
        return ReadJITDescriptorImpl<uint32_t>(all_entries);
}

template <typename ptr_t>
bool
JITLoaderGDB::ReadJITDescriptorImpl(bool all_entries)
{
    if (m_jit_descriptor_addr == LLDB_INVALID_ADDRESS)
        return false;

    Log *log(GetLogIfAnyCategoriesSet(LIBLLDB_LOG_JIT_LOADER));
    Target &target = m_process->GetTarget();
    ModuleList &module_list = target.GetImages();

    jit_descriptor<ptr_t> jit_desc;
    const size_t jit_desc_size = sizeof(jit_desc);
    Error error;
    size_t bytes_read = m_process->DoReadMemory(
        m_jit_descriptor_addr, &jit_desc, jit_desc_size, error);
    if (bytes_read != jit_desc_size || !error.Success())
    {
        if (log)
            log->Printf("JITLoaderGDB::%s failed to read JIT descriptor",
                        __FUNCTION__);
        return false;
    }

    jit_actions_t jit_action = (jit_actions_t)jit_desc.action_flag;
    addr_t jit_relevant_entry = (addr_t)jit_desc.relevant_entry;
    if (all_entries)
    {
        jit_action = JIT_REGISTER_FN;
        jit_relevant_entry = (addr_t)jit_desc.first_entry;
    }

    while (jit_relevant_entry != 0)
    {
        jit_code_entry<ptr_t> jit_entry;
        const size_t jit_entry_size = sizeof(jit_entry);
        bytes_read = m_process->DoReadMemory(jit_relevant_entry, &jit_entry, jit_entry_size, error);
        if (bytes_read != jit_entry_size || !error.Success())
        {
            if (log)
                log->Printf(
                    "JITLoaderGDB::%s failed to read JIT entry at 0x%" PRIx64,
                    __FUNCTION__, jit_relevant_entry);
            return false;
        }

        const addr_t &symbolfile_addr = (addr_t)jit_entry.symfile_addr;
        const size_t &symbolfile_size = (size_t)jit_entry.symfile_size;
        ModuleSP module_sp;

        if (jit_action == JIT_REGISTER_FN)
        {
            if (log)
                log->Printf(
                    "JITLoaderGDB::%s registering JIT entry at 0x%" PRIx64
                    " (%" PRIu64 " bytes)",
                    __FUNCTION__, symbolfile_addr, (uint64_t) symbolfile_size);

            char jit_name[64];
            snprintf(jit_name, 64, "JIT(0x%" PRIx64 ")", symbolfile_addr);
            module_sp = m_process->ReadModuleFromMemory(
                FileSpec(jit_name, false), symbolfile_addr, symbolfile_size);

            if (module_sp && module_sp->GetObjectFile())
            {
                bool changed;
                m_jit_objects.insert(std::make_pair(symbolfile_addr, module_sp));
                if (module_sp->GetObjectFile()->GetPluginName() == ConstString("mach-o"))
                {
                    ObjectFile *image_object_file = module_sp->GetObjectFile();
                    if (image_object_file)
                    {
                        const SectionList *section_list = image_object_file->GetSectionList ();
                        if (section_list)
                        {
                            uint64_t vmaddrheuristic = 0;
                            uint64_t lower = (uint64_t)-1;
                            uint64_t upper = 0;
                            updateSectionLoadAddress(*section_list, target, symbolfile_addr, symbolfile_size,
                                vmaddrheuristic, lower, upper);
                        }
                    }
                }
                else
                {
                    module_sp->SetLoadAddress(target, 0, true, changed);
                }

                // load the symbol table right away
                module_sp->GetObjectFile()->GetSymtab();

                module_list.AppendIfNeeded(module_sp);

                ModuleList module_list;
                module_list.Append(module_sp);
                target.ModulesDidLoad(module_list);
            }
            else
            {
                if (log)
                    log->Printf("JITLoaderGDB::%s failed to load module for "
                                "JIT entry at 0x%" PRIx64,
                                __FUNCTION__, symbolfile_addr);
            }
        }
        else if (jit_action == JIT_UNREGISTER_FN)
        {
            if (log)
                log->Printf(
                    "JITLoaderGDB::%s unregistering JIT entry at 0x%" PRIx64,
                    __FUNCTION__, symbolfile_addr);

            JITObjectMap::iterator it = m_jit_objects.find(symbolfile_addr);
            if (it != m_jit_objects.end())
            {
                module_sp = it->second;
                ObjectFile *image_object_file = module_sp->GetObjectFile();
                if (image_object_file)
                {
                    const SectionList *section_list = image_object_file->GetSectionList ();
                    if (section_list)
                    {
                        const uint32_t num_sections = section_list->GetSize();
                        for (uint32_t i = 0; i<num_sections; ++i)
                        {
                            SectionSP section_sp(section_list->GetSectionAtIndex(i));
                            if (section_sp)
                            {
                                target.GetSectionLoadList().SetSectionUnloaded (section_sp);
                            }
                        }
                    }
                }
                module_list.Remove(module_sp);
                m_jit_objects.erase(it);
            }
        }
        else if (jit_action == JIT_NOACTION)
        {
            // Nothing to do
        }
        else
        {
            assert(false && "Unknown jit action");
        }

        if (all_entries)
            jit_relevant_entry = (addr_t)jit_entry.next_entry;
        else
            jit_relevant_entry = 0;
    }

    return false; // Continue Running.
}

//------------------------------------------------------------------
// PluginInterface protocol
//------------------------------------------------------------------
lldb_private::ConstString
JITLoaderGDB::GetPluginNameStatic()
{
    static ConstString g_name("gdb");
    return g_name;
}

JITLoaderSP
JITLoaderGDB::CreateInstance(Process *process, bool force)
{
    JITLoaderSP jit_loader_sp;
    ArchSpec arch (process->GetTarget().GetArchitecture());
    if (arch.GetTriple().getVendor() != llvm::Triple::Apple)
        jit_loader_sp.reset(new JITLoaderGDB(process));
    return jit_loader_sp;
}

const char *
JITLoaderGDB::GetPluginDescriptionStatic()
{
    return "JIT loader plug-in that watches for JIT events using the GDB interface.";
}

lldb_private::ConstString
JITLoaderGDB::GetPluginName()
{
    return GetPluginNameStatic();
}

uint32_t
JITLoaderGDB::GetPluginVersion()
{
    return 1;
}

void
JITLoaderGDB::Initialize()
{
    PluginManager::RegisterPlugin (GetPluginNameStatic(),
                                   GetPluginDescriptionStatic(),
                                   CreateInstance);
}

void
JITLoaderGDB::Terminate()
{
    PluginManager::UnregisterPlugin (CreateInstance);
}

bool
JITLoaderGDB::DidSetJITBreakpoint() const
{
    return LLDB_BREAK_ID_IS_VALID(m_jit_break_id);
}

addr_t
JITLoaderGDB::GetSymbolAddress(ModuleList &module_list, const ConstString &name,
                               SymbolType symbol_type) const
{
    SymbolContextList target_symbols;
    Target &target = m_process->GetTarget();

    if (!module_list.FindSymbolsWithNameAndType(name, symbol_type,
                                                target_symbols))
        return LLDB_INVALID_ADDRESS;

    SymbolContext sym_ctx;
    target_symbols.GetContextAtIndex(0, sym_ctx);

    const Address *jit_descriptor_addr = &sym_ctx.symbol->GetAddress();
    if (!jit_descriptor_addr || !jit_descriptor_addr->IsValid())
        return LLDB_INVALID_ADDRESS;

    const addr_t jit_addr = jit_descriptor_addr->GetLoadAddress(&target);
    return jit_addr;
}
