/* 
Copyright (C) 2014 Jonathon Ogden   < jeog.dev@gmail.com >

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see http://www.gnu.org/licenses.
*/

#include "tos_databridge.h"
#include "concurrency.hpp"
#include "ipc.hpp"

#include <fstream>
#include <iomanip>
#include <algorithm>


bool 
IPCBase::send(std::string msg) const
{
    DWORD d; 
    BOOL ret;

    if(msg.length() > MAX_MESSAGE_SZ){
        TOSDB_LogH("IPC", "IPCBase::send() :: msg length > MAX_MESSAGE_SZ");
        throw std::invalid_argument("IPCBase::send() :: msg length > MAX_MESSAGE_SZ");
    }
    
    /* size_t to DWORD cast ok because we check msg length above */
    ret = WriteFile(_main_channel_pipe_hndl, (void*)msg.c_str(), msg.length() + 1, &d, NULL);
    if(!ret){        
        TOSDB_LogEx("IPC", "WriteFile failed in _recv()", GetLastError());  
        return false; 
    }

    return true;
}
 

bool 
IPCBase::recv(std::string *msg) const 
{    
    DWORD d; 
    BOOL ret;
    
    msg->clear();
    msg->resize(MAX_MESSAGE_SZ); 
    
    ret = ReadFile(_main_channel_pipe_hndl, (void*)msg->c_str(), MAX_MESSAGE_SZ + 1, &d, NULL);
    if(!ret || d == 0){
        TOSDB_LogEx("IPC", "ReadFile failed in _recv()", GetLastError());
        return false;
    }

    return true;
}

// needs to be non-blocking, called from ::call() which is called from DLLMain
template<typename T>
bool 
IPCBase::_call_pipe( std::string name,
                     T* in,
                     DWORD in_sz,
                     T* out, 
                     DWORD out_sz,
                     unsigned long timeout, 
                     std::function<bool(T*,DWORD)> handle_good_call,
                     std::function<void(void)> handle_file_not_found ) 
{     
    errno_t e;
    DWORD r = 0;  
   
    if( CallNamedPipe(name.c_str(), (void*)in, in_sz, (void*)out, out_sz, &r, timeout) )
    {    
        return handle_good_call(out, r);                 
    }

    e = GetLastError(); 
    if(e == ERROR_FILE_NOT_FOUND){
        if(handle_file_not_found){
            handle_file_not_found();
        }
    }else{
        TOSDB_LogRawH("IPC", ("CallNamedPipe failed for pipe: " + name + 
                              ", ERROR# " + std::to_string(e)).c_str() );
    }

    return false;     
}

template bool /* connected() / _probe_channel_pipe */
IPCBase::_call_pipe<uint8_t>(std::string, uint8_t*, DWORD, uint8_t*, DWORD, unsigned long, 
                             std::function<bool(uint8_t*,DWORD)>, std::function<void(void)>);

template bool /* call() / _main_channel_pipe */
IPCBase::_call_pipe<const char>(std::string, const char*, DWORD, const char*, DWORD, unsigned long, 
                                std::function<bool(const char*,DWORD)>, std::function<void(void)>);


bool 
IPCBase::connected(unsigned long timeout) 
{ 
    uint8_t i = PROBE_BYTE;
    uint8_t o = 0;  

    /* check for a single byte that == PROBE_BYTE */
    std::function<bool(uint8_t*,DWORD)> cb = 
        [](uint8_t *out, DWORD r){ 
            if (r != sizeof(PROBE_BYTE) ){
                TOSDB_LogRawH("IPC", ("bad probe size returned: " + std::to_string(r)).c_str());
                return false;
            }
            if( *out != PROBE_BYTE ){
                TOSDB_LogRawH("IPC", ("bad probe value returned: " + std::to_string(*out)).c_str());
                if( *out == PROBE_BYTE_WRONG_ARCH ){
                    TOSDB_LogRawH("IPC", "build mismatch between engine and library(x86 vs x64)");                
                }           
                return false;
            }
            return true;
        };
  
    return _call_pipe(_probe_channel_pipe_name, &i, sizeof(i), &o, sizeof(o), timeout, cb); 
}


// needs to be non-blocking, called from DLLMain
bool
IPCMaster::call(std::string *msg, unsigned long timeout)
{  
    /* buffer for returned string */
    std::string recv(MAX_MESSAGE_SZ, '\0');  

    /* check buffer length */
    if(msg->length() > MAX_MESSAGE_SZ){
        TOSDB_LogRawH("IPC", "IPCMaster::call() :: msg length > MAX_MESSAGE_SZ");
        throw std::invalid_argument("IPCMaster::call() :: msg length > MAX_MESSAGE_SZ");
    } 

    /* assign the receive buffer*/
    std::function<bool(const char*,DWORD)> cb_good = 
        [msg,&recv](const char* d1, DWORD d2){ *msg = recv; return true; };

    /* if we get here the main pipe *should* be available; log if not */
    std::function<void(void)> cb_no_file = 
        [](){ TOSDB_LogRawH("IPC", "main pipe not found (slave not available)"); };
       
    /* size_t to DWORD cast ok because we check msg length above */
    return _call_pipe( _main_channel_pipe_name, 
                       msg->c_str(), msg->length() + 1, 
                       recv.c_str(), recv.length() + 1, 
                       timeout, cb_good, cb_no_file ); 
}


HANDLE
IPCSlave::_create_pipe(std::string name)
{
    HANDLE h = CreateNamedPipe(name.c_str(), PIPE_ACCESS_DUPLEX,
                               PIPE_TYPE_MESSAGE | PIPE_READMODE_BYTE | PIPE_WAIT,
                               1, 0, 0, NMPWAIT_NOWAIT, &_sec_attr);    
     
    if(h == INVALID_HANDLE_VALUE){        
        errno_t e = GetLastError();
        std::string msg = "IPCSlave failed to create pipe: " + name + " ("  + std::to_string(e) + ")";
        TOSDB_LogEx("IPC-Slave", msg.c_str(), e);        
        throw std::runtime_error(msg);
    }

    return h;
}


bool 
IPCSlave::wait_for_master()
{    
    if( !ConnectNamedPipe(_main_channel_pipe_hndl, NULL) )
    {       
        TOSDB_LogEx("IPC-Slave", "ConnectNamedPipe failed in wait_for_master", GetLastError());        
        return false;
    }
    return true;
}


void
IPCSlave::_listen_for_probes()
{
    BOOL ret;
    DWORD r;
    uint8_t b;
    errno_t e;          

    while(_probe_channel_run_flag){ 
     /* Main run loop for the probe channel: a very simple named pipe server that 
        allows clients (master AND slave objects) to share a single pipe instance 
        in order to send a PROBE_BYTE. If the server receives a valid PROBE_BYTE it 
        returns it, disconnects the pipe instance and waits for another */

        ret = ConnectNamedPipe(_probe_channel_pipe_hndl, NULL);
        if(!ret){
            e = GetLastError();                               
            if(e != ERROR_PIPE_CONNECTED){  /* in case client connects first */
                TOSDB_LogEx("IPC", "ConnectNamedPipe failed in probe_channel", e);
                continue;
            }
        }

        b = 0;
        r = 0;
        ret = ReadFile(_probe_channel_pipe_hndl, (void*)&b, sizeof(b), &r, NULL);
        if(!ret){           
            e = GetLastError();
            if(e == ERROR_BROKEN_PIPE)
                TOSDB_LogDebug("***IPC*** PROBE RECV - (BROKEN_PIPE)");
            else
                TOSDB_LogEx("IPC", "ReadFile failed in _listen_for_probes()", e);            
        }                          
 
        if (r != sizeof(PROBE_BYTE)){
            TOSDB_LogH("IPC", ("bad probe size received: " + std::to_string(r)).c_str());
            b = 0;            
        }else if(b != PROBE_BYTE){
            TOSDB_LogH("IPC", ("bad probe value received: " + std::to_string(b)).c_str());                      
            if (b == PROBE_BYTE_WRONG_ARCH){
                TOSDB_LogH("IPC", "build mismatch between engine and library(x86 vs x64)");                
                b = PROBE_BYTE;
            }else{
                b = 0;  
            }
        }
 
        r = 0;
        ret = WriteFile(_probe_channel_pipe_hndl, (void*)&b, sizeof(b), &r, NULL);
        if(!ret || r != sizeof(PROBE_BYTE))
            TOSDB_LogEx("IPC", "WriteFile failed in _listen_for_probes()", GetLastError());

        FlushFileBuffers(_probe_channel_pipe_hndl);        
        DisconnectNamedPipe(_probe_channel_pipe_hndl);       
    } 
}


void 
IPCSlave::_init_security_objects()
{   
    std::string call_str;

    SID_NAME_USE dummy;   
    DWORD dom_sz = 128;
    DWORD sid_sz = SECURITY_MAX_SID_SIZE;
    
    SmartBuffer<char> dom_buf(dom_sz);   

    if( !LookupAccountName(NULL, "Everyone", _sec_sid.get(), &sid_sz, dom_buf.get(), &dom_sz, &dummy) ){    
        call_str = "LookupAccountName";
        goto handle_error;
    }
   
    _sec_attr.nLength = sizeof(SECURITY_ATTRIBUTES);
    _sec_attr.bInheritHandle = FALSE;
    _sec_attr.lpSecurityDescriptor = &_sec_desc;

    if( !InitializeSecurityDescriptor(&_sec_desc, SECURITY_DESCRIPTOR_REVISION) ){    
        call_str = "LookupAccountName";
        goto handle_error;
    }

    if( !SetSecurityDescriptorGroup(&_sec_desc, _sec_sid.get(), FALSE) ){    
        call_str = "SetSecurityDescriptorGroup";
        goto handle_error;
    }

    if( !InitializeAcl(_sec_acl.get(), ACL_SIZE, ACL_REVISION) ){    
        call_str = "InitializeAcl";
        goto handle_error;
    }
    
    if( !AddAccessAllowedAce(_sec_acl.get(), ACL_REVISION, FILE_GENERIC_WRITE, _sec_sid.get()) ){    
        call_str = "AddAccessAllowedAce(... FILE_GENERIC_WRITE ...)";
        goto handle_error;
    }

    if( !AddAccessAllowedAce(_sec_acl.get(), ACL_REVISION, FILE_GENERIC_READ, _sec_sid.get()) ){    
        call_str = "AddAccessAllowedAce(... FILE_GENERIC_READ ...)";;
        goto handle_error;
    }
    
    if( !SetSecurityDescriptorDacl(&_sec_desc, TRUE, _sec_acl.get(), FALSE) ){    
        call_str = "SetSecurityDescriptorDacl";
        goto handle_error;
    }    
    
    return;

    handle_error:
    {
        errno_t e = GetLastError();
        call_str.append(" failed in _init_security_objects()");
        TOSDB_LogEx("IPC-Slave", call_str.c_str(), e);
        throw std::runtime_error(call_str);
    }

}
